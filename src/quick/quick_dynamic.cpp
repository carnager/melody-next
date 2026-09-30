// SPDX-License-Identifier: GPL-3.0-only

#include "quick/quick_dynamic.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "bench/local_list_model.hpp"
#include "quick/quick_workspace.hpp"
#include "uicommon/track_row_roles.hpp"

#include <algorithm>

namespace trackknife::quick {

QuickDynamic::QuickDynamic(bench::DynamicPlaylistSession* session, QuickWorkspace& workspace,
                           QObject* parent)
    : QObject(parent), session_(session), workspace_(workspace) {
    session_->setParent(this);
    connect(session_, &bench::DynamicPlaylistSession::changed, this, &QuickDynamic::changed);
    connect(session_, &bench::DynamicPlaylistSession::catalogChanged, this,
            &QuickDynamic::catalogChanged);
    connect(session_, &bench::DynamicPlaylistSession::resultsChanged, this, [this] {
        markPlaying();
        emit resultsChanged();
    });
    // The playing track marked among the results, as it moves.
    markers_.setInterval(500);
    connect(&markers_, &QTimer::timeout, this, &QuickDynamic::markPlaying);
    markers_.start();
}

QVariantList QuickDynamic::sources() {
    QVariantList choices;
    for (const auto& choice : bench::DynamicPlaylistSession::sources()) {
        choices.append(QVariantMap{{QStringLiteral("label"), choice.label},
                                   {QStringLiteral("value"), choice.value}});
    }
    return choices;
}

QVariantMap QuickDynamic::state() const {
    return {{QStringLiteral("library"), session_->library()},
            {QStringLiteral("catalogIndex"), session_->catalogIndex()},
            {QStringLiteral("writable"), session_->catalogWritable()},
            {QStringLiteral("name"), session_->name()},
            {QStringLiteral("source"), session_->source()},
            {QStringLiteral("query"), session_->query()},
            {QStringLiteral("artist"), session_->artist()},
            {QStringLiteral("track"), session_->track()},
            {QStringLiteral("user"), session_->user()},
            {QStringLiteral("tag"), session_->tag()},
            {QStringLiteral("limit"), session_->limit()},
            {QStringLiteral("shuffle"), session_->shuffle()},
            {QStringLiteral("shuffleText"), session_->shuffleText()},
            {QStringLiteral("shuffleTip"), session_->shuffleTip()},
            {QStringLiteral("status"), session_->status()},
            {QStringLiteral("canRefresh"), session_->canRefresh()},
            {QStringLiteral("canOpen"), session_->canOpen()}};
}

QVariantList QuickDynamic::results() const {
    QVariantList rows;
    const auto* model = session_->results();
    for (int row = 0; row < model->rowCount(); ++row) {
        const auto& track = model->rows()[static_cast<std::size_t>(row)];
        rows.append(QVariantMap{
            {QStringLiteral("number"), QString::fromStdString(track.track_number)},
            {QStringLiteral("title"), bench::displayText(track.title)},
            {QStringLiteral("artist"), bench::displayText(track.artist)},
            {QStringLiteral("album"), bench::displayText(track.album)},
            {QStringLiteral("date"), bench::displayText(track.date)},
            {QStringLiteral("length"),
             track.duration_ms ? bench::formatTime(*track.duration_ms) : QString{}},
            {QStringLiteral("playing"), row == playing_}});
    }
    return rows;
}

void QuickDynamic::markPlaying() {
    auto* model = session_->results();
    workspace_.workspace().markPlaying(*model, playback_context_);
    int playing = -1;
    for (int row = 0; row < model->rowCount(); ++row) {
        if (model->index(row, 0).data(ui::track_current_role).toBool()) {
            playing = row;
            break;
        }
    }
    if (playing != playing_) {
        playing_ = playing;
        emit resultsChanged();
    }
}

std::vector<int> QuickDynamic::rowsOf(const QVariantList& rows) const {
    std::vector<int> chosen;
    for (const auto& row : rows) {
        const auto value = row.toInt();
        if (value >= 0 && value < session_->results()->rowCount()) {
            chosen.push_back(value);
        }
    }
    std::ranges::sort(chosen);
    return chosen;
}

void QuickDynamic::play(const int row) {
    const auto& tracks = session_->tracks();
    if (!session_->authorityValid() || row < 0 || static_cast<std::size_t>(row) >= tracks.size()) {
        return;
    }
    const auto name = session_->playlistName().isEmpty()
                          ? tr("Dynamic playback")
                          : session_->playlistName() + tr(" — playback");
    auto& work = workspace_.workspace();
    auto* destination = work.openDynamicResult(name, tracks, session_->engine(), false);
    playback_context_ = bench::document_text(destination->document.id);
    work.playRow(*destination, row);
}

void QuickDynamic::openSnapshot() {
    const auto name = session_->playlistName();
    workspace_.workspace().openDynamicResult(
        name.isEmpty() ? QStringLiteral("Dynamic playlist snapshot") : name, session_->tracks(),
        session_->engine(), true);
}

namespace {

[[nodiscard]] QuickWorkspace::Picked picked(bench::DynamicPlaylistSession& session,
                                            std::vector<int> rows) {
    const auto current = rows.empty() ? -1 : rows.front();
    return {.tab = nullptr,
            .model = session.results(),
            .engine = session.engine(),
            .rows = std::move(rows),
            .current = current};
}

} // namespace

void QuickDynamic::queue(const QVariantList& rows, const bool next) {
    workspace_.queueOf(picked(*session_, rowsOf(rows)), next);
}

void QuickDynamic::drag(const QVariantList& rows) {
    workspace_.dragPicked(picked(*session_, rowsOf(rows)), true);
}

void QuickDynamic::editTags(const QVariantList& rows) {
    workspace_.editTagsOf(picked(*session_, rowsOf(rows)));
}

void QuickDynamic::replayGain(const QVariantList& rows) {
    workspace_.replayGainOf(picked(*session_, rowsOf(rows)));
}

void QuickDynamic::convertFiles(const QVariantList& rows) {
    workspace_.convertFilesOf(picked(*session_, rowsOf(rows)));
}

void QuickDynamic::locate(const int row, const bool album) {
    workspace_.locateOf(picked(*session_, rowsOf({row})), album);
}

void QuickDynamic::copyToNewTab(const QVariantList& rows, const QString& name) {
    workspace_.transferToNewTabOf(picked(*session_, rowsOf(rows)), name, false);
}

void QuickDynamic::copyTo(const QVariantList& rows, const QString& list_id) {
    workspace_.transferOf(picked(*session_, rowsOf(rows)), list_id, false);
}

QVariantMap QuickDynamic::ratingState(const QVariantList& rows) const {
    return workspace_.ratingStateOf(picked(*session_, rowsOf(rows)));
}

void QuickDynamic::rate(const QVariantList& rows, const bool album, const int rating) {
    workspace_.rateOf(picked(*session_, rowsOf(rows)), album, rating);
}

QVariantMap QuickDynamic::lastFmTrack(const QVariantList& rows) const {
    return workspace_.lastFmTrackOf(picked(*session_, rowsOf(rows)));
}

void QuickDynamic::askLastFm(const QVariantList& rows) {
    workspace_.askLastFmOf(lastFmTrack(rows));
}

void QuickDynamic::loveOnLastFm(const QVariantList& rows, const bool love) {
    workspace_.loveOnLastFmOf(lastFmTrack(rows), love);
}

} // namespace trackknife::quick
