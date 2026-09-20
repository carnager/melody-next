// SPDX-License-Identifier: GPL-3.0-only

#include "quick/mpd_search_result_model.hpp"

#include "uicommon/rating_stars.hpp"
#include "uicommon/track_row_roles.hpp"

#include <QBrush>
#include <QPainter>
#include <QStringList>

#include <cmath>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>

namespace trackknife::quick {
namespace {

constexpr std::array<const char*, MpdSearchResultModel::column_count> headers{
    "Artist", "Result", "Album / date", "Duration", "Append", "Add next", "Replace"};
constexpr int maximum_artwork_requests = 32;
constexpr quint64 artwork_token_namespace = quint64{1} << 63U;

[[nodiscard]] QString from_utf8(const std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

[[nodiscard]] QString metadata_values(const mpd::Metadata& metadata, const std::string_view name) {
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

[[nodiscard]] QString format_duration(const qint64 milliseconds) {
    const auto seconds = std::max<qint64>(0, milliseconds / 1'000);
    const auto hours = seconds / 3'600;
    const auto minutes = (seconds / 60) % 60;
    const auto remainder = seconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(remainder, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(minutes).arg(remainder, 2, 10, QLatin1Char('0'));
}

[[nodiscard]] QString album_key(const mpd::Track& track) {
    if (!track.musicbrainz.release_ids.empty()) {
        return QStringLiteral("mbid:") + from_utf8(track.musicbrainz.release_ids.front());
    }
    return album_artist(track) + QChar::Null + metadata_first(track, "Album") + QChar::Null +
           metadata_first(track, "Date");
}

[[nodiscard]] std::optional<int> release_year(const QString& date) {
    if (date.size() < 4) {
        return std::nullopt;
    }
    bool valid = false;
    const auto year = date.left(4).toInt(&valid);
    return valid ? std::optional<int>{year} : std::nullopt;
}

[[nodiscard]] int compare_text(const QString& left, const QString& right) {
    return QString::compare(left, right, Qt::CaseInsensitive);
}

} // namespace

std::vector<mpd::AlbumSummary> filterAlbumSearchResults(std::vector<mpd::AlbumSummary> albums,
                                                        const QStringView query) {
    const auto terms = query.toString().simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    std::erase_if(albums, [&terms](const mpd::AlbumSummary& album) {
        const auto searchable = from_utf8(album.artist) + QLatin1Char(' ') + from_utf8(album.album);
        return !std::ranges::all_of(terms, [&searchable](const QString& term) {
            return searchable.contains(term, Qt::CaseInsensitive);
        });
    });
    return albums;
}

MpdSearchResultModel::MpdSearchResultModel(QObject* parent) : QAbstractTableModel(parent) {}

int MpdSearchResultModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(
        std::min(rows_.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
}

int MpdSearchResultModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : column_count;
}

QVariant MpdSearchResultModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 || index.column() < 0 ||
        index.column() >= column_count || static_cast<std::size_t>(index.row()) >= rows_.size()) {
        return {};
    }
    const auto& row = rows_.at(static_cast<std::size_t>(index.row()));
    if (role == ResultKindRole) {
        return static_cast<int>(row.kind);
    }
    if (role == TrackNumberRole) {
        return row.track_number;
    }
    if (role == UriListRole) {
        return row.uris;
    }
    if (role == ArtworkUriRole) {
        return row.artwork_uri;
    }
    if (role == Qt::DecorationRole && index.column() == 0 && row.kind == ResultKind::album) {
        if (!row.artwork.isNull()) {
            return row.artwork;
        }
        return album_placeholder_;
    }
    if ((role == Qt::ToolTipRole || role == Qt::AccessibleTextRole ||
         role == Qt::AccessibleDescriptionRole) &&
        index.column() >= first_action_column && row.kind != ResultKind::section) {
        switch (index.column()) {
        case 4:
            return role == Qt::ToolTipRole ? QStringLiteral("Append to queue (Enter)")
                                           : QStringLiteral("Append to queue");
        case 5:
            return role == Qt::ToolTipRole ? QStringLiteral("Add after current track")
                                           : QStringLiteral("Add after current track");
        case 6:
            return role == Qt::ToolTipRole ? QStringLiteral("Replace queue and play (Ctrl+Enter)")
                                           : QStringLiteral("Replace queue and play");
        default:
            return {};
        }
    }
    if (compact_ && index.column() == 0 && row.kind != ResultKind::section &&
        (role == Qt::DisplayRole || role == Qt::ToolTipRole || role == Qt::AccessibleTextRole)) {
        const auto label =
            row.artist.isEmpty() ? row.result : row.artist + QStringLiteral(" — ") + row.result;
        return role == Qt::ToolTipRole
                   ? QStringList{label, row.context, row.detail}.join(QLatin1Char('\n'))
                   : label;
    }
    if (role == Qt::ForegroundRole) {
        return foregroundFor(index);
    }
    if (role == ui::track_rating_role) {
        return QVariant::fromValue(row.rating);
    }
    if (role != Qt::DisplayRole) {
        return {};
    }
    if (row.kind == ResultKind::section) {
        return index.column() == 0 ? row.result : QString{};
    }
    switch (index.column()) {
    case 0:
        return row.artist;
    case 1:
        return row.result;
    case 2:
        return row.context;
    case 3:
        return row.detail;
    default:
        return {};
    }
}

QVariant MpdSearchResultModel::foregroundFor(const QModelIndex& index) const {
    const auto& row = rows_.at(static_cast<std::size_t>(index.row()));
    if (row.kind == ResultKind::album && index.column() == 3 && row.rating > 0U) {
        return QBrush{ui::ratingStarColor()};
    }
    return {};
}

QVariant MpdSearchResultModel::headerData(const int section, const Qt::Orientation orientation,
                                          const int role) const {
    if (role == Qt::DisplayRole && orientation == Qt::Horizontal && section >= 0 &&
        section < column_count) {
        return QString::fromLatin1(headers.at(static_cast<std::size_t>(section)));
    }
    return {};
}

Qt::ItemFlags MpdSearchResultModel::flags(const QModelIndex& index) const {
    if (!index.isValid() || kindAt(index.row()) == ResultKind::section) {
        return Qt::NoItemFlags;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

QHash<int, QByteArray> MpdSearchResultModel::roleNames() const {
    auto roles = QAbstractTableModel::roleNames();
    roles.insert(ResultKindRole, QByteArrayLiteral("resultKind"));
    roles.insert(UriListRole, QByteArrayLiteral("uris"));
    roles.insert(ArtworkUriRole, QByteArrayLiteral("artworkUri"));
    return roles;
}

void MpdSearchResultModel::replaceTracks(std::vector<mpd::Track> tracks) {
    replace(std::nullopt, std::move(tracks));
}

void MpdSearchResultModel::replaceSearchResults(std::vector<mpd::AlbumSummary> albums,
                                                std::vector<mpd::Track> tracks) {
    replace(std::move(albums), std::move(tracks));
}

void MpdSearchResultModel::replace(std::optional<std::vector<mpd::AlbumSummary>> album_summaries,
                                   std::vector<mpd::Track> tracks) {
    mpd::sort_search_results(tracks);

    struct Album {
        QString key;
        QString artist;
        QString title;
        QString date;
        QStringList uris;
        qint64 duration_ms{0};
        mpd::AlbumFilter filter;
        QString artwork_uri;
        unsigned rating{0U};
    };
    std::vector<Album> albums;
    if (album_summaries) {
        albums.reserve(album_summaries->size());
        for (auto& summary : *album_summaries) {
            albums.push_back(Album{
                .key = from_utf8(summary.filter.release_id.value_or(
                    summary.artist + '\0' + summary.album + '\0' + summary.date)),
                .artist = from_utf8(summary.artist),
                .title = from_utf8(summary.album),
                .date = from_utf8(summary.date),
                .uris = {},
                .duration_ms = 0,
                .filter = std::move(summary.filter),
                .artwork_uri = from_utf8(summary.artwork_uri),
                .rating = summary.rating != 0U ? summary.rating
                                               : std::min(10U, static_cast<unsigned>(std::lround(
                                                                   summary.computed_rating))),
            });
        }
    } else {
        for (const auto& track : tracks) {
            const auto title = metadata_first(track, "Album");
            if (title.isEmpty()) {
                continue;
            }
            const auto key = album_key(track);
            auto found = std::ranges::find(albums, key, &Album::key);
            if (found == albums.end()) {
                const auto tagged_album_artist = metadata_values(track.metadata, "AlbumArtist");
                const auto date = metadata_first(track, "Date");
                albums.push_back(Album{
                    .key = key,
                    .artist = album_artist(track),
                    .title = title,
                    .date = date,
                    .uris = {},
                    .duration_ms = 0,
                    .filter =
                        mpd::AlbumFilter{
                            .release_id =
                                track.musicbrainz.release_ids.empty()
                                    ? std::nullopt
                                    : std::optional{track.musicbrainz.release_ids.front()},
                            .artist = tagged_album_artist.isEmpty()
                                          ? metadata_values(track.metadata, "Artist")
                                                .toUtf8()
                                                .toStdString()
                                          : tagged_album_artist.toUtf8().toStdString(),
                            .album = title.toUtf8().toStdString(),
                            .date = date.isEmpty() ? std::nullopt
                                                   : std::optional{date.toUtf8().toStdString()},
                            .artist_is_album_artist = !tagged_album_artist.isEmpty(),
                        },
                    .artwork_uri = from_utf8(track.uri)});
                found = std::prev(albums.end());
            }
            found->uris.push_back(from_utf8(track.uri));
            if (track.duration) {
                found->duration_ms += track.duration->count();
            }
        }
    }
    std::ranges::stable_sort(albums, [](const Album& left, const Album& right) {
        const auto left_year = release_year(left.date);
        const auto right_year = release_year(right.date);
        if (left_year != right_year) {
            if (!left_year) {
                return false;
            }
            if (!right_year) {
                return true;
            }
            return *left_year < *right_year;
        }
        if (const auto order = compare_text(left.date, right.date); order != 0) {
            return order < 0;
        }
        if (const auto order = compare_text(left.artist, right.artist); order != 0) {
            return order < 0;
        }
        if (const auto order = compare_text(left.title, right.title); order != 0) {
            return order < 0;
        }
        return left.key < right.key;
    });

    std::vector<Row> rows;
    rows.reserve(albums.size() + tracks.size() + 2U);
    if (!albums.empty()) {
        rows.push_back(Row{.kind = ResultKind::section,
                           .artist = {},
                           .result = QStringLiteral("Albums (%1)").arg(albums.size()),
                           .context = {},
                           .detail = {},
                           .uris = {},
                           .album_filter = std::nullopt,
                           .artwork_uri = {},
                           .artwork = {},
                           .artwork_requested = false,
                           .artwork_token = 0U});
        for (auto& album : albums) {
            rows.push_back(Row{.kind = ResultKind::album,
                               .artist = std::move(album.artist),
                               .result = std::move(album.title),
                               .context = std::move(album.date),
                               .detail = ui::track_rating_stars(album.rating),
                               .uris = std::move(album.uris),
                               .album_filter = std::move(album.filter),
                               .artwork_uri = std::move(album.artwork_uri),
                               .artwork = {},
                               .artwork_requested = false,
                               .artwork_token = 0U,
                               .rating = album.rating});
        }
    }
    if (!tracks.empty()) {
        rows.push_back(Row{.kind = ResultKind::section,
                           .artist = {},
                           .result = QStringLiteral("Tracks (%1)").arg(tracks.size()),
                           .context = {},
                           .detail = {},
                           .uris = {},
                           .album_filter = std::nullopt,
                           .artwork_uri = {},
                           .artwork = {},
                           .artwork_requested = false,
                           .artwork_token = 0U});
        for (const auto& track : tracks) {
            const auto title = metadata_first(track, "Title");
            rows.push_back(Row{
                .kind = ResultKind::track,
                .artist = metadata_values(track.metadata, "Artist"),
                .result = title.isEmpty() ? from_utf8(track.uri) : title,
                .context = metadata_first(track, "Album"),
                .detail = track.duration ? format_duration(track.duration->count()) : QString{},
                .uris = {from_utf8(track.uri)},
                .album_filter = std::nullopt,
                .artwork_uri = {},
                .artwork = {},
                .artwork_requested = false,
                .artwork_token = 0U,
                .track_number = metadata_first(track, "Track"),
            });
        }
    }

    beginResetModel();
    rows_ = std::move(rows);
    artwork_request_in_flight_ = false;
    artwork_requests_this_generation_ = 0;
    ++artwork_generation_;
    endResetModel();
    requestNextArtwork();
}

void MpdSearchResultModel::setCompact(const bool compact) {
    if (compact_ == compact) {
        return;
    }
    compact_ = compact;
    if (rowCount() > 0) {
        emit dataChanged(index(0, 0), index(rowCount() - 1, 0),
                         {Qt::DisplayRole, Qt::ToolTipRole, Qt::AccessibleTextRole});
    }
}

void MpdSearchResultModel::setAlbumPlaceholder(QIcon icon) {
    album_placeholder_ = std::move(icon);
    if (rowCount() > 0) {
        emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {Qt::DecorationRole});
    }
}

void MpdSearchResultModel::setArtworkEnabled(const bool enabled) {
    artwork_enabled_ = enabled;
    if (enabled) {
        requestNextArtwork();
    }
}

void MpdSearchResultModel::refreshArtwork() {
    artwork_request_in_flight_ = false;
    artwork_requests_this_generation_ = 0;
    for (auto& row : rows_) {
        row.artwork = {};
        row.artwork_requested = false;
        row.artwork_token = 0;
    }
    if (!rows_.empty()) {
        emit dataChanged(index(0, 0), index(rowCount() - 1, column_count - 1),
                         {Qt::DecorationRole});
    }
    requestNextArtwork();
}

void MpdSearchResultModel::acceptArtwork(const quint64 token, const QImage& image) {
    for (int row_number = 0; row_number < rowCount(); ++row_number) {
        auto& row = rows_[static_cast<std::size_t>(row_number)];
        if (row.artwork_token != token) {
            continue;
        }
        artwork_request_in_flight_ = false;
        if (!image.isNull()) {
            row.artwork = image;
            if (row.rating > 0U) {
                // Composited once here so painting stays a plain image draw.
                row.artwork = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
                QPainter painter{&row.artwork};
                ui::paintRatingOverlay(&painter, row.artwork.rect(), row.rating);
            }
            emit dataChanged(index(row_number, 0), index(row_number, 0), {Qt::DecorationRole});
        }
        requestNextArtwork();
        return;
    }
}

void MpdSearchResultModel::requestNextArtwork() {
    if (!artwork_enabled_ || artwork_request_in_flight_ ||
        artwork_requests_this_generation_ >= maximum_artwork_requests) {
        return;
    }
    for (auto& row : rows_) {
        if (row.kind != ResultKind::album || row.artwork_requested || row.artwork_uri.isEmpty()) {
            continue;
        }
        row.artwork_requested = true;
        row.artwork_token = artwork_token_namespace | ++artwork_generation_;
        artwork_request_in_flight_ = true;
        ++artwork_requests_this_generation_;
        emit artworkRequested(row.artwork_token, row.artwork_uri);
        return;
    }
}

MpdSearchResultModel::ResultKind MpdSearchResultModel::kindAt(const int row) const {
    if (row < 0 || static_cast<std::size_t>(row) >= rows_.size()) {
        return ResultKind::section;
    }
    return rows_.at(static_cast<std::size_t>(row)).kind;
}

QStringList MpdSearchResultModel::urisAt(const int row) const {
    if (row < 0 || static_cast<std::size_t>(row) >= rows_.size()) {
        return {};
    }
    return rows_.at(static_cast<std::size_t>(row)).uris;
}

std::optional<mpd::AlbumFilter> MpdSearchResultModel::albumAt(const int row) const {
    if (row < 0 || static_cast<std::size_t>(row) >= rows_.size()) {
        return std::nullopt;
    }
    return rows_.at(static_cast<std::size_t>(row)).album_filter;
}

int MpdSearchResultModel::firstResultRow() const { return nextResultRow(-1, 1); }

int MpdSearchResultModel::nextResultRow(const int row, const int direction) const {
    if (direction == 0) {
        return -1;
    }
    for (auto candidate = row + direction; candidate >= 0 && candidate < rowCount();
         candidate += direction) {
        if (kindAt(candidate) != ResultKind::section) {
            return candidate;
        }
    }
    return -1;
}

std::vector<int> MpdSearchResultModel::sectionRows() const {
    std::vector<int> result;
    for (int row = 0; row < rowCount(); ++row) {
        if (kindAt(row) == ResultKind::section) {
            result.push_back(row);
        }
    }
    return result;
}

} // namespace trackknife::quick
