// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "uicommon/track_grouping.hpp"
#include "uicommon/track_view_layout.hpp"

#include <QAbstractListModel>
#include <QPointer>
#include <QSet>

namespace trackknife::bench {
class LocalListModel;
}

namespace trackknife::quick {

// The list on show as the Qt Quick window draws it: a row per track, each
// with its cells as the album view's rules say (ui::trackCell) and what its
// group adds above it -- the same rules the widgets delegate draws by. It
// also holds the selection, by entry, so it stays on the same tracks when
// rows move, are undone or change on the engine.
class TrackRowsModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(bool grouped READ grouped NOTIFY layoutChanged)
    Q_PROPERTY(bool sideArtwork READ sideArtwork NOTIFY layoutChanged)
    Q_PROPERTY(QVariantList columns READ columns NOTIFY layoutChanged)
    Q_PROPERTY(int currentRow READ currentRow NOTIFY currentRowChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)

  public:
    enum Role : int {
        cells_role = Qt::UserRole + 1,
        spacing_role,
        group_start_role,
        loose_run_role,
        disc_role,
        header_album_role,
        header_details_role,
        cover_key_role,
        cover_offset_role,
        group_height_role,
        album_rating_role,
        selected_role,
        current_track_role,
        entry_role,
        tooltip_role,
    };

    explicit TrackRowsModel(QObject* parent);

    // The list shown and how: its model, its columns, and the width they
    // are fitted to.
    void setSource(bench::LocalListModel* source, const ui::TrackViewLayout& layout);
    void setLayout(const ui::TrackViewLayout& layout);
    Q_INVOKABLE void setViewportWidth(int width);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] bool grouped() const;
    [[nodiscard]] bool sideArtwork() const;
    // The visible columns in order: id, logical, header text, fitted width.
    [[nodiscard]] QVariantList columns() const { return columns_; }
    [[nodiscard]] int currentRow() const;
    [[nodiscard]] int selectedCount() const { return static_cast<int>(selected_.size()); }

    // Selection, as a table's: a click selects one; with Ctrl it is added
    // or taken away; with Shift, the rows from the anchor to it.
    Q_INVOKABLE void press(int row, int modifiers);
    Q_INVOKABLE void moveCurrent(int row, int modifiers);
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void selectRows(const QVariantList& rows, int current = -1);
    Q_INVOKABLE void selectGroup(int row);
    [[nodiscard]] std::vector<int> selectedRows() const;
    [[nodiscard]] Q_INVOKABLE QVariantList selectedRowList() const;
    // The row's y, from the top of the list, and its height.
    [[nodiscard]] Q_INVOKABLE int rowHeight(int row) const;

  signals:
    void countChanged();
    void layoutChanged();
    void currentRowChanged();
    void selectionChanged();

  private:
    [[nodiscard]] int spacingOf(int row) const;
    [[nodiscard]] QString entryOf(int row) const;
    [[nodiscard]] int rowOfEntry(const QString& entry) const;
    [[nodiscard]] ui::TrackCellContext cellContext() const;
    [[nodiscard]] QVariantList cellsOf(int row) const;
    void refit();
    void refreshAll();
    void selectionEdited();

    QPointer<bench::LocalListModel> source_;
    ui::TrackViewLayout layout_;
    QSet<int> hidden_;
    QVariantList columns_;
    int viewport_width_{0};
    QSet<QString> selected_;
    QString current_;
    QString anchor_;
};

} // namespace trackknife::quick
