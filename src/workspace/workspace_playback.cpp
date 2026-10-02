// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/workspace.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/metadata/document.hpp"
#include "workspace/workspace_view.hpp"

#include <QPointer>
#include <QSettings>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace trackknife::bench {
namespace {

// Reads the row's projected ReplayGain values at one provenance layer:
// the last value wins, mirroring the CUE remark policy.
[[nodiscard]] std::optional<formats::ReplayGainInfo>
projected_replay_gain(const LocalTrackRow& row, const metadata::FieldProvenance provenance) {
    const auto last_value =
        [&row, provenance](const std::string_view name) -> std::optional<std::string> {
        const auto canonical = metadata::canonicalize_field_name(name);
        std::optional<std::string> value;
        for (const auto& field : row.metadata.fields) {
            if (field.provenance == provenance && field.canonical_name == canonical &&
                !field.values.empty()) {
                value = field.values.back();
            }
        }
        return value;
    };
    formats::ReplayGainInfo info;
    if (const auto text = last_value("REPLAYGAIN_TRACK_GAIN")) {
        info.track_gain_db = formats::parse_replay_gain_decibels(*text);
    }
    if (const auto text = last_value("REPLAYGAIN_TRACK_PEAK")) {
        info.track_peak = formats::parse_replay_gain_peak(*text);
    }
    if (const auto text = last_value("REPLAYGAIN_ALBUM_GAIN")) {
        info.album_gain_db = formats::parse_replay_gain_decibels(*text);
    }
    if (const auto text = last_value("REPLAYGAIN_ALBUM_PEAK")) {
        info.album_peak = formats::parse_replay_gain_peak(*text);
    }
    if (!info.track_gain_db && !info.album_gain_db) {
        return std::nullopt;
    }
    return info;
}

// The explicit playback override, in persistence-precedence order
// (ADR-0139/0141): fresh sidecar values first, then a CUE track's
// sheet-carried REM values; otherwise the decoder's own tags apply.
[[nodiscard]] std::optional<formats::ReplayGainInfo>
local_replay_gain_override(const LocalTrackRow& row) {
    if (auto sidecar = projected_replay_gain(row, metadata::FieldProvenance::sidecar)) {
        return sidecar;
    }
    if (row.logical_reference && row.logical_reference->starts_with("cue-v1")) {
        return projected_replay_gain(row, metadata::FieldProvenance::segment);
    }
    return std::nullopt;
}

} // namespace

namespace {
// Not an engine's status: the one this window told to stop, until it has.
const QString stopping_status = QStringLiteral("stopping (asked)");
} // namespace

void Workspace::saveLocalPlaybackModes() {
    QSettings settings;
    settings.setValue(QStringLiteral("playback/local-repeat"), playback_.modes.repeat);
    settings.setValue(QStringLiteral("playback/local-random"), playback_.modes.random);
    settings.setValue(QStringLiteral("playback/local-album-random"), playback_.modes.album_random);
    settings.setValue(QStringLiteral("playback/local-single"),
                      static_cast<int>(playback_.modes.single));
    settings.setValue(QStringLiteral("playback/local-consume"),
                      static_cast<int>(playback_.modes.consume));
    settings.setValue(QStringLiteral("playback/local-replaygain"), local_replaygain_);
    settings.setValue(QStringLiteral("playback/rg-preamp-with"), local_rg_preamp_with_);
    settings.setValue(QStringLiteral("playback/rg-preamp-without"), local_rg_preamp_without_);
}


