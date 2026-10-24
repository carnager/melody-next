// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/identify_session.hpp"

#include "trackknife/musicbrainz/acoustid.hpp"
#include "trackknife/musicbrainz/web_service.hpp"

#include <QCollator>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QLocale>
#include <QStringList>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <utility>

namespace trackknife::bench {

namespace {

[[nodiscard]] QString text(const std::string& value) { return QString::fromUtf8(value); }

[[nodiscard]] QString duration(const std::optional<std::int64_t> milliseconds) {
    if (!milliseconds || *milliseconds < 0) {
        return QStringLiteral("—");
    }
    const auto seconds = *milliseconds / 1'000;
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char{'0'});
}

[[nodiscard]] QString version_text(const musicbrainz::Release& release) {
    QStringList parts;
    for (const auto& part : {release.date, release.country, release.disambiguation, release.label,
                             release.catalog_number}) {
        if (!part.empty()) {
            parts.push_back(text(part));
        }
    }
    return parts.join(QStringLiteral(" · "));
}

[[nodiscard]] QString media_text(const musicbrainz::Release& release) {
    QStringList formats;
    for (const auto& medium : release.media) {
        if (!medium.format.empty()) {
            formats.push_back(text(medium.format));
        }
    }
    formats.removeDuplicates();
    auto result = formats.join(QStringLiteral(" + "));
    if (release.media.size() > 1U) {
        result += QStringLiteral(" × %1").arg(release.media.size());
    }
    return result;
}

[[nodiscard]] QString credit_text(const std::vector<musicbrainz::ArtistCredit>& credits) {
    std::string joined;
    for (const auto& credit : credits) {
        joined += credit.name;
        joined += credit.join_phrase;
    }
    return text(joined);
}

} // namespace

// Matching.

TrackMatchSession::TrackMatchSession(musicbrainz::Release release,
                                     std::vector<musicbrainz::LocalTrackDescriptor> local_tracks,
                                     std::vector<QString> local_paths,
                                     std::vector<std::size_t> item_indexes, QObject* parent,
                                     std::vector<QString> other_albums)
    : QObject(parent), release_(std::move(release)), local_tracks_(std::move(local_tracks)),
      local_paths_(std::move(local_paths)), item_indexes_(std::move(item_indexes)),
      other_albums_(std::move(other_albums)),
      own_count_(local_tracks_.size() - std::min(other_albums_.size(), local_tracks_.size())),
      status_(tr("Preparing suggested matches…")) {
    std::size_t release_track_count = 0;
    for (const auto& medium : release_.media) {
        release_track_count += medium.tracks.size();
    }
    if (local_tracks_.size() > 2'000U || release_track_count > 2'000U ||
        local_tracks_.size() != item_indexes_.size()) {
        status_ = tr("Track matching supports up to 2,000 local files and 2,000 release "
                     "tracks. Choose a smaller selection.");
        return;
    }
    // Keep the potentially expensive similarity matcher off the UI thread.
    auto* watcher = new QFutureWatcher<musicbrainz::ReleaseAlignment>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        alignment_ = watcher->result();
        watcher->deleteLater();
        // Those offered were not aligned: unplaced, until the person
        // places one.
        for (auto local = own_count_; local < local_tracks_.size(); ++local) {
            alignment_.tracks.push_back(musicbrainz::TrackAlignment{.local_index = local,
                                                                    .release_track_index = {},
                                                                    .confidence = 0.0,
                                                                    .user_confirmed = false});
        }
        slots_.resize(alignment_.release_tracks.size());
        std::vector<bool> placed(local_tracks_.size(), false);
        for (std::size_t local = 0; local < own_count_; ++local) {
            const auto& match = alignment_.tracks[local];
            if (match.confidence >= 0.5 && match.release_track_index &&
                *match.release_track_index < slots_.size() && !slots_[*match.release_track_index]) {
                slots_[*match.release_track_index] = local;
                placed[local] = true;
            }
        }
        // Fill remaining gaps in file order as reviewable proposals, not
        // confidence claims.
        std::size_t gap = 0;
        for (std::size_t local = 0; local < own_count_; ++local) {
            if (placed[local]) {
                continue;
            }
            while (gap < slots_.size() && slots_[gap]) {
                ++gap;
            }
            if (gap == slots_.size()) {
                slots_.push_back(local);
            } else {
                slots_[gap] = local;
            }
        }
        for (auto local = own_count_; local < local_tracks_.size(); ++local) {
            slots_.push_back(local);
        }
        ready_ = true;
        refresh();
    });
    // Only the album's own files are aligned: one offered never takes a
    // track from them.
    std::vector own(local_tracks_.begin(),
                    local_tracks_.begin() + static_cast<std::ptrdiff_t>(own_count_));
    watcher->setFuture(QtConcurrent::run(
        [release = release_, tracks = std::move(own), token = cancellation_.token()] {
            return musicbrainz::align_release_tracks(tracks, release, token);
        }));
}

TrackMatchSession::~TrackMatchSession() { cancellation_.request_cancellation(); }

QString TrackMatchSession::heading() const {
    return tr("Match files to %1 · %2 · %3")
        .arg(text(release_.title), text(release_.date), text(release_.country));
}

