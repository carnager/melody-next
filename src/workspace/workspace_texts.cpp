// SPDX-License-Identifier: GPL-3.0-only

// What the workspace's parts are called, as every window shows them: a
// list's tab, an empty list, a selection, Up Next, the playback modes.
// Written once so the windows cannot come to say different things.

#include "workspace/workspace.hpp"

#include "bench/bench_main_window_helpers.hpp"

#include <QStringList>

#include <array>
#include <utility>

namespace trackknife::bench {

QString Workspace::emptyListTitle(const EngineKey& engine) const {
    return !engine.isLocal() ? tr("Nothing from %1 here yet").arg(engineName(engine))
                             : tr("This list is empty");
}

QString Workspace::emptyListHint(const EngineKey& engine) const {
    return !engine.isLocal()
               ? tr("Add albums from %1's library on the left, or drag them here.")
                     .arg(engineName(engine))
               : tr("Drop files or folders here, or add albums from the library on the left.");
}

Workspace::TabChrome Workspace::tabChrome(const ListTab& tab) const {
    TabChrome chrome;
    const auto name = displayText(tab.document.name);
    chrome.playing = QString::fromStdString(tab.document.id.to_string()) == active_local_list_id_;
    chrome.text = name + (tab.document.dirty ? QStringLiteral(" *") : QString{});
    // The playing dot is the tab bar's own; the icon says where it plays.
    chrome.remote = !EngineKey::of(tab.document).isLocal();
    const auto kind = tab.document.kind == persistence::ListKind::scratch
                          ? QStringLiteral("Persistent scratch list")
                          : QStringLiteral("Named Trackknife working list");
    chrome.tooltip =
        QStringLiteral("%1%2%3").arg(kind,
                                     tab.document.pinned ? QStringLiteral(" · pinned") : QString{},
                                     tab.document.dirty ? QStringLiteral(" · modified") : QString{});
    if (chrome.playing) {
        chrome.tooltip += tr(" · Active playback queue");
    }
    if (const auto continuation = continuationOf(tab)) {
        chrome.continues = continuation->name.isEmpty() ? tr("a dynamic playlist") : continuation->name;
        chrome.text += QStringLiteral(" ∞");
        chrome.tooltip += tr(" · Continues with %1").arg(chrome.continues);
    }
    chrome.accessible_name = QStringLiteral("%1 track list").arg(name);
    return chrome;
}

Workspace::Summary Workspace::selectionSummary(const ListTab* tab,
                                               const std::vector<int>& rows) const {
    Summary none{.text = QStringLiteral("No tracks selected"), .tooltip = {}};
    if (tab == nullptr || rows.empty()) {
        return none;
    }
    const auto& tracks = tab->model->rows();
    if (rows.size() == 1U) {
        const auto row_index = rows.front();
        if (row_index < 0 || row_index >= static_cast<int>(tracks.size())) {
            return none;
        }
        const auto& track = tracks[static_cast<std::size_t>(row_index)];
        const auto fallback = tab->model->index(row_index, local_title_column).data().toString();
        const auto title = track.title.empty() ? fallback : displayText(track.title);
        QStringList details;
        details.push_back(track.artist.empty()
                              ? title
                              : QStringLiteral("%1 — %2").arg(displayText(track.artist), title));
        if (!track.album.empty() || !track.date.empty()) {
            auto release = displayText(track.album);
            if (!track.date.empty()) {
                release += release.isEmpty() ? displayText(track.date)
                                             : QStringLiteral(" (%1)").arg(displayText(track.date));
            }
            details.push_back(release);
        }
        if (track.duration_ms) {
            details.push_back(formatTime(*track.duration_ms));
        }
        return {.text = details.join(QStringLiteral(" · ")),
                .tooltip = QString::fromStdString(core::display_raw_path(track.raw_path))};
    }
    qint64 total_duration_ms = 0;
    int unknown_durations = 0;
    for (const auto row : rows) {
        if (row < 0 || row >= static_cast<int>(tracks.size())) {
            continue;
        }
        const auto& duration = tracks[static_cast<std::size_t>(row)].duration_ms;
        if (duration) {
            total_duration_ms += *duration;
        } else {
            ++unknown_durations;
        }
    }
    auto summary = QStringLiteral("%1 tracks selected · %2 total")
                       .arg(rows.size())
                       .arg(formatTime(total_duration_ms));
    if (unknown_durations > 0) {
        summary += QStringLiteral(" · %1 duration unknown").arg(unknown_durations);
    }
    return {.text = summary, .tooltip = summary};
}

Workspace::UpNextHeading Workspace::upNextHeading() const {
    UpNextHeading heading;
    auto* tab = const_cast<Workspace*>(this)->tabForDocument(playback_.anchors.document);
    QString playing;
    if (playback_.requests.active()) {
        playing = QString::fromStdString(playback_.requests.active()->source.artist + " — " +
                                         playback_.requests.active()->source.title);
    }
    // Up Next holds one engine's tracks (ADR-0227): named by that engine
    // while it holds any, else by the engine that is playing -- "Local" was
    // the MPD era's word for this computer.
    const bool holding =
        !playback_.requests.pending().empty() || playback_.requests.active().has_value();
    const auto* playing_engine = linkOf(transport_);
    const auto key = holding ? up_next_engine_
                             : (playing_engine != nullptr ? playing_engine->key : EngineKey::local());
    const auto engine = key.isLocal() ? QStringLiteral("This computer") : engineName(key);
    heading.status =
        QStringLiteral("%1 · %2 waiting").arg(engine).arg(playback_.requests.pending().size());
    heading.status_tooltip = playing.isEmpty() ? QString{} : QStringLiteral("Playing: ") + playing;
    // Where playback goes once these are done, and a way there now.
    const auto back = tab ? QString::fromStdString(tab->document.name) : QString{};
    heading.back = back.isEmpty() ? tr("Back to the list") : tr("Back to %1").arg(back);
    heading.back_tooltip = tr("Skip what is waiting and return to the list now");
    heading.back_enabled =
        playback_.requests.active().has_value() || !playback_.requests.pending().empty();
    heading.can_undo = playback_.requests.canUndo();
    return heading;
}

Workspace::ModeTexts Workspace::modeTexts() const {
    ModeTexts texts;
    const auto on_off = [](const bool on) {
        return on ? QStringLiteral("On") : QStringLiteral("Off");
    };
    texts.repeat = {.text = QStringLiteral("Repeat"),
                    .tooltip = QStringLiteral("Repeat: %1").arg(on_off(playback_.modes.repeat)),
                    .checked = playback_.modes.repeat,
                    .oneshot = false};
    texts.random = {.text = QStringLiteral("Random"),
                    .tooltip = QStringLiteral("Random: %1").arg(on_off(playback_.modes.random)),
                    .checked = playback_.modes.random,
                    .oneshot = false};
    texts.album_random = {
        .text = tr("Album shuffle"),
        .tooltip = tr("Shuffle albums during playback without rearranging the list. Tracks "
                      "within each album keep their list order."),
        .checked = playback_.modes.album_random,
        .oneshot = false};
    const auto cycle = [](const audio::ModeState mode, const QString& name, const QString& help) {
        const auto state = mode == audio::ModeState::off  ? QStringLiteral("Off")
                           : mode == audio::ModeState::on ? QStringLiteral("On")
                                                          : QStringLiteral("One-shot");
        return ModeText{.text = QStringLiteral("%1: %2").arg(name, state),
                        .tooltip = QStringLiteral("%1: %2\n%3").arg(name, state, help),
                        .checked = mode != audio::ModeState::off,
                        .oneshot = mode == audio::ModeState::oneshot};
    };
    texts.single = cycle(playback_.modes.single, QStringLiteral("Single"),
                         QStringLiteral("Stop after this track; with Repeat, repeat this track. "
                                        "Click to cycle Off / On / One-shot."));
    texts.consume =
        cycle(playback_.modes.consume, QStringLiteral("Consume"),
              QStringLiteral("Remove finished or skipped entries from the local list. Files stay "
                             "on disk. Click to cycle Off / On / One-shot."));
    for (const auto& [label, value] : replayGainModes()) {
        if (value == local_replaygain_) {
            texts.replaygain = QStringLiteral("ReplayGain: %1").arg(label);
            texts.replaygain_tooltip =
                QStringLiteral("ReplayGain: %1\nAutomatic: track gain with Random, album gain "
                               "otherwise.\nUses embedded gain with peak-based clipping "
                               "prevention when a matching peak is present.\nChanges apply as "
                               "buffered audio drains; missing gain plays unchanged.")
                    .arg(label);
        }
    }
    texts.replaygain_active = local_replaygain_ != QStringLiteral("off");
    return texts;
}

std::vector<std::pair<QString, QString>> Workspace::replayGainModes() {
    return {
        {QStringLiteral("Off"), QStringLiteral("off")},
        {QStringLiteral("Track"), QStringLiteral("track")},
        {QStringLiteral("Album"), QStringLiteral("album")},
        {QStringLiteral("Automatic"), QStringLiteral("auto")},
    };
}

} // namespace trackknife::bench
