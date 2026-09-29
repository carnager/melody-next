// SPDX-License-Identifier: GPL-3.0-only

#include "quick/player_state.hpp"

#include <algorithm>
#include <memory>

namespace trackknife::quick {

namespace {

using Json = EngineClient::Json;

[[nodiscard]] QString text(const Json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? QString::fromStdString(found->get<std::string>())
                                                       : QString{};
}

} // namespace

PlayerState::PlayerState(EngineClient& client, QObject* parent) : QObject(parent), client_(client) {
    since_.start();
    tick_.setInterval(250);
    connect(&tick_, &QTimer::timeout, this, &PlayerState::positionChanged);
    // The engine does not push position; while something plays, ask now and
    // then so the interpolation does not drift.
    poll_.setInterval(2000);
    connect(&poll_, &QTimer::timeout, this, &PlayerState::refresh);

    client_.onEvent([this](const std::string& name, const Json& data) {
        if (name == "playback.changed") {
            adopt(data);
        } else if (name == "outputs.changed") {
            adoptOutputs(data);
        }
    });
    connect(&client_, &EngineClient::connectedChanged, this, [this] {
        if (client_.connected()) {
            // A new connection counts its sequence from the start.
            sequence_ = 0;
            refresh();
            // An engine from before output agents has no list: it plays on
            // its own audio, which is what an empty list means here.
            client_.call(QStringLiteral("outputs.list"), Json::object(), [this](const auto& answer) {
                if (answer) {
                    adoptOutputs(*answer);
                }
            });
        }
    });
}

qreal PlayerState::position() const {
    auto ms = position_ms_;
    if (playing()) {
        ms += since_.elapsed();
    }
    if (duration_ms_ > 0) {
        ms = std::min(ms, duration_ms_);
    }
    return static_cast<qreal>(ms) / 1000.0;
}

void PlayerState::refresh() {
    client_.call(QStringLiteral("playback.state"), Json::object(), [this](const auto& answer) {
        if (answer) {
            adopt(*answer);
        }
    });
}

void PlayerState::adopt(const Json& state) {
    if (!state.is_object()) {
        return;
    }
    // States arrive on two paths -- events and answers -- so an older one
    // can come second. It is dropped rather than put back over a newer one.
    const auto sequence = state.value("sequence", std::uint64_t{0});
    if (sequence != 0 && sequence < sequence_) {
        return;
    }
    sequence_ = sequence;

    const auto previous_entry = entry_;
    const auto previous_revision = queue_revision_;
    status_ = text(state, "status");
    entry_ = text(state, "entry");
    path_ = text(state, "path");
    position_ms_ = state.value("position_ms", std::int64_t{0});
    since_.restart();
    duration_ms_ = std::max<std::int64_t>(0, state.value("duration_ms", std::int64_t{0}));
    volume_ = state.value("volume_percent", 100);
    requests_ = static_cast<int>(state.value("requests", std::size_t{0}));
    queue_size_ = static_cast<int>(state.value("queue_size", std::size_t{0}));
    queue_revision_ = state.value("queue_revision", std::uint64_t{0});
    if (const auto modes = state.find("modes"); modes != state.end() && modes->is_object()) {
        repeat_ = modes->value("repeat", false);
        random_ = modes->value("random", false);
        single_ = modes->value("single", 0);
        consume_ = modes->value("consume", 0);
    }
    if (const auto gain = state.find("replay_gain"); gain != state.end() && gain->is_object()) {
        replay_gain_ = text(*gain, "mode");
    }
    error_ = text(state, "error");
    if (const auto output = state.find("output"); output != state.end() && output->is_object()) {
        device_ = text(*output, "target");
        default_device_ = text(*output, "default");
        output_available_ = output->value("available", true);
        speakers_taken_by_ = text(*output, "taken_by");
        QVariantList devices;
        for (const auto& device : output->value("devices", Json::array())) {
            devices.push_back(QVariantMap{{QStringLiteral("name"), text(device, "name")},
                                          {QStringLiteral("description"), text(device, "description")}});
        }
        devices_ = std::move(devices);
    }

    if (playing()) {
        tick_.start();
        poll_.start();
    } else {
        tick_.stop();
        poll_.stop();
    }
    emit changed();
    emit positionChanged();
    if (entry_ != previous_entry) {
        fetchMetadata();
    }
    if (queue_revision_ != previous_revision) {
        emit queueChanged();
    }
}

// The engine formats what plays: it has the library's whole row, where this
// window has only what a list item summarises.
void PlayerState::fetchMetadata() {
    if (entry_.isEmpty()) {
        title_.clear();
        artist_.clear();
        album_.clear();
        date_.clear();
        emit metadataChanged();
        return;
    }
    const auto fields = std::to_array<std::pair<const char*, QString PlayerState::*>>(
        {{"%title%", &PlayerState::title_},
         {"%artist%", &PlayerState::artist_},
         {"%album%", &PlayerState::album_},
         {"%date%", &PlayerState::date_}});
    const auto asked_for = entry_;
    for (const auto& [format, member] : fields) {
        client_.call(QStringLiteral("playback.format"), Json{{"format", format}},
                     [this, asked_for, member](const auto& answer) {
                         if (!answer || entry_ != asked_for) {
                             return;
                         }
                         const auto found = answer->find("text");
                         this->*member = found != answer->end() && found->is_string()
                                             ? QString::fromStdString(found->template get<std::string>())
                                             : QString{};
                         emit metadataChanged();
                     });
    }
}

void PlayerState::adoptOutputs(const Json& payload) {
    QVariantList outputs;
    for (const auto& output : payload.value("outputs", Json::array())) {
        if (!output.is_object()) {
            continue;
        }
        outputs.push_back(QVariantMap{
            {QStringLiteral("id"), text(output, "id")},
            {QStringLiteral("name"), QString::fromStdString(protocol::displayable_text(output.value("name", std::string{})))},
            {QStringLiteral("local"), output.value("local", false)},
            {QStringLiteral("online"), output.value("online", false)},
            {QStringLiteral("selected"), output.value("selected", false)},
            {QStringLiteral("files"), output.value("files", true)}});
    }
    outputs_ = std::move(outputs);
    emit outputsChanged();
}

void PlayerState::selectOutput(const QString& id) {
    client_.command(QStringLiteral("outputs.select"), Json{{"id", id.toStdString()}});
}

void PlayerState::setDevice(const QString& name) {
    send(QStringLiteral("playback.set_output"),
         Json{{"target", name.isEmpty() ? Json(nullptr) : Json(name.toStdString())}});
}

void PlayerState::send(const QString& method, Json params) {
    client_.command(method, std::move(params), [this](const auto& answer) {
        if (answer) {
            adopt(*answer);
        }
    });
}

void PlayerState::toggle() {
    send(playing() ? QStringLiteral("playback.pause") : QStringLiteral("playback.resume"));
}

void PlayerState::stop() { send(QStringLiteral("playback.stop")); }

void PlayerState::next() { send(QStringLiteral("playback.next")); }

void PlayerState::previous() { send(QStringLiteral("playback.previous")); }

void PlayerState::seek(const qreal seconds) {
    position_ms_ = static_cast<qint64>(seconds * 1000.0);
    since_.restart();
    emit positionChanged();
    send(QStringLiteral("playback.seek"), Json{{"position_ms", position_ms_}});
}

void PlayerState::setVolume(const int percent) {
    volume_ = std::clamp(percent, 0, 100);
    send(QStringLiteral("playback.set_volume"), Json{{"percent", volume_}});
}

void PlayerState::setRepeat(const bool on) { send(QStringLiteral("playback.set_modes"), Json{{"repeat", on}}); }

void PlayerState::setRandom(const bool on) { send(QStringLiteral("playback.set_modes"), Json{{"random", on}}); }

void PlayerState::setSingle(const int state) {
    send(QStringLiteral("playback.set_modes"), Json{{"single", std::clamp(state, 0, 2)}});
}

void PlayerState::setConsume(const int state) {
    send(QStringLiteral("playback.set_modes"), Json{{"consume", std::clamp(state, 0, 2)}});
}

void PlayerState::cycleReplayGain() {
    const auto next = replay_gain_ == QStringLiteral("off")     ? "track"
                      : replay_gain_ == QStringLiteral("track") ? "album"
                                                                : "off";
    send(QStringLiteral("playback.set_replay_gain"), Json{{"mode", next}});
}

} // namespace trackknife::quick
