// SPDX-License-Identifier: GPL-3.0-only

#include "quick/mpd_queue_model.hpp"

#include "uicommon/rating_stars.hpp"

#include <QBrush>
#include <QStringList>

#include <algorithm>
#include <atomic>
#include <functional>
#include <chrono>
#include <cstddef>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace trackknife::quick {
namespace {

// Requests in flight at once. Only one is ever outstanding today, but the
// bound stays as the guard against a fan-out: it must count *pending*
// requests, since `requested` stays set on the albums already fetched.
constexpr qsizetype maximum_artwork_requests = 64;
constexpr int maximum_artwork_attempts = 3;
constexpr quint64 artwork_token_namespace = quint64{1} << 62U;
// One counter for every MpdQueueModel instance: the live queue, playlist,
// search, and server list tabs all receive the broadcast artwork responses,
// so per-instance counters would mint colliding tokens and paint one
// album's cover onto another model's pending album.
std::atomic<quint64> artwork_generation{0U};

[[nodiscard]] QString from_utf8(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

[[nodiscard]] QString metadata_values(const mpd::Metadata& metadata, std::string_view name) {
    QStringList values;
    for (const auto value : metadata.values(name)) {
        values.push_back(from_utf8(value));
    }
    return values.join(QStringLiteral(", "));
}

[[nodiscard]] QString metadata_first(const mpd::Track& track, const std::string_view name) {
    const auto value = track.metadata.first(name);
    return value ? from_utf8(*value) : QString{};
}

[[nodiscard]] QString album_artist(const mpd::Track& track) {
    auto value = metadata_values(track.metadata, "AlbumArtist");
    return value.isEmpty() ? metadata_values(track.metadata, "Artist") : value;
}

[[nodiscard]] QString album_group_key(const mpd::Track& track) {
    return album_artist(track) + QChar::Null + metadata_first(track, "Album") + QChar::Null +
           metadata_first(track, "Date");
}

[[nodiscard]] QString display_value(const mpd::Track& track, const int column) {
    switch (column) {
    case 0:
        return {};
    case 1:
        return metadata_values(track.metadata, "Artist");
    case 2:
        return metadata_values(track.metadata, "Track");
    case 3: {
        const auto title = track.metadata.first("Title");
        return title ? from_utf8(*title) : from_utf8(track.uri);
    }
    case 4:
        return metadata_values(track.metadata, "Album");
    case 5:
        return metadata_values(track.metadata, "Date");
    case 6:
        if (track.duration) {
            const auto seconds =
                std::chrono::duration_cast<std::chrono::seconds>(*track.duration).count();
            return QStringLiteral("%1:%2")
                .arg(seconds / 60)
                .arg(seconds % 60, 2, 10, QLatin1Char('0'));
        }
        return {};
    default:
        return {};
    }
}

[[nodiscard]] unsigned effective_rating(const mpd::Track& track,
                                        const QHash<QString, unsigned>& sticker_ratings) {
    if (track.rating) {
        return *track.rating;
    }
    return sticker_ratings.value(from_utf8(track.uri), 0U);
}

[[nodiscard]] std::optional<std::vector<std::uint32_t>>
queue_ids(const std::vector<mpd::Track>& tracks) {
    std::vector<std::uint32_t> ids;
    ids.reserve(tracks.size());
    std::unordered_set<std::uint32_t> unique_ids;
    unique_ids.reserve(tracks.size());
    for (const auto& track : tracks) {
        if (!track.queue_id || !unique_ids.insert(*track.queue_id).second) {
            return std::nullopt;
        }
        ids.push_back(*track.queue_id);
    }
    return ids;
}

[[nodiscard]] std::optional<std::size_t> single_insertion(const std::vector<std::uint32_t>& before,
                                                          const std::vector<std::uint32_t>& after) {
    if (after.size() != before.size() + 1U) {
        return std::nullopt;
    }
    std::size_t inserted = 0U;
    while (inserted < before.size() && before[inserted] == after[inserted]) {
        ++inserted;
    }
    for (std::size_t index = inserted; index < before.size(); ++index) {
        if (before[index] != after[index + 1U]) {
            return std::nullopt;
        }
    }
    return inserted;
}

[[nodiscard]] std::optional<std::size_t> single_removal(const std::vector<std::uint32_t>& before,
                                                        const std::vector<std::uint32_t>& after) {
    if (before.size() != after.size() + 1U) {
        return std::nullopt;
    }
    std::size_t removed = 0U;
    while (removed < after.size() && before[removed] == after[removed]) {
        ++removed;
    }
    for (std::size_t index = removed; index < after.size(); ++index) {
        if (before[index + 1U] != after[index]) {
            return std::nullopt;
        }
    }
    return removed;
}

[[nodiscard]] bool is_move(const std::vector<std::uint32_t>& before,
                           const std::vector<std::uint32_t>& after, const std::size_t source,
                           const std::size_t target) {
    auto moved = before;
    const auto id = moved[source];
    moved.erase(moved.begin() + static_cast<std::ptrdiff_t>(source));
    moved.insert(moved.begin() + static_cast<std::ptrdiff_t>(target), id);
    return moved == after;
}

[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>>
single_move(const std::vector<std::uint32_t>& before, const std::vector<std::uint32_t>& after) {
    if (before.size() != after.size() || before == after) {
        return std::nullopt;
    }
    std::size_t first = 0U;
    while (first < before.size() && before[first] == after[first]) {
        ++first;
    }

    const auto old_item_target =
        std::find(after.begin() + static_cast<std::ptrdiff_t>(first), after.end(), before[first]);
    if (old_item_target != after.end()) {
        const auto target = static_cast<std::size_t>(old_item_target - after.begin());
        if (is_move(before, after, first, target)) {
            return std::pair{first, target};
        }
    }

    const auto new_item_source =
        std::find(before.begin() + static_cast<std::ptrdiff_t>(first), before.end(), after[first]);
    if (new_item_source != before.end()) {
        const auto source = static_cast<std::size_t>(new_item_source - before.begin());
        if (is_move(before, after, source, first)) {
            return std::pair{source, first};
        }
    }
    return std::nullopt;
}

} // namespace

MpdQueueModel::MpdQueueModel(QObject* parent) : QAbstractTableModel(parent) {}

int MpdQueueModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(
        std::min(tracks_.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
}

int MpdQueueModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : column_count;
}

QVariant MpdQueueModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 || index.column() < 0 ||
        index.column() >= column_count || static_cast<std::size_t>(index.row()) >= tracks_.size()) {
        return {};
    }
    const auto& track = tracks_.at(static_cast<std::size_t>(index.row()));
    switch (role) {
    case Qt::DisplayRole:
        if (index.column() == ui::track_rating_column) {
            return ui::track_rating_stars(effective_rating(track, sticker_ratings_));
        }
        return display_value(track, index.column());
    case Qt::ForegroundRole:
        if (index.column() == ui::track_rating_column &&
            effective_rating(track, sticker_ratings_) > 0U) {
            return QBrush{ui::ratingStarColor()};
        }
        return {};
    case Qt::TextAlignmentRole:
        return index.column() == ui::track_length_column
                   ? QVariant::fromValue(Qt::Alignment{Qt::AlignRight | Qt::AlignVCenter})
                   : QVariant{};
    case UriRole:
        return from_utf8(track.uri);
    case QueueIdRole:
        return track.queue_id ? QVariant::fromValue(*track.queue_id) : QVariant{};
    case QueuePositionRole:
        return track.queue_position ? QVariant::fromValue(*track.queue_position) : QVariant{};
    case DurationMsRole:
        return track.duration ? QVariant::fromValue(track.duration->count()) : QVariant{};
    case CurrentRole:
        // ADR-0187: playlist rows carry no queue id, so a context tab marks
        // its playing row by position instead.
        if (current_row_) {
            return index.row() == *current_row_;
        }
        return track.queue_id && current_song_id_ == track.queue_id;
    case AlbumArtistRole: {
        const auto album_artist = metadata_values(track.metadata, "AlbumArtist");
        return album_artist.isEmpty() ? metadata_values(track.metadata, "Artist") : album_artist;
    }
    case PriorityRole:
        return track.priority ? QVariant::fromValue(*track.priority) : QVariant{};
    case RatingRole:
        return QVariant::fromValue(effective_rating(track, sticker_ratings_));
    case ui::track_album_rating_role:
        return QVariant::fromValue(album_ratings_.value(album_group_key(track), 0U));
    case AlbumArtworkRole: {
        const auto artwork = album_artwork_.constFind(album_group_key(track));
        return artwork == album_artwork_.cend() || artwork->image.isNull()
                   ? QVariant{}
                   : QVariant::fromValue(artwork->image);
    }
    case AlbumArtworkUriRole: {
        const auto artwork = album_artwork_.constFind(album_group_key(track));
        return artwork == album_artwork_.cend() ? QVariant{} : QVariant{artwork->uri};
    }
    default:
        return {};
    }
}

