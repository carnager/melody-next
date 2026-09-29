// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "quick/engine_client.hpp"

#include <QAbstractListModel>
#include <QVariantMap>
#include <QtQmlIntegration>

#include <vector>

namespace trackknife::quick {

class EngineSession;

// The tracks of the list on show, grouped into albums: a header row before
// each run of tracks from the same album in the same folder. Shows one list
// of one engine at a time; which is Engine's choice (Engine.show).
class TrackListModel final : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Reached through Engine.tracks")

    Q_PROPERTY(QString listId READ listId NOTIFY listIdChanged)
    Q_PROPERTY(trackknife::quick::EngineSession* session READ session NOTIFY listIdChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(int trackCount READ trackCount NOTIFY loaded)
    Q_PROPERTY(qreal totalDuration READ totalDuration NOTIFY loaded)

  public:
    enum Role : int {
        HeaderRole = Qt::UserRole + 1,
        EntryRole,
        PathRole,
        TitleRole,
        ArtistRole,
        AlbumRole,
        DateRole,
        NumberRole,
        DurationRole,
        CoverRole,
        GroupTracksRole,
        GroupDurationRole,
        // 0-10, half stars; a header row's is its album's.
        RatingRole,
    };

    explicit TrackListModel(QObject* parent);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] QString listId() const { return list_id_; }
    [[nodiscard]] EngineSession* session() const { return session_; }
    // Shows a list of an engine; null or empty shows nothing.
    void show(EngineSession* session, const QString& id);
    [[nodiscard]] bool loading() const { return loading_; }
    [[nodiscard]] int trackCount() const;
    [[nodiscard]] qreal totalDuration() const;

    // Asks the engine for the list again.
    void reload();

    // Plays the list from a track: the list becomes the engine's queue.
    Q_INVOKABLE void play(int row);
    // Adds tracks to up-next, in order.
    Q_INVOKABLE void enqueue(const QList<int>& rows);
    Q_INVOKABLE [[nodiscard]] int rowOfEntry(const QString& entry) const;
    // One line about some rows, for the status bar.
    Q_INVOKABLE [[nodiscard]] QString describe(const QList<int>& rows) const;
    // The track rows of the album whose header this is.
    Q_INVOKABLE [[nodiscard]] QList<int> groupRows(int headerRow) const;

    // Where in the list's items a view row is: the number of tracks before
    // it. rowCount() gives the end.
    Q_INVOKABLE [[nodiscard]] int itemIndexAt(int row) const;
    // Edits, by view rows, saved to the engine at the revision shown.
    // `before` is an item position (itemIndexAt).
    Q_INVOKABLE void moveRows(const QList<int>& rows, int before);
    Q_INVOKABLE void removeRows(const QList<int>& rows);
    // Copies into another list of the same engine, at its end.
    Q_INVOKABLE void copyToList(const QList<int>& rows, const QString& listId);
    // The next track row after `from` (before it, with `step` -1) whose
    // title, artist or album holds `text`, ignoring case, wrapping round;
    // -1 when none does. Finding never filters or changes anything.
    Q_INVOKABLE [[nodiscard]] int find(const QString& text, int from, int step) const;
    // A track's rating, or on a header row its album's; 0 clears it.
    Q_INVOKABLE void rate(int row, int rating);

  signals:
    void listIdChanged();
    void loadingChanged();
    // The same list is about to be shown again, changed: a view keeps its
    // place rather than jumping to the top.
    void refreshing();
    void loaded();

  private:
    struct Row final {
        bool header{false};
        QString entry;
        QString path;
        QString title;
        QString artist;
        QString album;
        QString date;
        int number{0};
        qint64 duration_ms{0};
        int group_tracks{0};
        qint64 group_duration_ms{0};
        // The engine's item as it came, written back as it came.
        EngineClient::Json item;
        // ADR-0179 content identity: this track's, or a header's album's.
        std::string rating_hash;
        int rating{0};
    };

    void adoptItems(const EngineClient::Json& items, bool same_list);
    [[nodiscard]] std::vector<std::size_t> itemIndices(const QList<int>& rows) const;
    // A rating set anywhere, by any client, shown wherever its key is.
    void adoptRating(const std::string& hash, int rating);
    void adoptFacts(const EngineClient::Json& tracks, std::uint64_t generation);
    [[nodiscard]] const Row* track(int row) const;

    EngineSession* session_{nullptr};
    QMetaObject::Connection reconnected_;
    QMetaObject::Connection rated_;
    QString list_id_;
    // The list whose rows are held, to tell a refresh from a new list.
    QString shown_;
    bool loading_{false};
    std::uint64_t revision_{0};
    std::vector<Row> rows_;
    // Answers for a list no longer shown are dropped by this.
    std::uint64_t generation_{0};
};

} // namespace trackknife::quick
