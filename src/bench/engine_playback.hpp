// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/catalogue_source.hpp"
#include "bench/local_list_model.hpp"
#include "trackknife/protocol/client.hpp"

#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QTimer>

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

    [[nodiscard]] bool active() const noexcept { return client_ != nullptr; }

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
        int volume_percent{100};
    };
    [[nodiscard]] State state() const;

    // Hands the engine a queue and starts one of its entries. The rows carry
    // their own identities (ADR-0221), so the engine's queue and the model
    // agree about which entry is which without a second mapping.
    void play(const std::vector<LocalTrackRow>& rows, const core::StableId& entry);

    void resume();
    void pause();
    void stop();
    void next();
    void previous();
    void seek(qint64 position_ms);
    void request(const core::StableId& entry);
    void setModes(bool repeat, bool random);
    void setVolume(int percent);

  signals:
    // The engine's state changed. Emitted on this object's thread.
    void changed();

  private:
    // One worker, so commands reach the socket in the order they were made.
    // Two threads racing would let playback.play arrive before the queue it
    // names -- and the engine, serving one connection in order, would faithfully
    // execute the wrong sequence.
    void send(std::vector<std::pair<QString, protocol::Json>> calls);
    void send(const QString& method, protocol::Json params);
    void adopt(const protocol::Json& payload);

    std::unique_ptr<protocol::Client> client_;
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
