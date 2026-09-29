// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "quick/engine_client.hpp"
#include "quick/up_next_model.hpp"

#include <QAbstractListModel>
#include <QTimer>
#include <QtQmlIntegration>

#include <functional>
#include <vector>

namespace trackknife::quick {

class EngineSession;

// An engine's library, three ways, as the widgets library panel offers it:
//
//  - browsing: its albums, each expandable to its tracks, by artist, newest
//    first, or at random; pages arrive as the view scrolls (fetchMore);
//  - searching words (ADR-0122): the albums and then the tracks matching;
//  - a query (ADR-0150): the tracks a tkq query finds, or why it does not
//    compile -- never a word search in its place.
class LibraryModel final : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Reached through EngineSession.library")

    Q_PROPERTY(QString search READ search WRITE setSearch NOTIFY searchChanged)
    Q_PROPERTY(bool queryMode READ queryMode WRITE setQueryMode NOTIFY searchChanged)
    Q_PROPERTY(int order READ order WRITE setOrder NOTIFY searchChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString error READ error NOTIFY loadingChanged)

  public:
    enum Role : int {
        KindRole = Qt::UserRole + 1,
        KeyRole,
        TitleRole,
        ArtistRole,
        AlbumRole,
        DateRole,
        TracksRole,
        NumberRole,
        DurationRole,
        ExpandedRole,
        CoverRole,
        RatingRole,
    };
    // Browsing order.
    enum Order : int { byArtist = 0, newestFirst = 1, atRandom = 2 };

    LibraryModel(EngineClient& client, EngineSession& session, QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

    [[nodiscard]] QString search() const { return search_; }
    // Debounced: typing does not send a query per keystroke.
    void setSearch(const QString& text);
    [[nodiscard]] bool queryMode() const { return query_mode_; }
    void setQueryMode(bool on);
    [[nodiscard]] int order() const { return order_; }
    void setOrder(int order);
    [[nodiscard]] bool loading() const { return loading_; }
    [[nodiscard]] QString error() const { return error_; }

    // Browsing again from the start: another random pick, say.
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void toggle(int row);
    // An album's tracks, or one track, onto up-next.
    Q_INVOKABLE void enqueue(int row);
    // An album's tracks, or one track, into one of this engine's lists:
    // before item `position`, or at the end when it is negative.
    Q_INVOKABLE void addToList(int row, const QString& listId, int position);

  signals:
    void searchChanged();
    void loadingChanged();

  private:
    enum class Kind : std::uint8_t {
        album,
        // A track shown under its expanded album.
        child,
        // A track found by a search or a query.
        track,
        // "Albums", "Tracks": a heading over search results.
        section,
    };
    struct Row final {
        Kind kind{Kind::album};
        QString key;
        QString title;
        QString artist;
        QString album;
        QString date;
        int tracks{0};
        int number{0};
        qint64 duration_ms{0};
        int rating{0};
        bool expanded{false};
    };

    // The tracks a row stands for, in order; an album's are asked for.
    void resolve(int row, std::function<void(std::vector<QueuedTrack>)> done);
    void restart();
    void requestPage();
    void requestSearch();
    void append(std::vector<Row> rows);
    void setLoading(bool loading, const QString& error = {});
    [[nodiscard]] static Row rowFrom(const EngineClient::Json& entry, Kind kind);
    [[nodiscard]] int rowOfAlbum(const QString& key) const;

    EngineClient& client_;
    EngineSession& session_;
    QString search_;
    bool query_mode_{false};
    int order_{byArtist};
    QTimer debounce_;
    std::vector<Row> rows_;
    // Browsing albums, or query tracks, already listed.
    std::size_t loaded_{0};
    bool more_{true};
    bool loading_{false};
    QString error_;
    std::uint64_t generation_{0};
};

} // namespace trackknife::quick