// As they were left, for the window: read once, when the workspace is
// made, before any engine is reached.
void Workspace::loadLocalPlaybackModes() {
    const QSettings settings;
    playback_.modes.repeat = settings.value(QStringLiteral("playback/local-repeat"), false).toBool();
    playback_.modes.random = settings.value(QStringLiteral("playback/local-random"), false).toBool();
    playback_.modes.album_random =
        settings.value(QStringLiteral("playback/local-album-random"), false).toBool();
    if (playback_.modes.album_random) {
        playback_.modes.random = false;
    }
    playback_.modes.single = audio::mode_state_from_int(
        settings.value(QStringLiteral("playback/local-single"), 0).toInt());
    playback_.modes.consume = audio::mode_state_from_int(
        settings.value(QStringLiteral("playback/local-consume"), 0).toInt());
    local_replaygain_ =
        settings.value(QStringLiteral("playback/local-replaygain"), QStringLiteral("off"))
            .toString();
    if (local_replaygain_ != QStringLiteral("track") &&
        local_replaygain_ != QStringLiteral("album") &&
        local_replaygain_ != QStringLiteral("auto")) {
        local_replaygain_ = QStringLiteral("off");
    }
    const auto preamp_limit = static_cast<double>(audio::maximum_replay_gain_preamp_db);
    local_rg_preamp_with_ =
        std::clamp(settings.value(QStringLiteral("playback/rg-preamp-with"), 0.0).toDouble(),
                   -preamp_limit, preamp_limit);
    local_rg_preamp_without_ =
        std::clamp(settings.value(QStringLiteral("playback/rg-preamp-without"), 0.0).toDouble(),
                   -preamp_limit, preamp_limit);
}

audio::ReplayGainMode Workspace::resolvedReplayGain() const {
    // "Auto" is this window's policy about its own shuffle, so it resolves
    // here whichever player is listening.
    if (local_replaygain_ == QStringLiteral("track") ||
        (local_replaygain_ == QStringLiteral("auto") && playback_.modes.random)) {
        return audio::ReplayGainMode::track;
    }
    if (local_replaygain_ == QStringLiteral("album") ||
        local_replaygain_ == QStringLiteral("auto")) {
        return audio::ReplayGainMode::album;
    }
    return audio::ReplayGainMode::off;
}

void Workspace::syncReplayGain() {
    // One ReplayGain for every engine this window reaches: whichever plays
    // next plays as loud as the last.
    const auto mode = resolvedReplayGain();
    const audio::ReplayGainPreamps preamps{
        .with_gain_db = static_cast<float>(local_rg_preamp_with_),
        .without_gain_db = static_cast<float>(local_rg_preamp_without_),
    };
    for (const auto& engine : engines_) {
        if (engine->playback != nullptr && engine->playback->active()) {
            engine->playback->setReplayGain(mode, preamps);
        }
    }
    // What the playing engine reports next is the answer to this, not news.
    engine_replay_gain_.reset();
}

void Workspace::applyLocalPlaybackModes() {
    saveLocalPlaybackModes();
    if (playingOnEngine()) {
        // The engine decides what plays next, so the modes are its business.
        transport_->setModes(playback_.modes);
    }
    syncReplayGain();
    view_->refreshLocalPlaybackControls();
}


void Workspace::syncEngineRequests() {
    // Asks go to the engine whose files they are, and only while it is the
    // one playing: the other would be asked for paths it does not have. No
    // asks at all are stated to whichever engine plays -- it may hold some
    // from before, saved across its own restart, and would play them.
    const auto* playing = linkOf(transport_);
    if (!playingOnEngine() || playing == nullptr) {
        return;
    }
    if (playing->key != up_next_engine_ && !playback_.requests.pending().empty()) {
        return;
    }
    std::vector<LocalTrackRow> rows;
    std::vector<std::optional<formats::ReplayGainInfo>> gains;
    QString stated;
    for (const auto& entry : playback_.requests.pending()) {
        stated += QString::fromStdString(entry.source.entry_id.to_string());
        gains.push_back(local_replay_gain_override(entry.source));
        rows.push_back(entry.source);
    }
    if (engine_requests_ == stated) {
        return;
    }
    engine_requests_ = stated;
    transport_->setRequests(rows, gains);
}


