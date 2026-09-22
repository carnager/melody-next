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

#include <filesystem>
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
        bool repeat{false};
        bool random{false};
        audio::ReplayGainMode replay_gain_mode{audio::ReplayGainMode::off};
        int volume_percent{100};
        // Which playback this is: replaying a track is a new instance, and
        // crediting a listen has to tell those apart.
        quint64 instance{0};
    };
    [[nodiscard]] State state() const;

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

  signals:
    // The engine's state changed. Emitted on this object's thread.
    void changed();
    // A connection was established -- at startup, or again after the engine
    // was restarted. Whoever owns this hands over the settings the engine
    // cannot know and attaches to whatever it is already playing.
    void connected();

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

    std::filesystem::path socket_;
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
};

} // namespace trackknife::bench
