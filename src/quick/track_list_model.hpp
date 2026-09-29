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

  signals:
    void listIdChanged();
    void loadingChanged();
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
    };

    void adoptItems(const EngineClient::Json& items);
    void adoptFacts(const EngineClient::Json& tracks, std::uint64_t generation);
    [[nodiscard]] const Row* track(int row) const;

    EngineSession* session_{nullptr};
    QMetaObject::Connection reconnected_;
    QString list_id_;
    bool loading_{false};
    std::vector<Row> rows_;
    // Answers for a list no longer shown are dropped by this.
    std::uint64_t generation_{0};
};

} // namespace trackknife::quick