QString TrackMatchSession::help() {
    return tr("Each row pairs a local file with the MusicBrainz track beside it. "
              "Drag a file onto a MusicBrainz track to put it there, drag files in the "
              "left pane, or use Move file up/down to change pairings. Files of other "
              "albums are listed last; one put on a track joins this album. "
              "MusicBrainz tracks stay in album order. Review before staging.");
}

QString TrackMatchSession::localPath(const std::size_t local) const {
    if (local < local_paths_.size() && !local_paths_[local].isEmpty()) {
        return local_paths_[local];
    }
    return local_tracks_[local].title.empty() ? tr("File %1").arg(local + 1)
                                              : text(local_tracks_[local].title);
}

QString TrackMatchSession::trackLabel(const std::size_t target) const {
    const auto& track = alignment_.release_tracks[target];
    return tr("%1.%2 · %3")
        .arg(track.medium_position)
        .arg(track.track.position)
        .arg(text(track.track.title));
}

bool TrackMatchSession::canMoveUp(const int row) const { return ready_ && row > 0; }

bool TrackMatchSession::canMoveDown(const int row) const {
    return ready_ && row >= 0 && static_cast<std::size_t>(row) + 1 < slots_.size();
}

bool TrackMatchSession::canUnmatch(const int row) const {
    return ready_ && row >= 0 && static_cast<std::size_t>(row) < alignment_.release_tracks.size() &&
           static_cast<std::size_t>(row) < slots_.size() &&
           slots_[static_cast<std::size_t>(row)].has_value();
}

bool TrackMatchSession::canStage() const {
    return ready_ &&
           std::ranges::any_of(assignments_, [](const auto& value) { return value.has_value(); });
}

void TrackMatchSession::moveFile(const std::size_t from, const std::size_t to) {
    if (!ready_ || from >= slots_.size() || to >= slots_.size() || from == to) {
        return;
    }
    const auto local = slots_[from];
    slots_.erase(slots_.begin() + static_cast<std::ptrdiff_t>(from));
    slots_.insert(slots_.begin() + static_cast<std::ptrdiff_t>(to), local);
    refresh(to);
}

bool TrackMatchSession::isGap(const std::size_t row) const {
    return ready_ && row < alignment_.release_tracks.size() && row < slots_.size() &&
           !slots_[row].has_value();
}

void TrackMatchSession::fillGap(const std::size_t from, const std::size_t gap) {
    if (!isGap(gap) || from >= slots_.size() || !slots_[from]) {
        return;
    }
    slots_[gap] = slots_[from];
    if (from < alignment_.release_tracks.size()) {
        slots_[from].reset();
    } else {
        slots_.erase(slots_.begin() + static_cast<std::ptrdiff_t>(from));
    }
    refresh(gap);
}

void TrackMatchSession::placeOn(const std::size_t from, const std::size_t track) {
    if (!ready_ || from >= slots_.size() || !slots_[from] || from == track ||
        track >= alignment_.release_tracks.size()) {
        return;
    }
    if (isGap(track)) {
        fillGap(from, track);
        return;
    }
    std::swap(slots_[from], slots_[track]);
    refresh(track);
}

void TrackMatchSession::move(const int row, const int direction) {
    if (!ready_ || row < 0 || (direction < 0 && row == 0) ||
        (direction > 0 && static_cast<std::size_t>(row) + 1 >= slots_.size())) {
        return;
    }
    const auto at = static_cast<std::size_t>(row);
    const auto target = direction < 0 ? at - 1 : at + 1;
    std::swap(slots_[at], slots_[target]);
    refresh(target);
}

void TrackMatchSession::unmatch(const int row) {
    if (!canUnmatch(row)) {
        return;
    }
    const auto at = static_cast<std::size_t>(row);
    const auto local = slots_[at];
    slots_[at].reset();
    slots_.push_back(local);
    refresh(slots_.size() - 1);
}

void TrackMatchSession::resetOrder(const bool by_filename) {
    if (!ready_) {
        return;
    }
    std::vector<std::size_t> files;
    files.reserve(own_count_);
    for (std::size_t local = 0; local < own_count_; ++local) {
        files.push_back(local);
    }
    if (by_filename) {
        QCollator collator;
        if (collator.locale().language() == QLocale::C) {
            collator.setLocale(QLocale{QLocale::English});
        }
        collator.setNumericMode(true);
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        std::stable_sort(files.begin(), files.end(), [this, &collator](auto a, auto b) {
            return collator.compare(QFileInfo(localPath(a)).fileName(),
                                    QFileInfo(localPath(b)).fileName()) < 0;
        });
    }
    slots_.assign(std::max(files.size(), alignment_.release_tracks.size()), std::nullopt);
    for (std::size_t row = 0; row < files.size(); ++row) {
        slots_[row] = files[row];
    }
    for (auto local = own_count_; local < local_tracks_.size(); ++local) {
        slots_.push_back(local);
    }
    refresh(0);
}

