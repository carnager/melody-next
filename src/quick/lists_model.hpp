// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "quick/engine_client.hpp"

#include <QAbstractListModel>
#include <QTimer>
#include <QtQmlIntegration>

#include <vector>

namespace trackknife::quick {

// The engine's lists (ADR-0233), working and saved, as the engine has them.
class ListsModel final : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Reached through Engine.lists")

  public:
    enum Role : int { IdRole = Qt::UserRole + 1, NameRole, KindRole, TracksRole, SavedRole, ModifiedRole };

    explicit ListsModel(EngineClient& client, QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE [[nodiscard]] int indexOf(const QString& id) const;
    Q_INVOKABLE [[nodiscard]] QString idAt(int row) const;
    Q_INVOKABLE [[nodiscard]] QString nameAt(int row) const;
    Q_INVOKABLE [[nodiscard]] QString nameOf(const QString& id) const;
    Q_INVOKABLE [[nodiscard]] bool isSaved(const QString& id) const;
    // The list to open when this window has none of the engine's: the one
    // changed last, working lists first.
    Q_INVOKABLE [[nodiscard]] QString newest() const;

  signals:
    // Written or deleted by any client, this one included.
    void listChanged(const QString& id);
    void loaded();

  private:
    void refresh();

    struct Summary final {
        QString id;
        QString name;
        bool saved{false};
        int tracks{0};
        qint64 modified_ms{0};
    };
    EngineClient& client_;
    std::vector<Summary> lists_;
    QTimer debounce_;
};

} // namespace trackknife::quick
