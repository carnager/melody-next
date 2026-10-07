// SPDX-License-Identifier: GPL-3.0-only

// ADR-0226: what the engine followed reports, taken in -- its modes, the
// rows it consumed and started, its queue -- and said as the window shows
// it: what plays, where it sounds, and with how much buffer.

#include "workspace/workspace.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "workspace/workspace_view.hpp"

#include <QFileInfo>
#include <QSettings>
#include <QStringList>

#include <algorithm>
#include <chrono>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace trackknife::bench {
namespace {

constexpr int minimum_custom_buffer_ms = 10;
constexpr int maximum_custom_buffer_ms = 10'000;
constexpr auto buffer_profile_settings_key = "playback/buffer-profile";
constexpr auto buffer_capacity_settings_key = "playback/buffer-capacity-ms";
constexpr auto buffer_threshold_settings_key = "playback/buffer-start-threshold-ms";

} // namespace

QString Workspace::bufferProfileLabel(const QString& profile) {
    if (profile == QStringLiteral("responsive")) {
        return QStringLiteral("Responsive");
    }
    if (profile == QStringLiteral("resilient")) {
        return QStringLiteral("Resilient");
    }
    if (profile == QStringLiteral("custom")) {
        return QStringLiteral("Custom");
    }
    return QStringLiteral("Balanced");
}

Workspace::PlaybackBufferPreference Workspace::loadPlaybackBufferPreference() {
    QSettings settings;
    const auto profile =
        settings.value(QString::fromLatin1(buffer_profile_settings_key), QStringLiteral("balanced"))
            .toString();
    const auto profile_bytes = utf8Bytes(profile);
    if (const auto preset = audio::playback_buffer_preset_from_id(profile_bytes)) {
        return {.profile = profile, .config = audio::playback_buffer_preset_config(*preset)};
    }
    if (profile == QStringLiteral("custom")) {
        bool capacity_ok = false;
        bool threshold_ok = false;
        const auto capacity =
            settings.value(QString::fromLatin1(buffer_capacity_settings_key)).toInt(&capacity_ok);
        const auto threshold =
            settings.value(QString::fromLatin1(buffer_threshold_settings_key)).toInt(&threshold_ok);
        const audio::PlaybackBufferDurationConfig config{
            .capacity = std::chrono::milliseconds{capacity},
            .start_threshold = std::chrono::milliseconds{threshold},
        };
        if (capacity_ok && threshold_ok && capacity >= minimum_custom_buffer_ms &&
            capacity <= maximum_custom_buffer_ms &&
            audio::valid_local_audition_buffer_config(config)) {
            return {.profile = profile, .config = config};
        }
    }
    return {.profile = QStringLiteral("balanced"),
            .config = audio::playback_buffer_preset_config(audio::PlaybackBufferPreset::balanced)};
}