void TrackMatchSession::refresh(const std::optional<std::size_t> selected) {
    while (slots_.size() > alignment_.release_tracks.size() && !slots_.back()) {
        slots_.pop_back();
    }
    assignments_.assign(local_tracks_.size(), std::nullopt);
    rows_.clear();
    rows_.reserve(slots_.size());
    std::size_t count = 0;
    std::size_t taken = 0;
    for (std::size_t row = 0; row < slots_.size(); ++row) {
        Row entry;
        if (const auto local = slots_[row]) {
            entry.file = QFileInfo(localPath(*local)).fileName();
            entry.file_tool_tip = localPath(*local);
            entry.length = duration(local_tracks_[*local].duration_ms);
            if (row < alignment_.release_tracks.size()) {
                assignments_[*local] = row;
                const auto& track = alignment_.release_tracks[row];
                entry.pairing = tr("→ %1.%2").arg(track.medium_position).arg(track.track.position);
                entry.pairing_tool_tip =
                    tr("Paired with %1. Review before staging.").arg(trackLabel(row));
                entry.paired = true;
                if (*local >= own_count_) {
                    ++taken;
                    entry.pairing_tool_tip +=
                        tr(" Taken from %1.").arg(other_albums_[*local - own_count_]);
                } else {
                    ++count;
                }
            }
        } else {
            entry.file = tr("No local file");
            entry.pairing = tr("— Gap");
            entry.gap = true;
        }
        if (row < alignment_.release_tracks.size()) {
            entry.track = trackLabel(row);
            entry.track_length = duration(alignment_.release_tracks[row].track.length_ms);
        } else if (const auto local = slots_[row]; local && *local >= own_count_) {
            const auto& album = other_albums_[*local - own_count_];
            entry.other = true;
            entry.pairing = tr("Other album");
            entry.pairing_tool_tip =
                tr("In %1. Drop it on a track to move it to this album.").arg(album);
            entry.track = tr("— Stays in %1").arg(album);
        } else {
            entry.pairing = tr("Unmatched");
            entry.track = tr("— No tags will be staged");
        }
        rows_.push_back(std::move(entry));
    }
    paired_count_ = count + taken;
    status_ = tr("%1 files paired · %2 files unmatched · %3 album tracks without a file. "
                 "Pairings are suggestions until you Stage matches.")
                  .arg(count + taken)
                  .arg(own_count_ - count)
                  .arg(alignment_.release_tracks.size() - count - taken);
    if (taken > 0U) {
        status_ += tr(" %n taken from other albums.", nullptr, static_cast<int>(taken));
    }
    emit changed(selected && !slots_.empty()
                     ? static_cast<int>(std::min(*selected, slots_.size() - 1))
                     : -1);
}

void TrackMatchSession::stage() {
    if (!canStage()) {
        return;
    }
    auto confirmed = musicbrainz::confirm_release_mapping(alignment_, assignments_);
    if (!confirmed) {
        status_ = text(confirmed.error().message);
        emit changed(-1);
        return;
    }
    auto proposals = musicbrainz::release_metadata_proposals(release_, *confirmed, item_indexes_);
    if (!proposals) {
        status_ = text(proposals.error().message);
        emit changed(-1);
        return;
    }
    emit accepted(std::move(*proposals));
}

// Identifying.

IdentifySession::IdentifySession(MusicBrainzLookupService service,
                                 std::vector<musicbrainz::LocalTrackDescriptor> local_tracks,
                                 std::vector<QString> local_paths,
                                 std::vector<std::size_t> item_indexes, QString initial_artist,
                                 QString initial_release, QString tagged_release,
                                 QObject* parent)
    : QObject(parent), service_(std::move(service)), local_tracks_(std::move(local_tracks)),
      local_paths_(std::move(local_paths)), item_indexes_(std::move(item_indexes)),
      initial_artist_(std::move(initial_artist)), initial_release_(std::move(initial_release)),
      tagged_release_(std::move(tagged_release)),
      status_(QStringLiteral("Searches MusicBrainz by text — no MusicBrainz tags are needed")) {}

IdentifySession::~IdentifySession() = default;

bool IdentifySession::canScan() const {
    return !busy_ && static_cast<bool>(service_.fingerprint) &&
           static_cast<bool>(service_.acoustid_lookup) && !local_paths_.empty();
}

void IdentifySession::setBusy(const bool busy) {
    busy_ = busy;
    emit changed();
}

// AcoustID identification (ADR-0096): fingerprint every selected file, vote
// releases by how many files matched a recording on them, then load the top
// candidates into the same version picker.
void IdentifySession::scan() {
    if (!canScan()) {
        return;
    }
    dropMatch();
    rows_.clear();
    row_candidates_.clear();
    candidates_.clear();
    suggested_ = -1;
    scan_votes_.clear();
    scan_matched_files_ = 0U;
    setBusy(true);
    scanFile(0U);
}