QVariant MpdQueueModel::headerData(const int section, const Qt::Orientation orientation,
                                   const int role) const {
    if (role == Qt::DisplayRole && orientation == Qt::Horizontal && section >= 0 &&
        section < column_count) {
        return QString::fromLatin1(ui::track_column_headers.at(static_cast<std::size_t>(section)));
    }
    if (role == Qt::DisplayRole && orientation == Qt::Vertical) {
        return section + 1;
    }
    return {};
}

Qt::ItemFlags MpdQueueModel::flags(const QModelIndex& index) const {
    auto item_flags = QAbstractTableModel::flags(index);
    if (index.isValid()) {
        item_flags |= Qt::ItemIsDragEnabled;
    }
    return item_flags;
}

Qt::DropActions MpdQueueModel::supportedDropActions() const {
    // Drops are executed by the view's typed callbacks, never by the model,
    // but Qt only tracks and paints the drop indicator for actions the target
    // model advertises.
    return Qt::MoveAction | Qt::CopyAction;
}

QHash<int, QByteArray> MpdQueueModel::roleNames() const {
    auto roles = QAbstractTableModel::roleNames();
    roles.insert(UriRole, QByteArrayLiteral("uri"));
    roles.insert(QueueIdRole, QByteArrayLiteral("queueId"));
    roles.insert(QueuePositionRole, QByteArrayLiteral("queuePosition"));
    roles.insert(DurationMsRole, QByteArrayLiteral("durationMs"));
    roles.insert(CurrentRole, QByteArrayLiteral("current"));
    roles.insert(AlbumArtistRole, QByteArrayLiteral("albumArtist"));
    roles.insert(PriorityRole, QByteArrayLiteral("priority"));
    roles.insert(AlbumArtworkRole, QByteArrayLiteral("albumArtwork"));
    roles.insert(AlbumArtworkUriRole, QByteArrayLiteral("albumArtworkUri"));
    roles.insert(RatingRole, QByteArrayLiteral("rating"));
    return roles;
}