void Workspace::syncEngineQueue() {
    if (!playingOnEngine()) {
        return;
    }
    auto* tab = tabForDocument(playback_.anchors.document);
    if (tab == nullptr) {
        return;
    }
    const auto& rows = tab->model->rows();
    QString stated;
    for (const auto& row : rows) {
        stated += QString::fromStdString(row.entry_id.to_string());
    }
    if (stated == engine_queue_) {
        return;
    }
    engine_queue_ = stated;
    std::vector<std::optional<formats::ReplayGainInfo>> overrides;
    overrides.reserve(rows.size());
    for (const auto& row : rows) {
        overrides.push_back(local_replay_gain_override(row));
    }
    // The engine follows identity, so the playing entry survives being handed
    // a queue that no longer holds it in the same row -- or at all.
    transport_->replaceQueue(rows, overrides);
}


void Workspace::adoptEngineQueue() {
    if (transport_ == nullptr) {
        return;
    }
    // Asked, not waited for: the UI thread never blocks on the engine.
    const auto asked = ++engine_queue_asked_;
    const QPointer<Workspace> window{this};
    auto* playback = transport_;
    playback->queueEntries([window, asked, playback](std::vector<LocalTrackRow> held) {
        // A later ask, or another engine playing since: out of date.
        if (window && asked == window->engine_queue_asked_ && playback == window->transport_) {
            window->adoptEngineQueue(std::move(held));
        }
    });
}


void Workspace::adoptEngineQueue(std::vector<LocalTrackRow> held) {
    auto* tab = tabForDocument(playback_.anchors.document);
    if (tab == nullptr || held.empty()) {
        return;
    }
    // Merged rather than replaced: the engine's entries are paths and tags it
    // was given, while these rows carry everything this window has read from
    // the files. Replacing them would throw that away and show a list of
    // filenames.
    std::vector<LocalTrackRow> merged;
    merged.reserve(held.size());
    const auto& existing = tab->model->rows();
    for (const auto& entry : held) {
        const auto row = tab->model->rowOfEntry(entry.entry_id, -1);
        if (row >= 0) {
            merged.push_back(existing[static_cast<std::size_t>(row)]);
            continue;
        }
        auto fresh = entry;
        if (fresh.title.empty()) {
            fresh.title =
                core::display_raw_path(fresh.raw_path.substr(fresh.raw_path.find_last_of('/') + 1));
        }
        merged.push_back(std::move(fresh));
    }
    QString stated;
    for (const auto& row : merged) {
        stated += QString::fromStdString(row.entry_id.to_string());
    }
    if (stated == engine_queue_) {
        return;
    }
    engine_queue_ = stated;
    tab->model->replaceRows(std::move(merged), true);
    enqueueUnprobedRows(*tab);
    // What plays is the engine's to say. A list replaced elsewhere and
    // started at once reaches the window in one report: the entry it knew as
    // playing is gone, and its row number -- where the old track was -- says
    // nothing of the new list.
    const auto state = transport_->state();
    if (const auto playing = core::StableId::parse(state.entry.toStdString());
        playing && tab->model->rowOfEntry(*playing, -1) >= 0) {
        playback_.anchors.current = *playing;
        engine_entry_ = state.entry;
    }
    playback_.row = resolvePlaybackRow(tab);
    if (playback_.row >= 0) {
        tab->model->setCurrentSource(tab->model->source(playback_.row), playback_.row);
    }
    takeEngineChange(*tab);
}


