// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/mpd/model.hpp"
#include "uicommon/track_row_roles.hpp"

#include <QAbstractTableModel>
#include <QHash>
#include <QImage>
#include <QString>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::quick {

class MpdQueueModel final : public QAbstractTableModel {
    Q_OBJECT

  public:
    static constexpr int column_count = ui::track_column_count;

    // The values implement the shared album-grouped presentation contract
    // consumed by the uicommon delegate/view.
    enum ExtraRole {
        UriRole = ui::track_source_role,
        QueueIdRole = ui::track_id_role,
        QueuePositionRole = ui::track_position_role,
        DurationMsRole = ui::track_duration_ms_role,
        CurrentRole = ui::track_current_role,
        AlbumArtistRole = ui::track_album_artist_role,
        PriorityRole = ui::track_priority_role,
        AlbumArtworkRole = ui::track_album_artwork_role,
        AlbumArtworkUriRole = ui::track_album_artwork_key_role,
        RatingRole = ui::track_rating_role,
    };
    static_assert(column_count == ui::track_column_count);

    explicit MpdQueueModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index,
                                int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    [[nodiscard]] Qt::DropActions supportedDropActions() const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] std::optional<std::uint32_t> queueIdAt(int row) const;
    [[nodiscard]] std::optional<int> rowForQueueId(std::uint32_t song_id) const;
    [[nodiscard]] std::optional<std::string> uriAt(int row) const;
    [[nodiscard]] qint64 totalDurationMs() const noexcept;
    [[nodiscard]] std::vector<mpd::Track> tracksSnapshot() const { return tracks_; }
    // Read-only UI-thread access for bounded presentation snapshots. Callers
    // must not retain the pointer across model changes or pass it to workers.
    [[nodiscard]] const mpd::Track* trackAt(int row) const noexcept {
        return row >= 0 && static_cast<std::size_t>(row) < tracks_.size()
                   ? &tracks_[static_cast<std::size_t>(row)]
                   : nullptr;
    }

    void replaceTracks(std::vector<mpd::Track> tracks);
    // ADR-0181: client-mode editing for client-owned server list tabs. The
    // live-queue instance never calls these; server reconciliation stays in
    // replaceTracks.
    void appendTracks(std::vector<mpd::Track> tracks);
    void removeTrackRows(QList<int> rows);
    // Moves the given rows in front of insertion_row (rows-count semantics of
    // QueueTableView's reorder callback); returns the moved block's first
    // final row, or -1 when nothing moved.
    int moveTrackRows(const QList<int>& rows, int insertion_row);
    // Re-arms artwork requests that came back empty (e.g. fired while the
    // connection was down) and pumps the request cycle again.
    void retryMissingArtwork();
    void setCurrentSongId(std::optional<std::uint32_t> song_id);
    void setArtworkEnabled(bool enabled);
    // ADR-0179: per-URI sticker ratings from the session snapshot. A track's
    // own Melody X-Rating line wins over the sticker map.
    void setStickerRatings(QHash<QString, unsigned> ratings);
    [[nodiscard]] unsigned ratingAt(int row) const;
    // Melody album ratings keyed by the delegate's album group identity;
    // painted over the group's cover artwork.
    void setAlbumRatings(QHash<QString, unsigned> ratings);
    [[nodiscard]] static QString albumGroupKey(const mpd::Track& track);

  public slots:
    void acceptArtwork(quint64 token, const QImage& image);

  signals:
    void artworkRequested(quint64 token, const QString& uri);

  private:
    struct AlbumArtwork {
        QString uri;
        QImage image;
        bool requested{false};
        quint64 token{0U};
    };

    void synchronizeArtwork();
    void requestNextArtwork();

    std::vector<mpd::Track> tracks_;
    QHash<QString, unsigned> sticker_ratings_;
    QHash<QString, unsigned> album_ratings_;
    std::optional<std::uint32_t> current_song_id_;
    QHash<QString, AlbumArtwork> album_artwork_;
    std::optional<quint64> active_artwork_token_;
    quint64 artwork_generation_{0U};
    bool artwork_enabled_{false};
};

} // namespace trackknife::quick