void MpdQueueModel::setStickerRatings(QHash<QString, unsigned> ratings) {
    if (sticker_ratings_ == ratings) {
        return;
    }
    sticker_ratings_ = std::move(ratings);
    if (!tracks_.empty()) {
        emit dataChanged(index(0, ui::track_rating_column),
                         index(static_cast<int>(tracks_.size()) - 1, ui::track_rating_column),
                         {Qt::DisplayRole, RatingRole});
    }
}

unsigned MpdQueueModel::ratingAt(const int row) const {
    if (row < 0 || static_cast<std::size_t>(row) >= tracks_.size()) {
        return 0U;
    }
    return effective_rating(tracks_.at(static_cast<std::size_t>(row)), sticker_ratings_);
}

void MpdQueueModel::setAlbumRatings(QHash<QString, unsigned> ratings) {
    if (album_ratings_ == ratings) {
        return;
    }
    album_ratings_ = std::move(ratings);
    if (!tracks_.empty()) {
        emit dataChanged(index(0, ui::track_artwork_column),
                         index(static_cast<int>(tracks_.size()) - 1, ui::track_artwork_column),
                         {ui::track_album_rating_role});
    }
}

QString MpdQueueModel::albumGroupKey(const mpd::Track& track) { return album_group_key(track); }