void Workspace::reattachToEngine() {
    if (!playingOnEngine()) {
        return;
    }
    const auto state = transport_->state();
    if (state.entry.isEmpty()) {
        return; // The engine holds nothing; there is nothing to attach to.
    }
    const auto playing = core::StableId::parse(state.entry.toStdString());
    if (!playing) {
        return;
    }

    // The list it came from is usually still open: identities are persisted
    // with the document (ADR-0221), so the entry the engine names is findable
    // without inventing a tab.
    for (const auto& tab : list_tabs_) {
        const auto row = tab->model->rowOfEntry(*playing, -1);
        if (row < 0) {
            continue;
        }
        adoptEngineRow(*tab, row, *playing);
        return;
    }

    // Otherwise the queue is the only record of what is playing, so it becomes
    // a list. The rows carry the engine's identities rather than fresh ones,
    // or the anchor below would name an entry this list does not contain.
    const auto asked = ++engine_reattach_asked_;
    const QPointer<Workspace> window{this};
    auto* playback = transport_;
    playback->queueEntries([window, asked, playback](std::vector<LocalTrackRow> rows) {
        if (window && asked == window->engine_reattach_asked_ && playback == window->transport_) {
            window->reattachToQueue(std::move(rows));
        }
    });
}


void Workspace::reattachToQueue(std::vector<LocalTrackRow> rows) {
    const auto playing = core::StableId::parse(transport_->state().entry.toStdString());
    if (!playing || rows.empty()) {
        return;
    }
    // While the queue was on its way, what plays may have come to rest in a
    // list after all.
    for (const auto& tab : list_tabs_) {
        if (const auto row = tab->model->rowOfEntry(*playing, -1); row >= 0) {
            adoptEngineRow(*tab, row, *playing);
            return;
        }
    }
    // Untitled: the filename, until the file is read.
    QString stated;
    for (auto& row : rows) {
        if (row.title.empty()) {
            row.title =
                core::display_raw_path(row.raw_path.substr(row.raw_path.find_last_of('/') + 1));
        }
        stated += QString::fromStdString(row.entry_id.to_string());
    }
    auto* playing_engine = linkOf(transport_);
    // An engine elsewhere's queue goes into its tab, which it always has.
    auto* tab = playing_engine != nullptr && !playing_engine->key.isLocal()
                    ? engineTab(*playing_engine)
                    : addList(persistence::ListDocument{.id = core::StableId::random(),
                                                           .kind = persistence::ListKind::scratch,
                                                           .name = "Playing on the engine",
                                                           .pinned = false,
                                                           .dirty = false,
                                                           .items = {}},
                                 true);
    if (tab == nullptr) {
        return;
    }
    tab->model->replaceRows(std::move(rows), true);
    // What the engine holds, as this window knows it: these rows.
    engine_queue_ = stated;
    // Titles are filenames until the files have been read; the ordinary probe
    // queue fills them in rather than a second path for this case.
    enqueueUnprobedRows(*tab);
    const auto row = tab->model->rowOfEntry(*playing, -1);
    if (row >= 0) {
        adoptEngineRow(*tab, row, *playing);
    }
    takeEngineChange(*tab);
    schedulePersist();
}


void Workspace::adoptEngineRow(ListTab& tab, const int row, const core::StableId& entry) {
    playback_.anchors.document = tab.document.id;
    playback_.anchors.current = entry;
    playback_.row = row;
    engine_entry_ = QString::fromStdString(entry.to_string());
    tab.model->setCurrentSource(tab.model->source(row), row);
    setActiveLocalList(QString::fromStdString(tab.document.id.to_string()));
    view_->refreshTransport();
    view_->refreshPlaybackCursor(true);
}