void IdentifySession::scanFile(const std::size_t position) {
    if (position >= local_paths_.size()) {
        finishScan();
        return;
    }
    status_ =
        QStringLiteral("Fingerprinting file %1 of %2…").arg(position + 1U).arg(local_paths_.size());
    emit changed();
    const QPointer self{this};
    service_.fingerprint(local_paths_[position], [self, position](core::Result<AcoustIdFingerprint>
                                                                      fingerprint) {
        if (self.isNull()) {
            return;
        }
        if (!fingerprint) {
            self->status_ =
                QStringLiteral("Fingerprinting failed · %1").arg(text(fingerprint.error().message));
            self->setBusy(false);
            return;
        }
        self->status_ = QStringLiteral("Looking up file %1 of %2…")
                            .arg(position + 1U)
                            .arg(self->local_paths_.size());
        emit self->changed();
        self->service_.acoustid_lookup(*fingerprint, [self,
                                                      position](core::Result<QByteArray> body) {
            if (self.isNull()) {
                return;
            }
            if (!body) {
                self->status_ =
                    QStringLiteral("AcoustID lookup failed · %1").arg(text(body.error().message));
                self->setBusy(false);
                return;
            }
            const auto lookup = musicbrainz::parse_acoustid_lookup(
                std::string_view{body->constData(), static_cast<std::size_t>(body->size())});
            if (!lookup) {
                self->status_ =
                    QStringLiteral("AcoustID lookup failed · %1").arg(text(lookup.error().message));
                self->setBusy(false);
                return;
            }
            auto matched = false;
            for (const auto& result : lookup->results) {
                if (result.score < 0.5) {
                    continue;
                }
                for (const auto& recording : result.recordings) {
                    for (const auto& release_id : recording.release_ids) {
                        self->scan_votes_[release_id].insert(position);
                        matched = true;
                    }
                }
            }
            if (matched) {
                ++self->scan_matched_files_;
            }
            self->scanFile(position + 1U);
        });
    });
}

void IdentifySession::finishScan() {
    if (scan_votes_.empty()) {
        status_ = QStringLiteral(
            "No AcoustID matches — try the text search, or the files may be unsubmitted");
        setBusy(false);
        return;
    }
    std::vector<std::pair<std::string, std::size_t>> ranked;
    ranked.reserve(scan_votes_.size());
    for (const auto& [release_id, files] : scan_votes_) {
        ranked.emplace_back(release_id, files.size());
    }
    std::ranges::stable_sort(
        ranked, [](const auto& left, const auto& right) { return left.second > right.second; });
    constexpr std::size_t maximum_candidates = 5U;
    if (ranked.size() > maximum_candidates) {
        ranked.resize(maximum_candidates);
    }
    scan_candidates_ = std::move(ranked);
    loadScanCandidate(0U);
}

void IdentifySession::loadScanCandidate(const std::size_t rank) {
    if (rank >= scan_candidates_.size()) {
        if (candidates_.empty()) {
            status_ = QStringLiteral("No AcoustID candidate could be loaded");
        } else {
            status_ =
                QStringLiteral(
                    "%1 fingerprint %2 · matched %3 of %4 files · every version is its own row")
                    .arg(candidates_.size())
                    .arg(candidates_.size() == 1U ? QStringLiteral("candidate")
                                                  : QStringLiteral("candidates"))
                    .arg(scan_matched_files_)
                    .arg(local_paths_.size());
            suggested_ = 0;
        }
        setBusy(false);
        return;
    }
    status_ =
        QStringLiteral("Loading candidate %1 of %2…").arg(rank + 1U).arg(scan_candidates_.size());
    emit changed();
    const auto url = musicbrainz::build_release_lookup_url(scan_candidates_[rank].first);
    if (!url) {
        loadScanCandidate(rank + 1U);
        return;
    }
    const QPointer self{this};
    service_.fetch(QString::fromStdString(*url), [self, rank](core::Result<QByteArray> body) {
        if (self.isNull()) {
            return;
        }
        if (body) {
            auto release = musicbrainz::parse_release_lookup(
                std::string_view{body->constData(), static_cast<std::size_t>(body->size())});
            if (release) {
                const auto matched = self->scan_candidates_[rank].second;
                self->row_candidates_.push_back(self->candidates_.size());
                self->rows_.push_back(Candidate{
                    .match =
                        QStringLiteral("%1/%2 files").arg(matched).arg(self->local_paths_.size()),
                    .match_tool_tip = {},
                    .album = text(release->title),
                    .album_tool_tip = text(release->id),
                    .artist = credit_text(release->artist_credits),
                    .tracks = QString::number(release->track_count),
                    .media = media_text(*release),
                    .version = version_text(*release),
                });
                self->candidates_.push_back(std::move(*release));
            }
        }
        self->loadScanCandidate(rank + 1U);
    });
}

void IdentifySession::start() {
    if (const auto id = musicbrainz::release_id_in(tagged_release_.toStdString())) {
        lookUp(*id, true);
    }
}

