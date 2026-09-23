// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QHash>
#include <QList>
#include <QTableView>

#include <functional>
#include <vector>

class QAbstractItemModel;
class QAbstractItemView;
class QDrag;
class QDragLeaveEvent;
class QDropEvent;
class QPaintEvent;
class QResizeEvent;
class QUrl;

namespace trackknife::ui {

class QueueTableViewTest;
class LocalFilesMimeData;

// Track-list view with album-header selection, Enter activation, and typed
// drag/drop callbacks. Drops are executed by the owning window's callbacks —
// reorder within the view, copy/move from another view, or local URL ingestion
// — never by the model.
class QueueTableView final : public QTableView {
  public:
    // Qt keeps DropIndicatorPosition protected; the insertion-row mapping is
    // part of this view's public contract (and its tests), so re-expose it.
    using QAbstractItemView::AboveItem;
    using QAbstractItemView::BelowItem;
    using QAbstractItemView::DropIndicatorPosition;
    using QAbstractItemView::OnItem;
    using QAbstractItemView::OnViewport;

    explicit QueueTableView(QWidget* parent);

    void setModel(QAbstractItemModel* model) override;
    // Enables album-header row geometry without QHeaderView::ResizeToContents,
    // which asks the delegate to size every cell in a large model. Group starts
    // receive one bounded per-section override; ordinary rows keep the fixed
    // default height.
    void setAlbumGroupingEnabled(bool enabled);
    [[nodiscard]] bool albumGroupingEnabled() const noexcept { return album_grouping_enabled_; }
    // A grouped presentation may reserve one ordinary model column as an
    // artwork gutter. Covers are painted across the visible rows of each
    // album without row spans, preserving normal row hit-testing and drops.
    void setAlbumArtworkColumn(int column);
    // What an empty list says in its middle: a line, and a quieter hint of
    // how to fill it. Empty strings show nothing.
    void setEmptyMessage(QString title, QString hint);
    [[nodiscard]] QString emptyTitle() const { return empty_title_; }
    [[nodiscard]] int albumArtworkColumn() const noexcept { return album_artwork_column_; }
    // Keeps the visible columns exactly fitted to the viewport. Non-expanding
    // columns retain their preferred width; expanding columns divide the
    // remainder proportionally and all columns retain their declared minimum.
    void setAutoFillColumns(QList<int> expanding_columns, QHash<int, int> preferred_widths,
                            QHash<int, int> minimum_widths);

    void setReorderCallback(std::function<void(const QVariantList&, int)> callback);
    void setExternalDropCallback(
        std::function<bool(QAbstractItemView*, const QVariantList&, int, Qt::DropAction)> callback);
    void setActivateCallback(std::function<void(const QModelIndex&)> callback);
    void setLocalUrlDropCallback(std::function<bool(const QList<QUrl>&, int)> callback);
    void setLocalFilesDropCallback(std::function<bool(const LocalFilesMimeData&, int)> callback);

    // Maps a drop position to the row content should be inserted before.
    // Above/on the hovered row inserts before it, below inserts after it, and
    // empty space appends.
    [[nodiscard]] int dropInsertionRow(const QPoint& position,
                                       DropIndicatorPosition indicator) const;
    // Resolves the actual drag target from row geometry. The upper half inserts
    // before, the lower half inserts after, and an album header targets the
    // beginning of that album. This is also the row painted by the custom
    // insertion marker and passed to callbacks.
    [[nodiscard]] int resolvedDropInsertionRow(const QPoint& position) const;

  protected:
    void rowsInserted(const QModelIndex& parent, int start, int end) override;
    QModelIndex moveCursor(CursorAction action, Qt::KeyboardModifiers modifiers) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void startDrag(Qt::DropActions supported_actions) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void currentChanged(const QModelIndex& current, const QModelIndex& previous) override;
    void resizeEvent(QResizeEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;

  private:
    friend class QueueTableViewTest;

    [[nodiscard]] bool handlesDrag(const QDropEvent* event) const;
    void showDropTarget(int insertion_row, Qt::DropAction action);
    void clearDropTarget();
    void finishDragPresentation();
    void updateArtworkOverlayGeometry();
    void updateArtworkOverlay();
    void refitColumnsToViewport();
    void rebuildAlbumRowGeometry();
    void refreshAlbumRowGeometry(int first_row, int last_row);

    std::function<void(const QVariantList&, int)> reorder_callback_;
    std::function<bool(QAbstractItemView*, const QVariantList&, int, Qt::DropAction)>
        external_drop_callback_;
    std::function<bool(const QList<QUrl>&, int)> local_url_drop_callback_;
    std::function<bool(const LocalFilesMimeData&, int)> local_files_drop_callback_;
    std::function<void(const QModelIndex&)> activate_callback_;
    std::function<Qt::DropAction(QDrag*, Qt::DropActions, Qt::DropAction)>
        drag_executor_for_testing_;
    int album_artwork_column_{-1};
    int drop_target_insertion_row_{-1};
    Qt::DropAction drop_target_action_{Qt::MoveAction};
    bool album_grouping_enabled_{false};
    std::vector<int> album_group_start_rows_;
    bool album_group_cache_valid_{false};
    std::vector<QMetaObject::Connection> album_model_connections_;
    QList<int> expanding_columns_;
    QHash<int, int> preferred_column_widths_;
    QHash<int, int> minimum_column_widths_;
    bool auto_fill_columns_{false};
    bool refitting_columns_{false};
    QString empty_title_;
    QString empty_hint_;
};

} // namespace trackknife::ui