void Workspace::followPlayback(EnginePlayback* playback, const bool stop_other) {
    if (playback == nullptr || playback == transport_) {
        return;
    }
    // ADR-0227: one engine plays at a time. Starting on one stops the other,
    // in that order, so an output agent the two share is released first.
    bool stopping = false;
    if (stop_other && transport_ != nullptr && transport_->active() &&
        transport_->state().status != QStringLiteral("stopped")) {
        transport_->stop();
        stopping = true;
    }
    if (auto* tab = tabForDocument(playback_.anchors.document); tab != nullptr) {
        tab->model->setCurrentSource({}, -1);
    }
    // The engine left behind is known: a report of it playing that arrives
    // after this -- it was started a moment ago -- is not a start elsewhere.
    if (transport_ != nullptr) {
        rememberEngineState(transport_);
        if (auto* left = linkOf(transport_); left != nullptr) {
            left->seen.status = stopping ? stopping_status : QStringLiteral("playing");
        }
    }
    transport_ = playback;
    // What the window knew about the other engine says nothing about this one.
    playback_.anchors = {};
    playback_.row = -1;
    engine_entry_.clear();
    engine_queue_.clear();
    engine_requests_.reset();
    engine_replay_gain_.reset();
    engine_consumed_.clear();
    engine_queue_revision_ = 0;
    view_->refreshTransport();
}


void Workspace::rememberEngineState(EnginePlayback* playback) {
    auto* engine = linkOf(playback);
    if (engine == nullptr) {
        return;
    }
    const auto state = playback->state();
    auto& seen = engine->seen;
    seen = SeenEngine{
        .status = state.status, .entry = state.entry, .queue_revision = state.queue_revision};
}


void Workspace::followIfStartedElsewhere(EnginePlayback* playback) {
    auto* engine = linkOf(playback);
    if (engine == nullptr || !playback->active()) {
        return;
    }
    const auto state = playback->state();
    auto& seen = engine->seen;
    // Started: playing where it was not, or on another entry of a queue
    // someone changed -- replaced, as a picker does. Moving on to the next
    // track of the same queue is not, or two engines playing at once would
    // take the window back and forth with every track.
    const bool playing = state.status == QStringLiteral("playing");
    // Told to stop by this window, it is not started elsewhere until it has
    // stopped: its reports on the way -- still playing, its queue already
    // emptied -- are the stop, not a start.
    if (seen.status == stopping_status && state.status != QStringLiteral("stopped")) {
        seen.entry = state.entry;
        seen.queue_revision = state.queue_revision;
        return;
    }
    // And playing nothing it can name is nothing to follow.
    const bool started =
        playing && !state.entry.isEmpty() &&
        (seen.status != QStringLiteral("playing") ||
         (seen.entry != state.entry && seen.queue_revision != state.queue_revision));
    seen = SeenEngine{
        .status = state.status, .entry = state.entry, .queue_revision = state.queue_revision};
    if (!started || playback == transport_) {
        return;
    }
    // Where the music is, as the window's own play would have done: the
    // list the entry came from if one is open, else the engine's queue as a
    // list of its own.
    followPlayback(playback, false);
    reattachToEngine();
}


bool Workspace::playingOnEngine() const {
    // Ownership, not visibility: whether an engine is connected, never which
    // tab is on screen. Deciding it from the visible tab once let the local
    // refresh see an idle player and wipe the anchors the engine was playing
    // from.
    return transport_ != nullptr && transport_->active();
}


int Workspace::resolvePlaybackRow(const ListTab* tab) const {
    if (tab == nullptr) {
        return -1;
    }
    const LocalListPlaybackView list{*tab->model};
    return playback_.resolveRow(list);
}