void IdentifySession::search(const QString& artist, const QString& release) {
    if (busy_ || !service_.fetch) {
        return;
    }
    // A link or id pasted in either field: that release -- or that
    // album's versions -- not a search.
    for (const auto* named : {&release, &artist}) {
        if (const auto id = musicbrainz::release_id_in(named->toStdString())) {
            lookUp(*id, false);
            return;
        }
        if (const auto group = musicbrainz::release_group_id_in(named->toStdString())) {
            lookUpGroup(*group, Named::group);
            return;
        }
    }
    const auto url = musicbrainz::build_release_search_url(musicbrainz::ReleaseSearchQuery{
        .artist = artist.trimmed().toStdString(),
        .release = release.trimmed().toStdString(),
        .track_count = std::nullopt,
        .limit = 25U,
    });
    if (!url) {
        status_ = QStringLiteral("Enter an artist or an album to search");
        emit changed();
        return;
    }
    status_ = QStringLiteral("Searching MusicBrainz…");
    dropMatch();
    rows_.clear();
    row_candidates_.clear();
    candidates_.clear();
    suggested_ = -1;
    setBusy(true);
    const QPointer self{this};
    service_.fetch(QString::fromStdString(*url), [self](core::Result<QByteArray> body) {
        if (!self.isNull()) {
            self->finishSearch(std::move(body));
        }
    });
}

void IdentifySession::finishSearch(core::Result<QByteArray> body) {
    busy_ = false;
    if (!body) {
        status_ = QStringLiteral("Search failed · %1").arg(text(body.error().message));
        emit changed();
        return;
    }
    auto parsed = musicbrainz::parse_release_search(
        std::string_view{body->constData(), static_cast<std::size_t>(body->size())});
    if (!parsed) {
        status_ = QStringLiteral("Search failed · %1").arg(text(parsed.error().message));
        emit changed();
        return;
    }
    const auto ranked = musicbrainz::rank_release_candidates(local_tracks_, *parsed);
    candidates_ = std::move(parsed->releases);
    for (const auto& entry : ranked) {
        const auto& release = candidates_[entry.release_index];
        row_candidates_.push_back(entry.release_index);
        rows_.push_back(Candidate{
            .match = release.track_count == local_tracks_.size()
                         ? QStringLiteral("Same track count")
                         : QStringLiteral("Different track count"),
            .match_tool_tip = QStringLiteral("Search relevance: %1/100 · ranking score: %2")
                                  .arg(release.search_score)
                                  .arg(entry.score),
            .album = text(release.title),
            .album_tool_tip = text(release.id),
            .artist = credit_text(release.artist_credits),
            .tracks = QString::number(release.track_count),
            .media = media_text(release),
            .version = version_text(release),
        });
    }
    if (candidates_.empty()) {
        status_ = QStringLiteral("No releases found — adjust the search text");
    } else {
        status_ = QStringLiteral("%1 release %2 · every version of an album is its own row")
                      .arg(candidates_.size())
                      .arg(candidates_.size() == 1U ? QStringLiteral("version")
                                                    : QStringLiteral("versions"));
        suggested_ = 0;
    }
    emit changed();
}

void IdentifySession::lookUp(const std::string& release_id, const bool tagged) {
    if (busy_ || !service_.fetch) {
        return;
    }
    const auto url = musicbrainz::build_release_lookup_url(release_id);
    if (!url) {
        return;
    }
    status_ = QStringLiteral("Looking up the release…");
    dropMatch();
    rows_.clear();
    row_candidates_.clear();
    candidates_.clear();
    suggested_ = -1;
    setBusy(true);
    const QPointer self{this};
    service_.fetch(QString::fromStdString(*url), [self, tagged](core::Result<QByteArray> body) {
        if (!self.isNull()) {
            self->finishReleaseLookup(std::move(body), tagged);
        }
    });
}

void IdentifySession::finishReleaseLookup(core::Result<QByteArray> body, const bool tagged) {
    auto release = body ? musicbrainz::parse_release_lookup(std::string_view{
                              body->constData(), static_cast<std::size_t>(body->size())})
                        : core::Result<musicbrainz::Release>{std::unexpected(body.error())};
    if (!release) {
        status_ = tagged ? QStringLiteral("The release the files are tagged with could not be "
                                          "looked up · %1 — search for it instead")
                               .arg(text(release.error().message))
                         : QStringLiteral("The release could not be looked up · %1")
                               .arg(text(release.error().message));
        setBusy(false);
        return;
    }
    row_candidates_.push_back(0U);
    rows_.push_back(Candidate{
        .match = tagged ? QStringLiteral("Tagged release") : QStringLiteral("Release named"),
        .match_tool_tip = release->track_count == local_tracks_.size()
                              ? QStringLiteral("Same track count")
                              : QStringLiteral("Different track count"),
        .album = text(release->title),
        .album_tool_tip = text(release->id),
        .artist = credit_text(release->artist_credits),
        .tracks = QString::number(release->track_count),
        .media = media_text(*release),
        .version = version_text(*release),
    });
    const auto group = release->release_group_id;
    candidates_.push_back(std::move(*release));
    suggested_ = 0;
    // The album's other versions beneath it, to change to.
    lookUpGroup(group, tagged ? Named::tagged : Named::release);
}

void IdentifySession::lookUpGroup(const std::string& group_id, const Named named) {
    const auto url = musicbrainz::build_release_group_releases_url(group_id);
    if (!url) {
        finishGroup(std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                                .message = "the release names no album",
                                                .context = {}}),
                    named);
        return;
    }
    if (named == Named::group) {
        status_ = QStringLiteral("Listing the album's versions…");
        dropMatch();
        rows_.clear();
        row_candidates_.clear();
        candidates_.clear();
        suggested_ = -1;
        setBusy(true);
    } else {
        status_ = QStringLiteral("Listing the album's other versions…");
        emit changed();
    }
    const QPointer self{this};
    service_.fetch(QString::fromStdString(*url), [self, named](core::Result<QByteArray> body) {
        if (!self.isNull()) {
            self->finishGroup(std::move(body), named);
        }
    });
}

