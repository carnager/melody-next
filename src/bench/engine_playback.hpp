// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/catalogue_source.hpp"
#include "bench/local_list_model.hpp"
#include "trackknife/audio/local_playback.hpp"
#include "trackknife/audio/playback_modes.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/protocol/client.hpp"

#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QTimer>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace trackknife::bench {

// The workspace driving an engine's playback.
//
// ADR-0220: when an engine is configured, it owns the queue, the modes, the
// order, up-next, gapless, listening and resume. The workspace stops doing all
// of that and becomes a remote control -- which is what lets it be closed
// without the music stopping.
//
// It opens its own connection rather than sharing the catalogue's. The engine
// serves one connection's requests in order, so a transport command behind a
// library query would wait for it; a second connection costs one socket and
// removes the whole question. This is the case ADR-0222 names when it says a
// client needing concurrency opens another connection.
class EnginePlayback final : public QObject {
    Q_OBJECT

  public:
    // Connects if the source has an engine. Inert otherwise, and `active()`
    // says so rather than every caller testing a pointer.
    explicit EnginePlayback(const CatalogueSource& catalogues, QObject* parent = nullptr);
    ~EnginePlayback() override;

    // Connected now, not "was connected once". An engine can be restarted
    // under a running window, and a client that never notices keeps sending
    // transport commands into a dead socket.
    [[nodiscard]] bool active() const;

    // The engine's queue, asked for rather than remembered. Used when a window
    // attaches to an engine that is already playing: the queue is the engine's
    // and this client has never seen it.
    [[nodiscard]] std::vector<LocalTrackRow> queueEntries() const;

    // What the engine last told us. Cached so painting transport does not
    // make a blocking call on the UI thread, and refreshed by events.
    struct State final {
        QString status{QStringLiteral("stopped")};
        QString entry;
        QString path;
        qint64 position_ms{0};
        qint64 duration_ms{-1};
        std::size_t queue_size{0};
        std::size_t requests{0};
        // The engine's, not this client's. A one-shot expires where playback
        // happens, so the buttons follow the engine rather than the other way
        // round once a track has gone by.
        audio::PlaybackModes modes;
        audio::ReplayGainMode replay_gain_mode{audio::ReplayGainMode::off};
        audio::ReplayGainPreamps replay_gain_preamps;
        int volume_percent{100};
        // Which playback this is: replaying a track is a new instance, and
        // crediting a listen has to tell those apart.
        quint64 instance{0};
        // Changes whenever the engine's queue, modes or asks change --
        // including when this client was not the one that changed them.
        quint64 queue_revision{0};
        // The entry the engine's consume mode last dropped, so this client
        // can drop the same row from the list it came from.
        QString consumed;
        // ADR-0226: the engine's sink and buffer. The devices are the engine
        // machine's, which is why they come from here and not from this one.
        struct Device final {
            std::string name;
            std::string description;
            friend bool operator==(const Device&, const Device&) = default;
        };
        std::optional<std::string> output_target;
        std::optional<std::string> default_output;
        bool output_available{true};
        bool output_suspended{false};
        std::vector<Device> devices;
        qint64 buffer_capacity_ms{0};
        qint64 buffer_start_threshold_ms{0};
        bool buffer_pending{false};
        quint64 underruns{0};
        // ADR-0228: what the engine can play on -- its own audio, if it has
        // any, and every output agent it knows -- and which it plays on.
        struct Output final {
            std::string id;
            std::string name;
            bool local{false};
            bool online{false};
            bool selected{false};
            // False for an agent that streams from the engine.
            bool files{true};
            friend bool operator==(const Output&, const Output&) = default;
        };
        std::vector<Output> outputs;
    };
    [[nodiscard]] State state() const;
    // Commands sent and not yet answered. While there are any, a state the
    // engine reports may predate them -- a queue without the rows just
    // added -- and is no reason to think the engine's queue has drifted.
    [[nodiscard]] bool settling() const noexcept { return in_flight_.load() > 0; }
    // Whether the engine scrobbles what it plays itself -- it has a Last.fm
    // session (lastfm.status). The window then does not, or a listen would
    // count twice. Asked on connecting, and again by refreshScrobbling().
    [[nodiscard]] bool scrobblesItself() const noexcept { return engine_scrobbles_.load(); }
    void refreshScrobbling();