std::optional<std::uint32_t> MpdQueueModel::queueIdAt(const int row) const {
    if (row < 0 || static_cast<std::size_t>(row) >= tracks_.size()) {
        return std::nullopt;
    }
    return tracks_.at(static_cast<std::size_t>(row)).queue_id;
}

std::optional<int> MpdQueueModel::rowForQueueId(const std::uint32_t song_id) const {
    for (std::size_t row = 0U; row < tracks_.size(); ++row) {
        if (tracks_[row].queue_id == song_id) {
            return static_cast<int>(row);
        }
    }
    return std::nullopt;
}

std::optional<std::string> MpdQueueModel::uriAt(const int row) const {
    if (row < 0 || static_cast<std::size_t>(row) >= tracks_.size()) {
        return std::nullopt;
    }
    return tracks_.at(static_cast<std::size_t>(row)).uri;
}

qint64 MpdQueueModel::totalDurationMs() const noexcept {
    qint64 total = 0;
    for (const auto& track : tracks_) {
        if (track.duration && track.duration->count() > 0) {
            total += track.duration->count();
        }
    }
    return total;
}

// ADR-0187: mark the playing row by position, for playlist tabs whose list
// is the active playback context. std::nullopt restores id-based marking.
void MpdQueueModel::setCurrentRow(const std::optional<int> row) {
    if (current_row_ == row) {
        return;
    }
    const auto previous = current_row_;
    current_row_ = row;
    const auto refresh = [this](const std::optional<int>& target) {
        if (target && *target >= 0 && *target < rowCount()) {
            emit dataChanged(index(*target, 0), index(*target, column_count - 1), {CurrentRole});
        }
    };
    refresh(previous);
    refresh(current_row_);
}

void MpdQueueModel::setCurrentSongId(const std::optional<std::uint32_t> song_id) {
    if (current_song_id_ == song_id) {
        return;
    }
    const auto old_row = current_song_id_ ? rowForQueueId(*current_song_id_) : std::nullopt;
    current_song_id_ = song_id;
    const auto new_row = current_song_id_ ? rowForQueueId(*current_song_id_) : std::nullopt;
    if (old_row) {
        emit dataChanged(index(*old_row, 0), index(*old_row, column_count - 1), {CurrentRole});
    }
    if (new_row && new_row != old_row) {
        emit dataChanged(index(*new_row, 0), index(*new_row, column_count - 1), {CurrentRole});
    }
}

void MpdQueueModel::appendTracks(std::vector<mpd::Track> tracks) {
    if (tracks.empty() ||
        tracks_.size() + tracks.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return;
    }
    const auto first = static_cast<int>(tracks_.size());
    beginInsertRows({}, first, first + static_cast<int>(tracks.size()) - 1);
    tracks_.insert(tracks_.end(), std::make_move_iterator(tracks.begin()),
                   std::make_move_iterator(tracks.end()));
    endInsertRows();
    synchronizeArtwork();
    requestNextArtwork();
}

