// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/mpd/model.hpp"

#include <QAbstractTableModel>
#include <QIcon>
#include <QImage>
#include <QStringList>
#include <QStringView>

#include <optional>
#include <vector>

namespace trackknife::quick {

[[nodiscard]] std::vector<mpd::AlbumSummary>
filterAlbumSearchResults(std::vector<mpd::AlbumSummary> albums, QStringView query);

class MpdSearchResultModel final : public QAbstractTableModel {
    Q_OBJECT

  public:
    static constexpr int column_count = 7;
    static constexpr int first_action_column = 4;

    enum class ResultKind { section, album, track };
    enum ExtraRole {
        ResultKindRole = Qt::UserRole + 1,
        UriListRole,
        ArtworkUriRole,
        TrackNumberRole,
    };

    explicit MpdSearchResultModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index,
                                int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void replaceTracks(std::vector<mpd::Track> tracks);
    void replaceSearchResults(std::vector<mpd::AlbumSummary> albums,
                              std::vector<mpd::Track> tracks);
    // Sidebar presentation uses one descriptive column plus queue actions.
    void setCompact(bool compact);
    void setAlbumPlaceholder(QIcon icon);
    void setArtworkEnabled(bool enabled);
    [[nodiscard]] ResultKind kindAt(int row) const;
    [[nodiscard]] QStringList urisAt(int row) const;
    [[nodiscard]] std::optional<mpd::AlbumFilter> albumAt(int row) const;
    [[nodiscard]] int firstResultRow() const;
    [[nodiscard]] int nextResultRow(int row, int direction) const;
    [[nodiscard]] std::vector<int> sectionRows() const;

  public slots:
    void acceptArtwork(quint64 token, const QImage& image);
    void refreshArtwork();

  signals:
    void artworkRequested(quint64 token, const QString& uri);

  private:
    void replace(std::optional<std::vector<mpd::AlbumSummary>> albums,
                 std::vector<mpd::Track> tracks);

    struct Row {
        ResultKind kind{ResultKind::section};
        QString artist;
        QString result;
        QString context;
        QString detail;
        QStringList uris;
        std::optional<mpd::AlbumFilter> album_filter;
        QString artwork_uri;
        QImage artwork;
        bool artwork_requested{false};
        quint64 artwork_token{0U};
        QString track_number{};
        // ADR-0179: the album's 0-10 rating (stored, else the server's
        // computed mean rounded); painted over the cover and shown as stars.
        unsigned rating{0U};
    };

    void requestNextArtwork();
    [[nodiscard]] QVariant foregroundFor(const QModelIndex& index) const;
    std::vector<Row> rows_;
    bool compact_{false};
    QIcon album_placeholder_;
    quint64 artwork_generation_{0U};
    bool artwork_enabled_{false};
    bool artwork_request_in_flight_{false};
    int artwork_requests_this_generation_{0};
};

} // namespace trackknife::quick
