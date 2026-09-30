// SPDX-License-Identifier: GPL-3.0-only

#include "quick/quick_identify.hpp"

namespace trackknife::quick {

QuickIdentify::QuickIdentify(bench::IdentifySession* session, QObject* parent)
    : QObject(parent), session_(session) {
    session_->setParent(this);
    connect(session_, &bench::IdentifySession::changed, this, [this] {
        const auto count = static_cast<int>(session_->candidates().size());
        if (count != candidate_count_) {
            candidate_count_ = count;
            emit candidatesChanged();
        }
        emit changed();
    });
    connect(session_, &bench::IdentifySession::matchOpened, this,
            [this](bench::TrackMatchSession* match) {
                match_ = match;
                connect(match, &bench::TrackMatchSession::changed, this,
                        &QuickIdentify::matchChanged);
                emit matchChanged(-1);
                emit changed();
            });
    connect(session_, &bench::IdentifySession::matchClosed, this, [this] {
        match_ = nullptr;
        emit matchChanged(-1);
        emit changed();
    });
    connect(session_, &bench::IdentifySession::accepted, this, &QuickIdentify::accepted);
}

QVariantMap QuickIdentify::state() const {
    return {{QStringLiteral("status"), session_->status()},
            {QStringLiteral("busy"), session_->busy()},
            {QStringLiteral("canSearch"), session_->canSearch()},
            {QStringLiteral("canScan"), session_->canScan()},
            {QStringLiteral("suggested"), session_->suggested()}};
}

QVariantList QuickIdentify::candidates() const {
    QVariantList rows;
    for (const auto& candidate : session_->candidates()) {
        rows.push_back(QVariantMap{{QStringLiteral("match"), candidate.match},
                                   {QStringLiteral("matchToolTip"), candidate.match_tool_tip},
                                   {QStringLiteral("album"), candidate.album},
                                   {QStringLiteral("albumToolTip"), candidate.album_tool_tip},
                                   {QStringLiteral("artist"), candidate.artist},
                                   {QStringLiteral("tracks"), candidate.tracks},
                                   {QStringLiteral("media"), candidate.media},
                                   {QStringLiteral("version"), candidate.version}});
    }
    return rows;
}

QVariantMap QuickIdentify::match() const {
    if (match_ == nullptr) {
        return {};
    }
    return {{QStringLiteral("heading"), match_->heading()},
            {QStringLiteral("help"), bench::TrackMatchSession::help()},
            {QStringLiteral("status"), match_->status()},
            {QStringLiteral("ready"), match_->ready()},
            {QStringLiteral("canStage"), match_->canStage()}};
}

QVariantList QuickIdentify::matchRows() const {
    QVariantList rows;
    if (match_ == nullptr) {
        return rows;
    }
    for (const auto& row : match_->rows()) {
        rows.push_back(QVariantMap{{QStringLiteral("file"), row.file},
                                   {QStringLiteral("fileToolTip"), row.file_tool_tip},
                                   {QStringLiteral("length"), row.length},
                                   {QStringLiteral("pairing"), row.pairing},
                                   {QStringLiteral("pairingToolTip"), row.pairing_tool_tip},
                                   {QStringLiteral("paired"), row.paired},
                                   {QStringLiteral("track"), row.track},
                                   {QStringLiteral("trackLength"), row.track_length}});
    }
    return rows;
}

bool QuickIdentify::canMoveUp(const int row) const {
    return match_ != nullptr && match_->canMoveUp(row);
}

bool QuickIdentify::canMoveDown(const int row) const {
    return match_ != nullptr && match_->canMoveDown(row);
}

bool QuickIdentify::canUnmatch(const int row) const {
    return match_ != nullptr && match_->canUnmatch(row);
}

void QuickIdentify::move(const int row, const int direction) {
    if (match_ != nullptr) {
        match_->move(row, direction);
    }
}

void QuickIdentify::moveFile(const int from, const int to) {
    if (match_ != nullptr && from >= 0 && to >= 0) {
        match_->moveFile(static_cast<std::size_t>(from), static_cast<std::size_t>(to));
    }
}

void QuickIdentify::unmatch(const int row) {
    if (match_ != nullptr) {
        match_->unmatch(row);
    }
}

void QuickIdentify::resetOrder(const bool by_filename) {
    if (match_ != nullptr) {
        match_->resetOrder(by_filename);
    }
}

void QuickIdentify::stage() {
    if (match_ != nullptr) {
        match_->stage();
    }
}

} // namespace trackknife::quick