void MpdQueueModel::insertTracks(const int insertion_row, std::vector<mpd::Track> tracks) {
    if (tracks.empty() ||
        tracks_.size() + tracks.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return;
    }
    if (insertion_row < 0 || insertion_row >= static_cast<int>(tracks_.size())) {
        appendTracks(std::move(tracks));
        return;
    }
    beginInsertRows({}, insertion_row, insertion_row + static_cast<int>(tracks.size()) - 1);
    tracks_.insert(tracks_.begin() + static_cast<std::ptrdiff_t>(insertion_row),
                   std::make_move_iterator(tracks.begin()), std::make_move_iterator(tracks.end()));
    endInsertRows();
    synchronizeArtwork();
    requestNextArtwork();
}

void MpdQueueModel::removeTrackRows(QList<int> rows) {
    std::sort(rows.begin(), rows.end(), std::greater<>{});
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    for (const auto row : rows) {
        if (row < 0 || static_cast<std::size_t>(row) >= tracks_.size()) {
            continue;
        }
        beginRemoveRows({}, row, row);
        tracks_.erase(tracks_.begin() + row);
        endRemoveRows();
    }
    synchronizeArtwork();
}

int MpdQueueModel::moveTrackRows(const QList<int>& rows, const int insertion_row) {
    std::vector<int> sources;
    sources.reserve(static_cast<std::size_t>(rows.size()));
    for (const auto row : rows) {
        if (row >= 0 && static_cast<std::size_t>(row) < tracks_.size()) {
            sources.push_back(row);
        }
    }
    std::sort(sources.begin(), sources.end());
    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
    if (sources.empty()) {
        return -1;
    }
    std::vector<mpd::Track> block;
    block.reserve(sources.size());
    for (const auto row : sources) {
        block.push_back(tracks_[static_cast<std::size_t>(row)]);
    }
    auto target = std::clamp(insertion_row, 0, static_cast<int>(tracks_.size()));
    for (const auto row : sources) {
        if (row < target) {
            --target;
        }
    }
    beginResetModel();
    for (auto it = sources.rbegin(); it != sources.rend(); ++it) {
        tracks_.erase(tracks_.begin() + *it);
    }
    tracks_.insert(tracks_.begin() + target, std::make_move_iterator(block.begin()),
                   std::make_move_iterator(block.end()));
    endResetModel();
    synchronizeArtwork();
    return target;
}

void MpdQueueModel::replaceTracks(std::vector<mpd::Track> tracks) {
    if (tracks_ == tracks) {
        return;
    }

    const auto before_ids = queue_ids(tracks_);
    const auto after_ids = queue_ids(tracks);
    const auto representable =
        tracks_.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()) &&
        tracks.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max());
    if (!representable || !before_ids || !after_ids) {
        beginResetModel();
        tracks_ = std::move(tracks);
        synchronizeArtwork();
        endResetModel();
        requestNextArtwork();
        return;
    }

    if (*before_ids == *after_ids) {
        tracks_ = std::move(tracks);
        synchronizeArtwork();
        if (!tracks_.empty()) {
            emit dataChanged(index(0, 0), index(rowCount() - 1, column_count - 1));
        }
        requestNextArtwork();
        return;
    }

    bool reconciled = false;
    if (const auto inserted = single_insertion(*before_ids, *after_ids)) {
        const auto row = static_cast<int>(*inserted);
        beginInsertRows({}, row, row);
        tracks_.insert(tracks_.begin() + static_cast<std::ptrdiff_t>(*inserted), tracks[*inserted]);
        endInsertRows();
        reconciled = true;
    } else if (const auto removed = single_removal(*before_ids, *after_ids)) {
        const auto row = static_cast<int>(*removed);
        beginRemoveRows({}, row, row);
        tracks_.erase(tracks_.begin() + static_cast<std::ptrdiff_t>(*removed));
        endRemoveRows();
        reconciled = true;
    } else if (const auto moved = single_move(*before_ids, *after_ids)) {
        const auto source = static_cast<int>(moved->first);
        const auto target = static_cast<int>(moved->second);
        const auto destination = target > source ? target + 1 : target;
        beginMoveRows({}, source, source, {}, destination);
        auto item = std::move(tracks_[moved->first]);
        tracks_.erase(tracks_.begin() + static_cast<std::ptrdiff_t>(moved->first));
        tracks_.insert(tracks_.begin() + static_cast<std::ptrdiff_t>(moved->second),
                       std::move(item));
        endMoveRows();
        reconciled = true;
    }

    if (!reconciled) {
        beginResetModel();
        tracks_ = std::move(tracks);
        synchronizeArtwork();
        endResetModel();
        requestNextArtwork();
        return;
    }

    tracks_ = std::move(tracks);
    synchronizeArtwork();
    if (!tracks_.empty()) {
        emit dataChanged(index(0, 0), index(rowCount() - 1, column_count - 1));
    }
    requestNextArtwork();
}

