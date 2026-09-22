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
    socket_ = catalogues.socket();
    static_cast<void>(open());

    // An engine is a separate process with its own lifetime: it can be
    // restarted, or started after the window. Without this the only way back
    // is to restart the window.
    reconnect_timer_ = new QTimer(this);
    reconnect_timer_->setInterval(3'000);
    connect(reconnect_timer_, &QTimer::timeout, this, [this] { maintain(); });
    reconnect_timer_->start();

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

bool EnginePlayback::open() {
    auto client = protocol::Client::connect(socket_);
    if (!client) {
        return false;
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

    // Asked once, so the workspace is correct before the first event arrives.
    if (auto answer = client_->call("playback.state")) {
        adopt(*answer);
    }
    return true;
}

void EnginePlayback::maintain() {
    if (client_ && client_->connected()) {
        return;
    }
    if (client_) {
        // Calls on a dead connection fail rather than block, so this does not
        // hold the UI thread.
        pool_.waitForDone();
        client_->close();
        client_.reset();
        {
            const std::lock_guard guard{mutex_};
            state_ = State{};
        }
        emit changed();
    }
    if (!open()) {
        return;
    }
    emit connected();
    emit changed();
}

bool EnginePlayback::active() const { return client_ != nullptr && client_->connected(); }

std::vector<LocalTrackRow> EnginePlayback::queueEntries() const {
    if (!client_) {
        return {};
    }
    auto answer = client_->call("playback.queue");
    if (!answer || !answer->contains("entries")) {
        return {};
    }
    std::vector<LocalTrackRow> rows;
    for (const auto& item : answer->at("entries")) {
        auto decoded = protocol::decode_raw_path(item.value("path", std::string{}));
        if (!decoded) {
            continue;
        }
        LocalTrackRow row;
        row.raw_path = std::move(*decoded);
        // The engine's identity, not a fresh one: the entry the engine says it
        // is playing has to be findable in this list.
        if (const auto identity = item.find("entry");
            identity != item.end() && identity->is_string()) {
            if (auto parsed = core::StableId::parse(identity->get<std::string>())) {
                row.entry_id = *parsed;
            }
        }
        if (const auto duration = item.find("duration_ms");
            duration != item.end() && duration->is_number_integer()) {
            const auto value = duration->get<std::int64_t>();
            if (value >= 0) {
                row.duration_ms = value;
            }
        }
        if (const auto selection = item.find("selection");
            selection != item.end() && selection->is_object()) {
            if (const auto stream = selection->find("stream_index");
                stream != selection->end() && stream->is_number_integer()) {
                row.selection.stream_index = stream->get<int>();
            }
            if (const auto subsong = selection->find("subsong_index");
                subsong != selection->end() && subsong->is_number_integer()) {
                row.selection.subsong_index = subsong->get<int>();
            }
        }
        if (const auto segment = item.find("segment");
            segment != item.end() && segment->is_object()) {
            formats::SampleRange range;
            range.start_sample = segment->value("start_sample", std::int64_t{0});
            if (const auto end = segment->find("end_sample");
                end != segment->end() && end->is_number_integer()) {
                range.end_sample = end->get<std::int64_t>();
            }
            row.segment = range;
        }
        rows.push_back(std::move(row));
    }
    return rows;
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
    state_.instance = payload.value("instance", std::uint64_t{0});
    state_.consumed = payload.contains("consumed") && payload.at("consumed").is_string()
                          ? QString::fromStdString(payload.at("consumed").get<std::string>())
                          : QString{};
    if (const auto modes = payload.find("modes"); modes != payload.end() && modes->is_object()) {
        state_.modes.repeat = modes->value("repeat", false);
        state_.modes.random = modes->value("random", false);
        state_.modes.album_random = modes->value("album_random", false);
        state_.modes.single = audio::mode_state_from_int(modes->value("single", 0));
        state_.modes.consume = audio::mode_state_from_int(modes->value("consume", 0));
    }
    state_.replay_gain_mode = audio::ReplayGainMode::off;
    if (const auto gain = payload.find("replay_gain"); gain != payload.end() && gain->is_object()) {
        const auto name = gain->value("mode", std::string{"off"});
        if (name == "track") {
            state_.replay_gain_mode = audio::ReplayGainMode::track;
        } else if (name == "album") {
            state_.replay_gain_mode = audio::ReplayGainMode::album;
        }
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

protocol::Json EnginePlayback::entryJson(const LocalTrackRow& row,
                                         const std::optional<formats::ReplayGainInfo>& gain) {
    protocol::Json item = protocol::Json::object();
    item["path"] = protocol::encode_raw_path(row.raw_path);
    // ADR-0221: the row's own identity, so the engine's queue and this
    // model agree about which entry is which with no second mapping.
    item["entry"] = row.entry_id.to_string();
    if (row.duration_ms) {
        item["duration_ms"] = *row.duration_ms;
    }
    // Which audio in the container, and which range of it. A CUE album is one
    // file and many segments, so an entry without these plays the whole file
    // from the start.
    if (row.selection.stream_index || row.selection.subsong_index) {
        protocol::Json selection = protocol::Json::object();
        if (row.selection.stream_index) {
            selection["stream_index"] = *row.selection.stream_index;
        }
        if (row.selection.subsong_index) {
            selection["subsong_index"] = *row.selection.subsong_index;
        }
        item["selection"] = std::move(selection);
    }
    if (row.segment) {
        protocol::Json segment = protocol::Json::object();
        segment["start_sample"] = row.segment->start_sample;
        if (row.segment->end_sample) {
            segment["end_sample"] = *row.segment->end_sample;
        }
        item["segment"] = std::move(segment);
    }
    if (gain) {
        protocol::Json rendered = protocol::Json::object();
        const auto number = [&rendered](const char* member, const std::optional<double>& value) {
            if (value) {
                rendered[member] = *value;
            }
        };
        number("track_gain_db", gain->track_gain_db);
        number("track_peak", gain->track_peak);
        number("album_gain_db", gain->album_gain_db);
        number("album_peak", gain->album_peak);
        item["replay_gain"] = std::move(rendered);
    }
    return item;
}

void EnginePlayback::setRequests(const std::vector<LocalTrackRow>& rows,
                                 const std::vector<std::optional<formats::ReplayGainInfo>>& gains) {
    auto entries = protocol::Json::array();
    auto identities = protocol::Json::array();
    for (std::size_t index = 0; index < rows.size(); ++index) {
        entries.push_back(
            entryJson(rows[index], index < gains.size() ? gains[index] : std::nullopt));
        identities.push_back(rows[index].entry_id.to_string());
    }
    // Enqueue first: a request names an entry the engine holds, and up-next
    // can carry a track that was never in the playing list. The two travel as
    // one sequence, so the engine never sees the second without the first.
    std::vector<std::pair<QString, protocol::Json>> calls;
    calls.emplace_back(QStringLiteral("playback.enqueue"),
                       protocol::Json{{"entries", std::move(entries)}});
    calls.emplace_back(QStringLiteral("playback.set_requests"),
                       protocol::Json{{"entries", std::move(identities)}});
    send(std::move(calls));
}

void EnginePlayback::play(const std::vector<LocalTrackRow>& rows,
                          const std::vector<std::optional<formats::ReplayGainInfo>>& overrides,
                          const core::StableId& entry) {
    auto entries = protocol::Json::array();
    for (std::size_t index = 0; index < rows.size(); ++index) {
        entries.push_back(
            entryJson(rows[index], index < overrides.size() ? overrides[index] : std::nullopt));
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

void EnginePlayback::setModes(const audio::PlaybackModes& modes) {
    send(QStringLiteral("playback.set_modes"),
         protocol::Json{{"repeat", modes.repeat},
                        {"random", modes.random},
                        {"album_random", modes.album_random},
                        {"single", static_cast<int>(modes.single)},
                        {"consume", static_cast<int>(modes.consume)}});
}

void EnginePlayback::setReplayGain(const audio::ReplayGainMode mode,
                                   const audio::ReplayGainPreamps preamps) {
    const auto* name = mode == audio::ReplayGainMode::track   ? "track"
                       : mode == audio::ReplayGainMode::album ? "album"
                                                              : "off";
    send(QStringLiteral("playback.set_replay_gain"),
         protocol::Json{{"mode", name},
                        {"preamp_with_gain_db", preamps.with_gain_db},
                        {"preamp_without_gain_db", preamps.without_gain_db}});
}

} // namespace trackknife::bench
