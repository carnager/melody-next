// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QAbstractTableModel>
#include <QQmlEngine>
#include <QStringList>
#include <QVariantList>

namespace trackknife::quick {

// Rows of text for a QML TableView: each row a list of strings, as the
// windows that only list things -- problems, loudness sources, folder
// covers -- are given them.
class TextRows : public QAbstractTableModel {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList rows READ rows WRITE setRows NOTIFY rowsChanged)

  public:
    using QAbstractTableModel::QAbstractTableModel;

    [[nodiscard]] QVariantList rows() const { return rows_; }
    void setRows(const QVariantList& rows);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;

  signals:
    void rowsChanged();

  private:
    QVariantList rows_;
    int columns_{0};
};

} // namespace trackknife::quick
