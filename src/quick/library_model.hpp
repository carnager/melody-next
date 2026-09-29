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

// The engine's library as albums, each expandable to its tracks, filtered
// by a search text. Pages arrive as the view scrolls (fetchMore).
class LibraryModel final : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Reached through Engine.library")

    Q_PROPERTY(QString search READ search WRITE setSearch NOTIFY searchChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)

  public:
    enum Role : int {
        TrackRole = Qt::UserRole + 1,
        KeyRole,
        TitleRole,
        ArtistRole,
        DateRole,
        TracksRole,
        NumberRole,
        DurationRole,
        ExpandedRole,
        CoverRole,
    };

    LibraryModel(EngineClient& client, EngineSession& session, QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

    [[nodiscard]] QString search() const { return search_; }
    // Debounced: typing does not send a query per keystroke.
    void setSearch(const QString& text);
    [[nodiscard]] bool loading() const { return loading_; }

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
    struct Row final {
        bool track{false};
        QString key;
        QString title;
        QString artist;
        QString date;
        int tracks{0};
        int number{0};
        qint64 duration_ms{0};
        bool expanded{false};
    };

    // The tracks a row stands for, in order; an album's are asked for.
    void resolve(int row, std::function<void(std::vector<QueuedTrack>)> done);
    void restart();
    void requestPage();
    [[nodiscard]] static Row rowFrom(const EngineClient::Json& entry);
    [[nodiscard]] int rowOfAlbum(const QString& key) const;

    EngineClient& client_;
    EngineSession& session_;
    QString search_;
    QTimer debounce_;
    std::vector<Row> rows_;
    std::size_t albums_loaded_{0};
    bool more_{true};
    bool loading_{false};
    std::uint64_t generation_{0};
};

} // namespace trackknife::quick
