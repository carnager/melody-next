// SPDX-License-Identifier: GPL-3.0-only

#include "bench/engine_playback.hpp"

#include "trackknife/protocol/message.hpp"

#include <QMetaObject>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

#include <utility>

namespace trackknife::bench {

EnginePlayback::EnginePlayback(const CatalogueSource& catalogues, QObject* parent)
    : QObject(parent) {
    pool_.setMaxThreadCount(1);
    if (!catalogues.usingEngine()) {
        return;
    }
    auto client = protocol::Client::connect(catalogues.socket());
    if (!client) {
        return;
    }
    client_ = std::move(*client);

    const QPointer self{this};
    client_->on_event([self, this](const protocol::Event& event) {
        if (event.name != "playback.changed") {
            return;
        }
        // Parsed here, on the reader thread, so the signal carries nothing
        // that needs decoding on the UI thread.
        adopt(event.data);
        if (self) {
            QMetaObject::invokeMethod(
                self,
                [self] {
                    if (self) {
                        emit self->changed();
                    }
                },
                Qt::QueuedConnection);
        }
    });

    // Ask once, so the workspace is correct before the first event arrives.
    if (auto answer = client_->call("playback.state")) {
        adopt(*answer);
    }

    position_timer_ = new QTimer(this);
    position_timer_->setInterval(500);
    connect(position_timer_, &QTimer::timeout, this, [this] {
        const auto status = state().status;
        if (status != QStringLiteral("playing") && status != QStringLiteral("loading")) {
            return;
        }
        send(QStringLiteral("playback.state"), protocol::Json::object());
    });
    position_timer_->start();
}

EnginePlayback::~EnginePlayback() {
    // Before the client goes: a queued command would otherwise call through a
    // destroyed connection.
    pool_.waitForDone();
    if (client_) {
        client_->close();
    }
}

void EnginePlayback::adopt(const protocol::Json& payload) {
    const std::lock_guard guard{mutex_};
    state_.status = QString::fromStdString(payload.value("status", std::string{"stopped"}));
    state_.entry = payload.contains("entry") && payload.at("entry").is_string()
                       ? QString::fromStdString(payload.at("entry").get<std::string>())
                       : QString{};
    state_.path.clear();
    if (payload.contains("path") && payload.at("path").is_string()) {
        // A raw path is bytes, so it travels base64 (ADR-0222). Decoded here
        // rather than at every use, and a path this client cannot represent
        // is left empty rather than shown mangled.
        if (auto decoded = protocol::decode_raw_path(payload.at("path").get<std::string>())) {
            state_.path =
                QString::fromLocal8Bit(decoded->data(), static_cast<qsizetype>(decoded->size()));
        }
    }
    state_.position_ms = payload.value("position_ms", qint64{0});
    state_.duration_ms = payload.value("duration_ms", qint64{-1});
    state_.queue_size = payload.value("queue_size", std::size_t{0});
    state_.requests = payload.value("requests", std::size_t{0});
    state_.volume_percent = payload.value("volume_percent", 100);
    if (const auto modes = payload.find("modes"); modes != payload.end() && modes->is_object()) {
        state_.repeat = modes->value("repeat", false);
        state_.random = modes->value("random", false);
    }
}

EnginePlayback::State EnginePlayback::state() const {
    const std::lock_guard guard{mutex_};
    return state_;
}

void EnginePlayback::send(const QString& method, protocol::Json params) {
    std::vector<std::pair<QString, protocol::Json>> one;
    one.emplace_back(method, std::move(params));
    send(std::move(one));
}

void EnginePlayback::send(std::vector<std::pair<QString, protocol::Json>> calls) {
    if (!client_ || calls.empty()) {
        return;
    }
    // Off the UI thread: a call blocks on a round trip, and a transport
    // button must not wait for one. The answer is a state document, so it is
    // adopted rather than discarded and the UI is correct without waiting for
    // the event that follows.
    //
    // A sequence travels as one task rather than as several, so nothing can be
    // interleaved between a queue and the play that names it.
    const QPointer self{this};
    static_cast<void>(QtConcurrent::run(&pool_, [self, this, calls = std::move(calls)] {
        if (!self || client_ == nullptr) {
            return;
        }
        bool adopted = false;
        for (const auto& [method, params] : calls) {
            auto answer = client_->call(method.toStdString(), params);
            if (!answer) {
                continue;
            }
            adopt(*answer);
            adopted = true;
        }
        if (!adopted || !self) {
            return;
        }
        QMetaObject::invokeMethod(
            self,
            [self] {
                if (self) {
                    emit self->changed();
                }
            },
            Qt::QueuedConnection);
    }));
}

void EnginePlayback::play(const std::vector<LocalTrackRow>& rows,
                          const std::vector<std::optional<formats::ReplayGainInfo>>& overrides,
                          const core::StableId& entry) {
    auto entries = protocol::Json::array();
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows[index];
        protocol::Json item = protocol::Json::object();
        item["path"] = protocol::encode_raw_path(row.raw_path);
        // ADR-0221: the row's own identity, so the engine's queue and this
        // model agree about which entry is which with no second mapping.
        item["entry"] = row.entry_id.to_string();
        if (row.duration_ms) {
            item["duration_ms"] = *row.duration_ms;
        }
        if (index < overrides.size() && overrides[index]) {
            const auto& gain = *overrides[index];
            protocol::Json rendered = protocol::Json::object();
            const auto number = [&rendered](const char* member,
                                            const std::optional<double>& value) {
                if (value) {
                    rendered[member] = *value;
                }
            };
            number("track_gain_db", gain.track_gain_db);
            number("track_peak", gain.track_peak);
            number("album_gain_db", gain.album_gain_db);
            number("album_peak", gain.album_peak);
            item["replay_gain"] = std::move(rendered);
        }
        entries.push_back(std::move(item));
    }
    std::vector<std::pair<QString, protocol::Json>> calls;
    calls.emplace_back(QStringLiteral("playback.replace_queue"),
                       protocol::Json{{"entries", std::move(entries)}});
    calls.emplace_back(QStringLiteral("playback.play"),
                       protocol::Json{{"entry", entry.to_string()}});
    send(std::move(calls));
}

void EnginePlayback::resume() { send(QStringLiteral("playback.resume"), protocol::Json::object()); }

void EnginePlayback::pause() { send(QStringLiteral("playback.pause"), protocol::Json::object()); }

void EnginePlayback::stop() { send(QStringLiteral("playback.stop"), protocol::Json::object()); }

void EnginePlayback::next() { send(QStringLiteral("playback.next"), protocol::Json::object()); }

void EnginePlayback::previous() {
    send(QStringLiteral("playback.previous"), protocol::Json::object());
}

void EnginePlayback::seek(const qint64 position_ms) {
    send(QStringLiteral("playback.seek"), protocol::Json{{"position_ms", position_ms}});
}

void EnginePlayback::request(const core::StableId& entry) {
    send(QStringLiteral("playback.request"), protocol::Json{{"entry", entry.to_string()}});
}

void EnginePlayback::setVolume(const int percent) {
    send(QStringLiteral("playback.set_volume"), protocol::Json{{"percent", percent}});
}

void EnginePlayback::setModes(const bool repeat, const bool random) {
    send(QStringLiteral("playback.set_modes"),
         protocol::Json{{"repeat", repeat}, {"random", random}});
}

} // namespace trackknife::bench