void IdentifySession::finishGroup(core::Result<QByteArray> body, const Named named) {
    auto listed =
        body ? musicbrainz::parse_release_search(
                   std::string_view{body->constData(), static_cast<std::size_t>(body->size())},
                   musicbrainz::WebServiceLimits{
                       .releases = musicbrainz::release_group_releases_limit})
             : core::Result<musicbrainz::ReleaseSearchResult>{std::unexpected(body.error())};
    const auto first = named == Named::tagged
                           ? QStringLiteral("The release the files are tagged with")
                           : QStringLiteral("The release named");
    if (!listed) {
        status_ = named == Named::group
                      ? QStringLiteral("The album's versions could not be listed · %1")
                            .arg(text(listed.error().message))
                      : QStringLiteral("%1 · its album's other versions could not be listed · %2")
                            .arg(first, text(listed.error().message));
        setBusy(false);
        return;
    }
    // Those with as many tracks as files first, then by date.
    auto& releases = listed->releases;
    std::erase_if(releases, [this](const musicbrainz::Release& release) {
        return std::ranges::any_of(candidates_, [&release](const auto& candidate) {
            return candidate.id == release.id;
        });
    });
    std::ranges::stable_sort(releases, [this](const auto& left, const auto& right) {
        const auto left_fits = left.track_count == local_tracks_.size();
        const auto right_fits = right.track_count == local_tracks_.size();
        if (left_fits != right_fits) {
            return left_fits;
        }
        return (left.date.empty() ? std::string{"9999"} : left.date) <
               (right.date.empty() ? std::string{"9999"} : right.date);
    });
    const auto others = releases.size();
    for (auto& release : releases) {
        row_candidates_.push_back(candidates_.size());
        rows_.push_back(Candidate{
            .match = release.track_count == local_tracks_.size()
                         ? QStringLiteral("Same album · same track count")
                         : QStringLiteral("Same album"),
            .match_tool_tip = QStringLiteral("Another version of the album"),
            .album = text(release.title),
            .album_tool_tip = text(release.id),
            .artist = credit_text(release.artist_credits),
            .tracks = QString::number(release.track_count),
            .media = media_text(release),
            .version = version_text(release),
        });
        candidates_.push_back(std::move(release));
    }
    // Where there are more than one page lists.
    const auto more =
        listed->total_count > musicbrainz::release_group_releases_limit
            ? QStringLiteral(" · the first %1 of %2")
                  .arg(musicbrainz::release_group_releases_limit)
                  .arg(listed->total_count)
            : QString{};
    if (named == Named::group) {
        suggested_ = candidates_.empty() ? -1 : 0;
        status_ = candidates_.empty()
                      ? QStringLiteral("The album has no versions on MusicBrainz")
                      : QStringLiteral("%1 %2 of the album%3")
                            .arg(others)
                            .arg(others == 1U ? QStringLiteral("version")
                                              : QStringLiteral("versions"))
                            .arg(more);
    } else {
        status_ = QStringLiteral("%1, and %2 other %3 of its album%4%5")
                      .arg(first)
                      .arg(others)
                      .arg(others == 1U ? QStringLiteral("version") : QStringLiteral("versions"))
                      .arg(more,
                           named == Named::tagged ? QStringLiteral(" — search to look for another")
                                                  : QString{});
    }
    setBusy(false);
}

void IdentifySession::use(const int row) {
    if (busy_ || row < 0 || static_cast<std::size_t>(row) >= row_candidates_.size() ||
        !service_.fetch) {
        return;
    }
    const auto index = row_candidates_[static_cast<std::size_t>(row)];
    if (index >= candidates_.size()) {
        return;
    }
    // Said again once it has come.
    if (!status_.startsWith(QStringLiteral("Loading the release"))) {
        listed_status_ = status_;
    }
    loading_row_ = row;
    // Looked up already -- tagged, or named by its id: its tracks are here.
    if (std::ranges::any_of(candidates_[index].media,
                            [](const auto& medium) { return !medium.tracks.empty(); })) {
        opened(candidates_[index]);
        return;
    }
    const auto url = musicbrainz::build_release_lookup_url(candidates_[index].id);
    if (!url) {
        status_ = QStringLiteral("This candidate has no usable release id");
        emit changed();
        return;
    }
    status_ = QStringLiteral("Loading the release's track list…");
    setBusy(true);
    const QPointer self{this};
    service_.fetch(QString::fromStdString(*url), [self](core::Result<QByteArray> body) {
        if (!self.isNull()) {
            self->finishLookup(std::move(body));
        }
    });
}

void IdentifySession::finishLookup(core::Result<QByteArray> body) {
    busy_ = false;
    opened(body ? musicbrainz::parse_release_lookup(std::string_view{
                      body->constData(), static_cast<std::size_t>(body->size())})
                : core::Result<musicbrainz::Release>{std::unexpected(body.error())});
}