void MpdQueueModel::setArtworkEnabled(const bool enabled) {
    artwork_enabled_ = enabled;
    if (enabled) {
        requestNextArtwork();
    }
}

void MpdQueueModel::retryMissingArtwork() {
    bool rearmed = false;
    for (auto& artwork : album_artwork_) {
        if (artwork.requested && artwork.image.isNull() &&
            (!active_artwork_token_ || artwork.token != *active_artwork_token_)) {
            artwork.requested = false;
            artwork.token = 0U;
            artwork.failed_attempts = 0;
            rearmed = true;
        }
    }
    if (rearmed) {
        requestNextArtwork();
    }
}

void MpdQueueModel::acceptArtwork(const quint64 token, const QImage& image) {
    if (!active_artwork_token_ || *active_artwork_token_ != token) {
        return;
    }
    active_artwork_token_.reset();
    for (auto artwork = album_artwork_.begin(); artwork != album_artwork_.end(); ++artwork) {
        if (artwork->token != token) {
            continue;
        }
        if (!image.isNull()) {
            artwork->image = image;
            artwork->failed_attempts = 0;
            if (!tracks_.empty()) {
                emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {AlbumArtworkRole});
            }
        } else if (++artwork->failed_attempts < maximum_artwork_attempts) {
            artwork->requested = false;
        }
        // Answered either way: the request is no longer in flight, and a
        // finished album must not occupy the request budget forever.
        artwork->token = 0U;
        break;
    }
    requestNextArtwork();
}

void MpdQueueModel::synchronizeArtwork() {
    QHash<QString, AlbumArtwork> synchronized;
    synchronized.reserve(static_cast<qsizetype>(tracks_.size()));
    for (const auto& track : tracks_) {
        const auto key = album_group_key(track);
        if (synchronized.contains(key)) {
            continue;
        }
        if (const auto existing = album_artwork_.constFind(key);
            existing != album_artwork_.cend()) {
            synchronized.insert(key, *existing);
        } else {
            synchronized.insert(key, AlbumArtwork{.uri = from_utf8(track.uri),
                                                  .image = {},
                                                  .requested = false,
                                                  .token = 0U});
        }
    }
    album_artwork_ = std::move(synchronized);
}

void MpdQueueModel::requestNextArtwork() {
    const auto pending = std::ranges::count_if(album_artwork_, [](const AlbumArtwork& artwork) {
        return artwork.requested && artwork.token != 0U;
    });
    if (!artwork_enabled_ || active_artwork_token_ || pending >= maximum_artwork_requests) {
        return;
    }
    QString previous_key;
    bool first = true;
    for (const auto& track : tracks_) {
        const auto key = album_group_key(track);
        if (!first && key == previous_key) {
            continue;
        }
        first = false;
        previous_key = key;
        auto artwork = album_artwork_.find(key);
        if (artwork == album_artwork_.end() || artwork->requested || artwork->uri.isEmpty()) {
            continue;
        }
        artwork->requested = true;
        artwork->token = artwork_token_namespace | ++artwork_generation;
        active_artwork_token_ = artwork->token;
        emit artworkRequested(artwork->token, artwork->uri);
        return;
    }
}

} // namespace trackknife::quick
