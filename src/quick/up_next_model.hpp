// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "quick/engine_client.hpp"

#include <QAbstractListModel>
#include <QHash>
#include <QtQmlIntegration>

#include <vector>

namespace trackknife::quick {

struct QueuedTrack final {
    QString entry;
    QString path;
    QString title;
    QString artist;
    QString album;
    QString date;
    qint64 duration_ms{0};
};

// The engine's up-next requests, in order. The engine answers with
// identities; what they are comes from its queue, or, for tracks added from
// here that are not in the playing list, from what was sent.
class UpNextModel final : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Reached through Engine.upNext")

  public:
    enum Role : int { EntryRole = Qt::UserRole + 1, TitleRole, ArtistRole, AlbumRole, DurationRole, CoverRole };

    UpNextModel(EngineClient& client, int session, QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void remember(const QueuedTrack& track);
    void refresh();

    Q_INVOKABLE void remove(int row);
    Q_INVOKABLE void move(int from, int to);
    Q_INVOKABLE void clear();

  private:
    void adopt(const std::vector<QString>& entries);
    void sendOrder();

    EngineClient& client_;
    int session_;
    QHash<QString, QueuedTrack> known_;
    std::vector<QueuedTrack> rows_;
    std::uint64_t generation_{0};
};

} // namespace trackknife::quick