void IdentifySession::opened(core::Result<musicbrainz::Release> release) {
    // Another chosen meanwhile: that one next.
    const auto wanted = std::exchange(wanted_row_, std::nullopt);
    if (!release) {
        status_ = QStringLiteral("Loading failed · %1").arg(text(release.error().message));
        emit changed();
        if (wanted && *wanted != loading_row_) {
            use(*wanted);
        }
        return;
    }
    status_ = listed_status_;
    // The one shown before goes first, and whatever shows it.
    dropMatch();
    match_ = new TrackMatchSession(std::move(*release), local_tracks_, local_paths_, item_indexes_,
                                   this);
    matched_row_ = loading_row_;
    connect(match_, &TrackMatchSession::accepted, this, &IdentifySession::accepted);
    emit changed();
    emit matchOpened(match_);
    if (wanted && *wanted != matched_row_) {
        use(*wanted);
    }
}

void IdentifySession::preview(const int row) {
    if (row == matched_row_ && match_ != nullptr) {
        wanted_row_.reset();
        return;
    }
    if (busy_) {
        wanted_row_ = row;
        return;
    }
    use(row);
}

void IdentifySession::dropMatch() {
    wanted_row_.reset();
    matched_row_ = -1;
    if (match_ == nullptr) {
        return;
    }
    emit matchClosed();
    delete match_.data();
}

void IdentifySession::back() {
    if (match_ == nullptr) {
        return;
    }
    match_->deleteLater();
    match_ = nullptr;
    matched_row_ = -1;
    emit matchClosed();
    emit changed();
}

MultiIdentifySession::MultiIdentifySession(TaggerSession& tagger, MusicBrainzLookupService service,
                                           std::vector<TaggerSession::Identify> albums,
                                           QObject* parent)
    : QObject(parent), tagger_(&tagger) {
    albums_.reserve(albums.size());
    for (auto& files : albums) {
        auto* session = new IdentifySession(service, files.descriptors, files.local_paths,
                                            files.items, files.artist, files.release,
                                            files.release_id, this);
        const auto index = albums_.size();
        connect(session, &IdentifySession::changed, this, [this] {
            emit changed();
            lookAhead();
        });
        // Its pairing, asked for by apply().
        connect(session, &IdentifySession::accepted, this,
                [this, index](metadata::MetadataProposalSet proposals) {
                    to_stage_.emplace_back(index, std::move(proposals));
                });
        connect(session, &IdentifySession::matchOpened, this,
                [this](TrackMatchSession* match) {
                    // Ready once paired.
                    connect(match, &TrackMatchSession::changed, this,
                            &MultiIdentifySession::changed);
                });
        albums_.push_back(Album{.files = std::move(files),
                                .session = session,
                                .started = false,
                                .skipped = false,
                                .applied = false,
                                .staged = false,
                                .note = {}});
    }
    connect(&tagger, &TaggerSession::proposalsSettled, this, &MultiIdentifySession::settled);
    // The tagger busy -- reading the files, staging something else --
    // staging waits for it.
    connect(&tagger, &TaggerSession::changed, this, &MultiIdentifySession::stageNext);
}

MultiIdentifySession::~MultiIdentifySession() = default;

IdentifySession* MultiIdentifySession::album(const std::size_t index) const {
    return index < albums_.size() ? albums_[index].session.data() : nullptr;
}

QString MultiIdentifySession::name(const std::size_t index) const {
    if (index >= albums_.size()) {
        return {};
    }
    const auto& files = albums_[index].files;
    if (!files.release.isEmpty()) {
        return files.artist.isEmpty()
                   ? files.release
                   : QStringLiteral("%1 — %2").arg(files.artist, files.release);
    }
    return files.local_paths.empty() ? QString{}
                                     : QFileInfo{files.local_paths.front()}.dir().dirName();
}

bool MultiIdentifySession::ready(const Album& album) const {
    const auto* match = album.session->match();
    return match != nullptr && match->canStage();
}

MultiIdentifySession::State MultiIdentifySession::state(const std::size_t index) const {
    const auto& album = albums_[index];
    if (album.staged) {
        return State::staged;
    }
    if (album.applied) {
        return State::staging;
    }
    if (album.skipped) {
        return State::skipped;
    }
    if (ready(album)) {
        return State::ready;
    }
    if (!album.started) {
        return State::waiting;
    }
    if (album.session->busy()) {
        return State::looking_up;
    }
    return album.session->candidates().empty() ? State::nothing_found : State::found;
}