void Workspace::configurePlaybackBuffer(const QString& profile, const int capacity_ms,
                                        const int start_threshold_ms) {
    const audio::PlaybackBufferDurationConfig config{
        .capacity = std::chrono::milliseconds{capacity_ms},
        .start_threshold = std::chrono::milliseconds{start_threshold_ms},
    };
    if (!audio::valid_local_audition_buffer_config(config)) {
        view_->showMessage(QStringLiteral("Invalid playback buffer values"), 5'000);
        view_->refreshPlaybackBufferChecks();
        return;
    }

    if (!playingOnEngine()) {
        view_->showMessage(QStringLiteral("Playback buffer unchanged: no engine"), 5'000);
        view_->refreshPlaybackBufferChecks();
        return;
    }
    // ADR-0226: the engine's buffer, which it keeps. Settings mirror it so
    // the dialog shows the engine's value.
    transport_->setBuffer(capacity_ms, start_threshold_ms);

    selected_buffer_profile_ = profile;
    QSettings settings;
    settings.setValue(QString::fromLatin1(buffer_profile_settings_key), profile);
    settings.setValue(QString::fromLatin1(buffer_capacity_settings_key), capacity_ms);
    settings.setValue(QString::fromLatin1(buffer_threshold_settings_key), start_threshold_ms);
    settings.sync();
    view_->refreshPlaybackBufferChecks();

    const bool pending = transport_->state().status != QStringLiteral("stopped");
    view_->showMessage(
        QStringLiteral("%1 buffer · %2 ms capacity · %3 ms start%4")
            .arg(bufferProfileLabel(profile))
            .arg(capacity_ms)
            .arg(start_threshold_ms)
            .arg(pending ? QStringLiteral(" · applies next track") : QString{}),
        5'000);
}

void Workspace::reloadPlaybackPreferences() {
    const QSettings settings;
    const auto preference = loadPlaybackBufferPreference();
    const auto engine = playingOnEngine() ? std::optional{transport_->state()} : std::nullopt;
    if (selected_buffer_profile_ != preference.profile ||
        (engine &&
         (engine->buffer_capacity_ms != preference.config.capacity.count() ||
          engine->buffer_start_threshold_ms != preference.config.start_threshold.count()))) {
        configurePlaybackBuffer(preference.profile,
                                static_cast<int>(preference.config.capacity.count()),
                                static_cast<int>(preference.config.start_threshold.count()));
    }
    const auto with_gain =
        settings.value(QStringLiteral("playback/rg-preamp-with"), 0.0).toDouble();
    const auto without_gain =
        settings.value(QStringLiteral("playback/rg-preamp-without"), 0.0).toDouble();
    if (local_rg_preamp_with_ != with_gain || local_rg_preamp_without_ != without_gain) {
        local_rg_preamp_with_ = with_gain;
        local_rg_preamp_without_ = without_gain;
        applyLocalPlaybackModes();
    }
}

void Workspace::previous() {
    if (playingOnEngine()) {
        transport_->previous();
    }
}

void Workspace::next() {
    if (playingOnEngine()) {
        transport_->next();
    }
}

void Workspace::stop() {
    if (playingOnEngine()) {
        transport_->stop();
    }
}

void Workspace::setVolume(const int percent) {
    if (playingOnEngine()) {
        // The engine owns the output, so the volume lives there: another
        // client watching the same engine sees the same number, and it
        // survives this window closing.
        transport_->setVolume(percent);
    }
}

void Workspace::selectOutput(const std::string& id) {
    if (playingOnEngine()) {
        transport_->selectOutput(id);
    }
}

void Workspace::setOutputDevice(const std::optional<std::string>& target) {
    if (playingOnEngine()) {
        transport_->setOutput(target);
    }
}

void Workspace::refreshOutputs() {
    if (playingOnEngine()) {
        transport_->refreshOutputs();
    }
}

void Workspace::setRepeat(const bool on) {
    playback_.modes.repeat = on;
    applyLocalPlaybackModes();
}

// Random and album shuffle exclude each other.
void Workspace::setRandom(const bool on) {
    playback_.modes.random = on;
    if (on) {
        playback_.modes.album_random = false;
    }
    applyLocalPlaybackModes();
}

void Workspace::setAlbumRandom(const bool on) {
    playback_.modes.album_random = on;
    if (on) {
        playback_.modes.random = false;
    }
    applyLocalPlaybackModes();
}

void Workspace::cycleSingle() {
    playback_.modes.single = audio::next_mode_state(playback_.modes.single);
    applyLocalPlaybackModes();
}

void Workspace::cycleConsume() {
    playback_.modes.consume = audio::next_mode_state(playback_.modes.consume);
    applyLocalPlaybackModes();
}

void Workspace::setReplayGain(const QString& mode) {
    local_replaygain_ = mode;
    applyLocalPlaybackModes();
}

void Workspace::followEngineState(const EnginePlayback::State& state) {
    // ReplayGain changed on the playing engine from elsewhere: taken, and
    // given to the others. Only a change counts -- what an engine reports
    // before it has been given this window's setting is its default, not a
    // choice -- and a report from before this window's own change, still on
    // its way, is none.
    const auto reported = state.replay_gain_mode;
    const bool changed_there =
        engine_replay_gain_.has_value() && *engine_replay_gain_ != reported;
    engine_replay_gain_ = reported;
    if (changed_there && !transport_->settling() && reported != resolvedReplayGain()) {
        local_replaygain_ = reported == audio::ReplayGainMode::track
                                ? QStringLiteral("track")
                            : reported == audio::ReplayGainMode::album
                                ? QStringLiteral("album")
                                : QStringLiteral("off");
        saveLocalPlaybackModes();
        syncReplayGain();
        view_->refreshLocalPlaybackControls();
    }
    // Not while a command of this window's is unanswered: a state the engine
    // reported before it -- Random still off just after Random was turned on
    // -- would undo it, and the ReplayGain it resolves to would follow. The
    // command's answer brings the state after it.
    if (!transport_->settling() && state.modes != playback_.modes) {
        // The engine owns the modes while it owns playback: a one-shot
        // expires where the track actually ended. Adopted rather than pushed
        // back, or the two would argue.
        playback_.modes = state.modes;
        saveLocalPlaybackModes();
        view_->refreshLocalPlaybackControls();
    }
    if (!state.consumed.isEmpty() && state.consumed != engine_consumed_) {
        engine_consumed_ = state.consumed;
        // The engine dropped it from its queue; the list it came from drops it
        // too. Told rather than deduced, so the two cannot disagree.
        if (const auto dropped = core::StableId::parse(state.consumed.toStdString())) {
            for (const auto& tab : list_tabs_) {
                const auto row = tab->model->rowOfEntry(*dropped, -1);
                if (row < 0) {
                    continue;
                }
                consuming_row_ = true;
                tab->model->removeRowIndexes({row}, false);
                consuming_row_ = false;
                takeEngineChange(*tab);
                // Nor is it in the engine's queue any more, as this window
                // knows it.
                engine_queue_.remove(state.consumed);
                schedulePersist();
                break;
            }
        }
    }
    if (state.entry != engine_entry_) {
        engine_entry_ = state.entry;
        // The engine consumes a request by playing it, so the panel has to let
        // go of it too or it would be re-stated on the next sync and play
        // twice. The return-point the local path keeps is the engine's
        // business now: it continues from the row it played, which is a
        // difference worth knowing rather than papering over.
        const auto started = core::StableId::parse(state.entry.toStdString());
        if (started) {
            const auto& pending = playback_.requests.pending();
            const auto match = std::ranges::find_if(pending, [&started](const auto& entry) {
                return entry.source.entry_id == *started;
            });
            if (match != pending.end()) {
                playback_.requests.started(*match);
                engine_requests_.reset();
                persistUpNext();
                view_->refreshUpNext();
            } else if (playback_.requests.active()) {
                playback_.requests.finished();
                persistUpNext();
                view_->refreshUpNext();
            }
        }
        if (auto* tab = tabForDocument(playback_.anchors.document); tab != nullptr) {
            const auto& rows = tab->model->rows();
            const auto match = std::find_if(rows.begin(), rows.end(), [&state](const auto& row) {
                return QString::fromStdString(row.entry_id.to_string()) == state.entry;
            });
            if (match != rows.end()) {
                const auto row = static_cast<int>(std::distance(rows.begin(), match));
                playback_.anchors.current = match->entry_id;
                playback_.row = row;
                tab->model->setCurrentSource(tab->model->source(row), row);
            }
        }
    }

    // Has the engine's queue drifted from what this window is showing? Only
    // a new revision says it might have, so the queue is fetched then and not
    // on every sample -- and whatever its size: another client replacing the
    // list with one as long must show too. Its own edits come back unchanged
    // and are dropped by the comparison inside.
    // Not while this window's own commands are on their way: until they are
    // answered, the engine may report a queue from before them, and adopting
    // it would trade the rows just added for the engine's older list -- and
    // then, once the engine caught up, bring them back as bare paths. The
    // revision is left unread so the check runs once they are answered.
    if (state.queue_revision != engine_queue_revision_ && !transport_->settling()) {
        engine_queue_revision_ = state.queue_revision;
        if (tabForDocument(playback_.anchors.document) != nullptr) {
            adoptEngineQueue();
        }
    }
    // ADR-0269: Up Next is the engine's. When it moves -- another client, or
    // a request played -- what the engine holds is shown; this window's own
    // changes come back the same and change nothing.
    if ((!engine_requests_revision_ || state.requests_revision != *engine_requests_revision_) &&
        !transport_->settling()) {
        engine_requests_revision_ = state.requests_revision;
        adoptEngineRequests();
    }

}

void Workspace::adoptEngineRequests() {
    auto* playing = linkOf(transport_);
    if (transport_ == nullptr || playing == nullptr) {
        return;
    }
    const QPointer self{this};
    const QPointer asked{transport_};
    transport_->request(
        QStringLiteral("playback.requests"), protocol::Json{{"details", true}},
        [self, asked, key = playing->key](core::Result<protocol::Json> answer) {
            // An engine from before ADR-0269 answers identities only, and is
            // left as it was: this window states its Up Next to it.
            if (!self || !answer || asked != self->transport_) {
                return;
            }
            const auto entries = answer->value("entries", protocol::Json::array());
            if (!entries.empty() && !entries.front().is_object()) {
                return;
            }
            // Rows this window has already keep what it knows of them.
            std::vector<LocalTrackRow> rows;
            QString stated;
            for (const auto& entry : entries) {
                auto row = EnginePlayback::rowOfEntry(entry);
                if (!row) {
                    continue;
                }
                const auto& pending = self->playback_.requests.pending();
                const auto known = std::ranges::find_if(pending, [&row](const auto& held) {
                    return held.source.entry_id == row->entry_id;
                });
                if (known != pending.end()) {
                    row = known->source;
                }
                stated += QString::fromStdString(row->entry_id.to_string());
                rows.push_back(std::move(*row));
            }
            QString shown;
            for (const auto& held : self->playback_.requests.pending()) {
                shown += QString::fromStdString(held.source.entry_id.to_string());
            }
            const bool first = !self->engine_requests_known_;
            self->engine_requests_known_ = true;
            self->engine_requests_ = stated;
            // Heard for the first time and holding none, while this window
            // has some -- asked for before the engine answered: they are its
            // now. Later, an empty Up Next is another client's clearing.
            if (first && stated.isEmpty() && !shown.isEmpty()) {
                self->engine_requests_.reset();
                self->syncEngineRequests();
                return;
            }
            if (shown == stated) {
                return;
            }
            self->playback_.requests.replace(std::move(rows));
            if (!stated.isEmpty()) {
                self->up_next_engine_ = key;
            }
            self->persistUpNext();
            self->view_->refreshUpNext();
        });
}

Workspace::NowPlaying Workspace::nowPlaying(const EnginePlayback::State& state) {
    NowPlaying shown;
    const auto stopped = state.status == QStringLiteral("stopped");
    if (stopped && !state.error.isEmpty()) {
        // Asked to play and could not: said where the track would be, or
        // the engine reads as idle and the ask as lost.
        shown.title = tr("Could not play");
        shown.context = state.error;
        shown.tooltip = state.error;
        return shown;
    }
    if (stopped || state.path.isEmpty()) {
        shown.title = QStringLiteral("Nothing playing");
        return shown;
    }
    // Named from the list the entry came from when it is still open, so the
    // header reads the same as the row; the file name is the fallback for an
    // entry whose tab has been closed.
    auto label = QFileInfo{state.path}.fileName();
    QString context;
    const auto* row = playingRow(state.entry);
    if (row != nullptr && !row->title.empty()) {
        label = QString::fromStdString(row->title);
        // "Artist — Album (Year)", as much of it as the tags have.
        QStringList parts;
        if (!row->artist.empty()) {
            parts << QString::fromStdString(row->artist);
        }
        if (!row->album.empty()) {
            auto album = QString::fromStdString(row->album);
            if (row->date.size() >= 4U) {
                album += QStringLiteral(" (%1)").arg(QString::fromStdString(row->date.substr(0, 4)));
            }
            parts << album;
        }
        context = parts.join(QStringLiteral(" — "));
    }
    if (auto* tab = tabForDocument(playback_.anchors.document);
        tab != nullptr && context.isEmpty()) {
        context = QString::fromStdString(tab->document.name);
    }
    // Paused because another engine is playing on these speakers: said where
    // the album would be, so the silence has a reason.
    if (!state.speakers_taken_by.isEmpty() && state.status != QStringLiteral("playing")) {
        // Named as the engine at that address is, when it is one of ours.
        auto taker = state.speakers_taken_by;
        for (const auto& engine : engines_) {
            if (!engine->key.isLocal() && engine->setting.address == state.speakers_taken_by) {
                taker = engineName(engine->key);
            }
        }
        context = tr("Paused · %1 is playing on these speakers").arg(taker);
    }
    shown.title = label;
    // In the window title too, which is what a taskbar or window switcher
    // shows.
    const auto artist = row != nullptr ? QString::fromStdString(row->artist) : QString{};
    shown.window_title = (artist.isEmpty() ? label : artist + QStringLiteral(" – ") + label) +
                         QStringLiteral(" — Trackknife");
    shown.context = context;
    shown.tooltip = state.path;
    shown.cover_entry = state.entry;
    if (const auto* tab = tabForDocument(playback_.anchors.document);
        tab != nullptr && row != nullptr && !row->rating_hash.empty()) {
        shown.rating_hash = QString::fromStdString(row->rating_hash);
        shown.rating_engine = EngineKey::of(tab->document);
        shown.rating = row->rating;
    }
    return shown;
}

QString Workspace::outputLabel(const EnginePlayback::State::Output& output) const {
    // A machine has one name wherever it is listed: this computer is
    // "caprica" in its own engine's speakers as in the server's, where it
    // plays as an agent. Only an engine that gives no name is described.
    if (output.local && (output.name.empty() || output.name == "this machine")) {
        return output_choices_engine_.isLocal() ? QStringLiteral("This computer")
                                                : QStringLiteral("The server");
    }
    return displayText(output.name);
}

Workspace::OutputSummary Workspace::takeOutputs(const EnginePlayback::State& state) {
    OutputSummary summary;
    std::vector<std::pair<std::string, std::string>> choices;
    choices.reserve(state.devices.size());
    for (const auto& device : state.devices) {
        choices.emplace_back(device.name, device.description);
    }
    if (engine_output_seen_) {
        if (selected_device_available_ && !state.output_available) {
            view_->showMessage(QStringLiteral("Audio output unavailable · playback paused"),
                               5'000);
        } else if (!selected_device_available_ && state.output_available &&
                   !state.output_suspended) {
            view_->showMessage(
                QStringLiteral("Audio output available again · press Play to resume"), 5'000);
        } else if (!state.output_target && default_device_ &&
                   state.default_output != default_device_) {
            view_->showMessage(QStringLiteral("System audio output changed"), 5'000);
        }
    }
    // ADR-0228: the chosen agent going away and coming back is worth saying;
    // the music waits for it either way.
    const auto chosen = [](const std::vector<EnginePlayback::State::Output>& outputs) {
        const auto found = std::ranges::find_if(outputs, &EnginePlayback::State::Output::selected);
        return found == outputs.end() ? std::optional<EnginePlayback::State::Output>{}
                                      : std::optional{*found};
    };
    const auto was = chosen(output_choices_);
    const auto now = chosen(state.outputs);
    if (engine_output_seen_ && was && now && was->id == now->id && !now->local &&
        was->online != now->online) {
        view_->showMessage(now->online
                               ? QStringLiteral("%1 is back").arg(outputLabel(*now))
                               : QStringLiteral("%1 went away · playback waits for it")
                                     .arg(outputLabel(*now)),
                           5'000);
    }
    const auto* playing = linkOf(transport_);
    const auto engine = playing != nullptr ? playing->key : EngineKey::local();
    const bool outputs_changed =
        state.outputs != output_choices_ || engine != output_choices_engine_;
    output_choices_ = state.outputs;
    output_choices_engine_ = engine;
    engine_output_seen_ = true;
    summary.menu_changed = outputs_changed || choices != device_choices_ ||
                           state.output_target != selected_device_ ||
                           state.output_available != selected_device_available_ ||
                           state.default_output != default_device_;
    device_choices_ = std::move(choices);
    selected_device_ = state.output_target;
    default_device_ = state.default_output;
    selected_device_available_ = state.output_available;

    const auto label_of = [this](const std::string& name) {
        const auto found =
            std::ranges::find(device_choices_, name, &std::pair<std::string, std::string>::first);
        return found == device_choices_.end()
                   ? displayText(name)
                   : displayText(found->second.empty() ? found->first : found->second);
    };
    QString device_label = QStringLiteral("System default");
    if (selected_device_) {
        device_label = label_of(*selected_device_);
    } else if (default_device_) {
        device_label += QStringLiteral(" — %1").arg(label_of(*default_device_));
    }
    if (!state.output_available) {
        device_label += QStringLiteral(" (unavailable)");
    }

    // The profile is a name for a pair of numbers, and the engine keeps only
    // the numbers; a match against the presets recovers the name.
    auto profile = QStringLiteral("custom");
    for (const auto preset :
         {audio::PlaybackBufferPreset::responsive, audio::PlaybackBufferPreset::balanced,
          audio::PlaybackBufferPreset::resilient}) {
        const auto config = audio::playback_buffer_preset_config(preset);
        if (config.capacity.count() == state.buffer_capacity_ms &&
            config.start_threshold.count() == state.buffer_start_threshold_ms) {
            const auto id = audio::playback_buffer_preset_id(preset);
            profile = QString::fromLatin1(id.data(), static_cast<qsizetype>(id.size()));
        }
    }
    if (state.buffer_capacity_ms > 0 && profile != selected_buffer_profile_) {
        selected_buffer_profile_ = profile;
        QSettings settings;
        settings.setValue(QString::fromLatin1(buffer_profile_settings_key), profile);
        settings.setValue(QString::fromLatin1(buffer_capacity_settings_key),
                          static_cast<int>(state.buffer_capacity_ms));
        settings.setValue(QString::fromLatin1(buffer_threshold_settings_key),
                          static_cast<int>(state.buffer_start_threshold_ms));
        view_->refreshPlaybackBufferChecks();
    }

    // Where the sound goes, on the button itself: music coming out of another
    // room is not something to have to hover to find out. Named when there is
    // a choice to have made -- an agent, or a device other than the default.
    const bool agents = std::ranges::any_of(output_choices_, [](const auto& output) {
        return !output.local;
    });
    // Nothing chosen yet: the engine's own speakers, by the same name.
    const auto own = std::ranges::find_if(output_choices_,
                                          [](const auto& output) { return output.local; });
    const auto engine_name =
        own != output_choices_.end()
            ? outputLabel(*own)
            : (engine.isLocal() ? QStringLiteral("This computer") : engineName(engine));
    const auto speaker = now ? outputLabel(*now) : engine_name;
    const auto device = selected_device_ ? label_of(*selected_device_) : QString{};
    if (agents) {
        summary.shown = device.isEmpty() ? speaker : QStringLiteral("%1 · %2").arg(speaker, device);
    } else if (!device.isEmpty()) {
        summary.shown = device;
    }
    if (now && !now->online) {
        summary.shown += QStringLiteral(" (offline)");
    }
    summary.description = QStringLiteral("%1 → %2 → %3%4")
                              .arg(engine_name, speaker, device_label,
                                   now && !now->online ? QStringLiteral(" (offline)") : QString{});
    summary.selected_output = now ? QString::fromStdString(now->id) : QString{};
    summary.tooltip =
        QStringLiteral("Audio output: %1\nBuffer: %2 · %3 ms capacity · %4 ms start%5\n"
                       "Underruns: %6")
            .arg(summary.description)
            .arg(bufferProfileLabel(selected_buffer_profile_))
            .arg(state.buffer_capacity_ms)
            .arg(state.buffer_start_threshold_ms)
            .arg(state.buffer_pending ? QStringLiteral(" · applies next track") : QString{})
            .arg(state.underruns);
    if (!state.output_available) {
        summary.tooltip += QStringLiteral("\nPlayback is paused until an output is available");
    } else if (state.output_suspended) {
        summary.tooltip += QStringLiteral("\nReconnecting the audio output");
    }
    return summary;
}

Workspace::OutputMenu Workspace::outputMenu() const {
    OutputMenu menu;
    // ADR-0228: which of the engine's outputs plays -- shown once there is a
    // choice, which is when an agent has ever registered.
    menu.speakers_shown = std::ranges::any_of(output_choices_, [](const auto& output) {
        return !output.local;
    });
    if (menu.speakers_shown) {
        for (const auto& output : output_choices_) {
            auto label = outputLabel(output);
            if (!output.online) {
                label += QStringLiteral(" (offline)");
            }
            QString tooltip;
            // An offline agent can still be chosen: the music waits there
            // and starts when it is back.
            if (!output.online) {
                tooltip = QStringLiteral("Not connected. Chosen, it plays as soon as it is back.");
            } else if (!output.local && !output.files) {
                tooltip = QStringLiteral("Streams the music from the engine");
            }
            menu.speakers.push_back(OutputMenu::Speaker{.id = output.id,
                                                        .label = std::move(label),
                                                        .tooltip = std::move(tooltip),
                                                        .checked = output.selected});
        }
        const auto chosen =
            std::ranges::find_if(output_choices_, &EnginePlayback::State::Output::selected);
        menu.devices_heading = chosen != output_choices_.end()
                                   ? QStringLiteral("Sound device on %1").arg(outputLabel(*chosen))
                                   : QStringLiteral("Sound device");
    }
    const auto add_device = [this, &menu](const QString& label, std::optional<std::string> target,
                                          const bool enabled = true) {
        const bool checked = target == selected_device_;
        menu.devices.push_back(OutputMenu::Device{
            .target = std::move(target), .label = label, .checked = checked, .enabled = enabled});
    };
    add_device(QStringLiteral("System default"), std::nullopt);
    for (const auto& [name, description] : device_choices_) {
        add_device(displayText(description.empty() ? name : description), name);
    }
    if (selected_device_ && std::ranges::none_of(device_choices_, [this](const auto& choice) {
            return choice.first == *selected_device_;
        })) {
        add_device(QStringLiteral("%1 (unavailable)").arg(displayText(*selected_device_)),
                   selected_device_, false);
    }
    return menu;
}

} // namespace trackknife::bench