void Workspace::playRow(ListTab& tab, const int row) {
    // ADR-0227: a tab plays on the engine whose files it lists.
    auto* target = playbackOf(EngineKey::of(tab.document));
    if (target == nullptr || !target->active()) {
        view_->showMessage(!EngineKey::of(tab.document).isLocal()
                                     ? QStringLiteral("Nothing can play: the remote engine is "
                                                      "not connected")
                                     : QStringLiteral("Nothing can play: this computer's engine "
                                                      "is not running"),
                                 5'000);
        return;
    }
    followPlayback(target);
    // The engine owns the queue, so it is given the whole list rather than
    // one track: skipping, shuffling and gapless are its decisions, and it
    // cannot make them from a single entry.
    const auto& rows = tab.model->rows();
    if (row < 0 || static_cast<std::size_t>(row) >= rows.size()) {
        return;
    }
    std::vector<std::optional<formats::ReplayGainInfo>> overrides;
    overrides.reserve(rows.size());
    for (const auto& source_row : rows) {
        overrides.push_back(local_replay_gain_override(source_row));
    }
    transport_->play(rows, overrides, rows[static_cast<std::size_t>(row)].entry_id);
    if (playback_.anchors.document != tab.document.id) {
        if (auto* previous = tabForDocument(playback_.anchors.document); previous != nullptr) {
            previous->model->setCurrentSource({}, -1);
        }
    }
    playback_.anchors.document = tab.document.id;
    playback_.anchors.current = rows[static_cast<std::size_t>(row)].entry_id;
    playback_.row = row;
    setActiveLocalList(QString::fromStdString(tab.document.id.to_string()));
    tab.model->setCurrentSource(tab.model->source(row), row);
}


void Workspace::togglePlayPause() {
    if (!playingOnEngine()) {
        return;
    }
    if (transport_->state().status == QStringLiteral("playing")) {
        transport_->pause();
    } else {
        transport_->resume();
    }
}


void Workspace::seekToMs(const qint64 position_ms) {
    if (playingOnEngine()) {
        transport_->seek(position_ms);
    }
}


const LocalTrackRow* Workspace::playingRow(const QString& entry) {
    const auto identity = core::StableId::parse(entry.toStdString());
    if (!identity) {
        return nullptr;
    }
    const auto in = [&identity](const std::vector<LocalTrackRow>& rows) -> const LocalTrackRow* {
        const auto found = std::ranges::find(rows, *identity, &LocalTrackRow::entry_id);
        return found != rows.end() ? &*found : nullptr;
    };
    // The list it was played from, first: the header reads as the row does.
    if (auto* tab = tabForDocument(playback_.anchors.document); tab != nullptr) {
        if (const auto* row = in(tab->model->rows())) {
            return row;
        }
    }
    // Up Next: an ask is often from another list, the library or a search,
    // and it leaves Up Next as it starts -- so the one playing is looked for
    // there as well as among those waiting.
    if (const auto& active = playback_.requests.active();
        active && active->source.entry_id == *identity) {
        return &active->source;
    }
    for (const auto& waiting : playback_.requests.pending()) {
        if (waiting.source.entry_id == *identity) {
            return &waiting.source;
        }
    }
    for (const auto& tab : list_tabs_) {
        if (const auto* row = in(tab->model->rows())) {
            return row;
        }
    }
    return nullptr;
}


Workspace::EngineLink* Workspace::linkOf(const EnginePlayback* playback) const {
    if (playback == nullptr) {
        return nullptr;
    }
    const auto found =
        std::ranges::find(engines_, playback, [](const auto& each) { return each->playback; });
    return found != engines_.end() ? found->get() : nullptr;
}


Workspace::ListTab* Workspace::remoteQueueTab() {
    auto* remote = remoteEngine();
    return remote != nullptr ? engineTab(*remote) : nullptr;
}


Workspace::ListTab* Workspace::engineTab(EngineLink& engine) {
    // Its own, by its key: a list of an engine the address led to before
    // is not its tab.
    for (const auto& tab : list_tabs_) {
        if (EngineKey::of(tab->document) == engine.key) {
            return tab.get();
        }
    }
    if (engine.catalogue == nullptr) {
        return nullptr;
    }
    // Opened on first connection, named after the engine so it reads as a
    // place rather than a list.
    auto* tab = addList(persistence::ListDocument{.id = core::StableId::random(),
                                                     .kind = persistence::ListKind::scratch,
                                                     .name = utf8Bytes(engine.catalogue->name()),
                                                     .pinned = false,
                                                     .dirty = false,
                                                     .items = {},
                                                     .engine = engine.key.stored()},
                           false);
    schedulePersist();
    return tab;
}


} // namespace trackknife::bench