QString MultiIdentifySession::stateText(const std::size_t index) const {
    if (index >= albums_.size()) {
        return {};
    }
    const auto& album = albums_[index];
    switch (state(index)) {
    case State::waiting:
        return QStringLiteral("Waiting");
    case State::looking_up:
        return QStringLiteral("Looking up…");
    case State::found: {
        if (!album.note.isEmpty()) {
            return album.note;
        }
        const auto versions = album.session->candidates().size();
        return versions == 1U ? QStringLiteral("1 version")
                              : QStringLiteral("%1 versions").arg(versions);
    }
    case State::nothing_found:
        return album.note.isEmpty() ? QStringLiteral("Nothing found") : album.note;
    case State::ready: {
        // Which version: its row's.
        const auto row = album.session->matchedRow();
        const auto& rows = album.session->candidates();
        if (!album.note.isEmpty()) {
            return album.note;
        }
        if (row < 0 || static_cast<std::size_t>(row) >= rows.size()) {
            return QStringLiteral("✓");
        }
        const auto& chosen = rows[static_cast<std::size_t>(row)];
        return chosen.media.isEmpty()
                   ? QStringLiteral("✓ %1").arg(chosen.album)
                   : QStringLiteral("✓ %1 · %2").arg(chosen.album, chosen.media);
    }
    case State::skipped:
        return QStringLiteral("Skipped");
    case State::staging:
        return QStringLiteral("Staging…");
    case State::staged:
        return QStringLiteral("Staged");
    }
    return {};
}

void MultiIdentifySession::setCurrent(const std::size_t index) {
    if (index >= albums_.size()) {
        return;
    }
    current_ = index;
    lookUp(index);
    lookAhead();
    emit changed();
}

void MultiIdentifySession::setSkipped(const std::size_t index, const bool skipped) {
    if (index >= albums_.size() || albums_[index].applied) {
        return;
    }
    albums_[index].skipped = skipped;
    emit changed();
}

std::size_t MultiIdentifySession::readyCount() const {
    return static_cast<std::size_t>(std::ranges::count_if(albums_, [this](const Album& album) {
        return !album.skipped && !album.applied && ready(album);
    }));
}

void MultiIdentifySession::lookUp(const std::size_t index) {
    auto& album = albums_[index];
    if (album.started || album.applied) {
        return;
    }
    album.started = true;
    // Its tagged release, else a search by its tags; an album of untagged
    // files is the person's to search for, or to fingerprint.
    if (!album.files.release_id.isEmpty()) {
        album.session->start();
    } else if (!album.files.artist.isEmpty() || !album.files.release.isEmpty()) {
        album.session->search(album.files.artist, album.files.release);
    } else {
        album.note = QStringLiteral("No tags to search by");
    }
}

void MultiIdentifySession::lookAhead() {
    // One at a time, besides the one shown: MusicBrainz is asked once a
    // second, and the one shown is asked first.
    for (std::size_t index = 0; index < albums_.size(); ++index) {
        if (index != current_ && albums_[index].started && albums_[index].session->busy()) {
            return;
        }
    }
    for (std::size_t step = 1U; step < albums_.size(); ++step) {
        const auto index = (current_ + step) % albums_.size();
        if (!albums_[index].started && !albums_[index].applied) {
            lookUp(index);
            // One with nothing to search by asks nothing: on to the next.
            if (albums_[index].session->busy()) {
                return;
            }
        }
    }
}

void MultiIdentifySession::apply() {
    refused_ = false;
    applied_.clear();
    for (std::size_t index = 0; index < albums_.size(); ++index) {
        auto& album = albums_[index];
        if (album.skipped || album.applied || !ready(album)) {
            continue;
        }
        // Its pairing, as proposals: queued by the accepted connection.
        const auto queued = to_stage_.size();
        album.session->match()->stage();
        if (to_stage_.size() > queued) {
            album.applied = true;
            album.note.clear();
            applied_.push_back(index);
        }
    }
    emit changed();
    stageNext();
}

void MultiIdentifySession::stageNext() {
    if (staging_ || to_stage_.empty() || tagger_.isNull() || !tagger_->canStageProposals()) {
        return;
    }
    auto [index, proposals] = std::move(to_stage_.front());
    to_stage_.pop_front();
    staging_ = index;
    tagger_->applyMusicBrainzProposals(std::move(proposals));
}

void MultiIdentifySession::settled(const bool staged) {
    if (!staging_) {
        return;
    }
    auto& album = albums_[*staging_];
    staging_.reset();
    if (staged) {
        album.staged = true;
    } else {
        // Back to the person, saying why.
        album.applied = false;
        refused_ = true;
        const auto why = tagger_.isNull() ? QString{} : tagger_->status();
        album.note = why.isEmpty() ? QStringLiteral("Could not be staged")
                                   : QStringLiteral("Could not be staged: %1").arg(why);
    }
    emit changed();
    stageNext();
    if (!staging()) {
        // Every album applied taken: done. One the tagger refused keeps the
        // window open, saying why.
        if (!refused_ && std::ranges::any_of(albums_, &Album::staged)) {
            // Their covers, by the tagger, once the window has gone.
            if (fetch_covers_ && !tagger_.isNull()) {
                std::vector<std::vector<std::size_t>> covers;
                for (const auto index : std::exchange(applied_, {})) {
                    if (albums_[index].staged) {
                        covers.push_back(albums_[index].files.items);
                    }
                }
                tagger_->fetchCovers(std::move(covers));
            }
            emit finished();
        }
        if (released_) {
            deleteLater();
        }
    }
}

void MultiIdentifySession::release() {
    released_ = true;
    if (!staging()) {
        deleteLater();
    }
}

} // namespace trackknife::bench