    // Hands the engine a queue without starting anything: an edit to the list
    // that is playing, rather than a new thing to play.
    void replaceQueue(const std::vector<LocalTrackRow>& rows,
                      const std::vector<std::optional<formats::ReplayGainInfo>>& overrides);

    // Hands the engine a queue and starts one of its entries. The rows carry
    // their own identities (ADR-0221), so the engine's queue and the model
    // agree about which entry is which without a second mapping.
    // `overrides` is one entry per row, in the same order: the explicit
    // ReplayGain override where the client has one (ADR-0139/0141), and
    // nothing where the decoder's own tags should apply. The engine opens the
    // file and reads those for itself; what it cannot see is a sidecar value
    // or a CUE sheet's REM lines, which is why they travel.
    void play(const std::vector<LocalTrackRow>& rows,
              const std::vector<std::optional<formats::ReplayGainInfo>>& overrides,
              const core::StableId& entry);

    // The up-next order, stated rather than rebuilt one request at a time.
    // Entries not already in the engine's queue are added to it: an engine
    // only plays what it holds, and up-next can carry a track that was never
    // in the playing list.
    void setRequests(const std::vector<LocalTrackRow>& rows,
                     const std::vector<std::optional<formats::ReplayGainInfo>>& gains);

    void resume();
    void pause();
    void stop();
    void next();
    void previous();
    void seek(qint64 position_ms);
    void request(const core::StableId& entry);
    // Every mode the workspace offers, because they all decide what the
    // engine plays next. Sent together: the engine leaves absent members
    // alone, and restating the set is what makes the buttons and the engine
    // agree after a reconnect.
    void setModes(const audio::PlaybackModes& modes);
    void setVolume(int percent);
    // ADR-0138. `mode` is already resolved to off/track/album -- "auto" is
    // this client's policy about its own shuffle, not something to ask the
    // engine to interpret.
    void setReplayGain(audio::ReplayGainMode mode, audio::ReplayGainPreamps preamps);
    // Nullopt is the system default sink.
    void setOutput(const std::optional<std::string>& target);
    void refreshOutputs();
    // ADR-0228: plays on another of the engine's outputs, taking the music
    // there where it is.
    void selectOutput(const std::string& id);
    void setBuffer(qint64 capacity_ms, qint64 start_threshold_ms);

  signals:
    // The engine's state changed. Emitted on this object's thread.
    void changed();
    // A connection was established -- at startup, or again after the engine
    // was restarted. Whoever owns this hands over the settings the engine
    // cannot know and attaches to whatever it is already playing.
    void connected();
    // The engine refused a command, with its reason -- a track the output
    // could not play, say. Emitted on this object's thread.
    void failed(const QString& message);

  private:
    // Connects if one is configured. Answers whether a connection now exists.
    bool open();
    // Drops a dead connection and tries again. Cheap when connected.
    void maintain();

    [[nodiscard]] static protocol::Json
    entryJson(const LocalTrackRow& row, const std::optional<formats::ReplayGainInfo>& gain);
    // One worker, so commands reach the socket in the order they were made.
    // Two threads racing would let playback.play arrive before the queue it
    // names -- and the engine, serving one connection in order, would faithfully
    // execute the wrong sequence.
    void send(std::vector<std::pair<QString, protocol::Json>> calls);
    void send(const QString& method, protocol::Json params);
    void adopt(const protocol::Json& payload);
    void adoptOutputs(const protocol::Json& payload);

    protocol::Endpoint endpoint_;
    std::function<bool()> revive_;
    std::unique_ptr<protocol::Client> client_;
    QTimer* reconnect_timer_{nullptr};
    QThreadPool pool_;
    // The engine does not broadcast position -- it moves continuously and
    // would be a storm of events carrying nothing (ADR-0222). A client that
    // wants a live position asks, so this asks while something is playing and
    // stays silent when nothing is.
    QTimer* position_timer_{nullptr};
    mutable std::mutex mutex_;
    State state_;
    std::atomic<int> in_flight_{0};
    std::atomic_bool engine_scrobbles_{false};
};

} // namespace trackknife::bench
