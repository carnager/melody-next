// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/queue_table_view.hpp"
#include "uicommon/local_files_mime_data.hpp"

#include "uicommon/queue_item_delegate.hpp"
#include "uicommon/rating_stars.hpp"
#include "uicommon/track_row_roles.hpp"

#include <QAbstractItemModel>
#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFontMetrics>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QPixmap>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStyle>

#include <algorithm>
#include <utility>

namespace trackknife::ui {
namespace {

constexpr int maximum_side_artwork_extent = 160;
constexpr int artwork_padding = 6;

[[nodiscard]] int viewColumn(const QTableView* view, const char* property, const int fallback) {
    return view->property(property).isValid() ? view->property(property).toInt() : fallback;
}

[[nodiscard]] QString groupKey(const QTableView* view, const int row) {
    const auto* model = view->model();
    if (model == nullptr || row < 0 || row >= model->rowCount()) {
        return {};
    }
    const auto album_column = viewColumn(view, track_album_column_property, track_album_column);
    const auto date_column = viewColumn(view, track_date_column_property, track_date_column);
    const auto anchor = model->index(row, 0);
    return anchor.data(track_album_artist_role).toString() + QChar::Null +
           model->index(row, album_column).data().toString() + QChar::Null +
           model->index(row, date_column).data().toString();
}

[[nodiscard]] bool beginsAlbum(const QTableView* view, const int row) {
    const auto* model = view->model();
    if (model == nullptr || row < 0 || row >= model->rowCount()) {
        return false;
    }
    const auto cached = model->index(row, 0).data(track_album_group_start_role);
    if (cached.isValid()) {
        return cached.toBool();
    }
    const auto key = groupKey(view, row);
    return (row == 0 || key != groupKey(view, row - 1)) && row + 1 < model->rowCount() &&
           key == groupKey(view, row + 1);
}

[[nodiscard]] QString albumTitle(const QTableView* view, const int row) {
    const auto album_column = viewColumn(view, track_album_column_property, track_album_column);
    const auto date_column = viewColumn(view, track_date_column_property, track_date_column);
    const auto anchor = view->model()->index(row, 0);
    const auto artist = anchor.data(track_album_artist_role).toString();
    const auto album = view->model()->index(row, album_column).data().toString();
    const auto date = view->model()->index(row, date_column).data().toString();
    auto title = artist.isEmpty() ? QStringLiteral("Unknown artist") : artist;
    title += QStringLiteral(" — ");
    title += album.isEmpty() ? QStringLiteral("Unknown album") : album;
    if (!date.isEmpty()) {
        title += QStringLiteral(" (%1)").arg(date);
    }
    return title;
}

[[nodiscard]] QString albumDuration(const QTableView* view, const int first_row,
                                    const QString& key) {
    qint64 milliseconds = 0;
    for (int row = first_row; row < view->model()->rowCount() && groupKey(view, row) == key;
         ++row) {
        milliseconds += view->model()->index(row, 0).data(track_duration_ms_role).toLongLong();
    }
    const auto seconds = std::max<qint64>(0, milliseconds / 1'000);
    const auto hours = seconds / 3'600;
    const auto minutes = (seconds / 60) % 60;
    const auto remainder = seconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(remainder, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(minutes).arg(remainder, 2, 10, QLatin1Char('0'));
}

void paintAlbumArtwork(QueueTableView* view, QPainter* painter) {
    const auto column = view->albumArtworkColumn();
    auto* model = view->model();
    if (column < 0 || model == nullptr || model->rowCount() == 0 || view->isColumnHidden(column)) {
        return;
    }
    auto first_visible = view->rowAt(0);
    if (first_visible < 0) {
        first_visible = 0;
    }
    auto last_visible = view->rowAt(view->viewport()->height() - 1);
    if (last_visible < 0) {
        last_visible = model->rowCount() - 1;
    }
    // Covers are capped, so group starts more than this many dense rows above
    // the viewport cannot contribute a visible pixel.
    const auto first_candidate = std::max(0, first_visible - 10);
    const auto last_candidate = std::min(model->rowCount() - 1, last_visible + 1);
    const auto artwork_left = view->horizontalHeader()->sectionViewportPosition(column);
    const auto artwork_width = view->horizontalHeader()->sectionSize(column);
    const auto& palette = view->palette();
    for (int row = first_candidate; row <= last_candidate; ++row) {
        if (row > 0 && groupKey(view, row) == groupKey(view, row - 1)) {
            continue;
        }
        const auto top = view->rowViewportPosition(row);
        if (top >= view->viewport()->height()) {
            break;
        }
        const auto key = groupKey(view, row);
        if (row + 1 >= model->rowCount() || groupKey(view, row + 1) != key) {
            continue;
        }
        auto group_bottom = top + view->rowHeight(row);
        for (int next = row + 1;
             next < model->rowCount() && groupKey(view, next) == key &&
             group_bottom < top + QueueItemDelegate::album_header_height +
                                maximum_side_artwork_extent + artwork_padding * 2;
             ++next) {
            group_bottom = view->rowViewportPosition(next) + view->rowHeight(next);
        }

        const QRect header{0, top, view->viewport()->width(),
                           QueueItemDelegate::album_header_height};
        painter->fillRect(header, palette.alternateBase());
        painter->setPen(palette.mid().color());
        painter->drawLine(header.bottomLeft(), header.bottomRight());
        auto header_font = view->font();
        header_font.setBold(true);
        painter->setFont(header_font);
        painter->setPen(palette.text().color());
        const auto duration = albumDuration(view, row, key);
        const auto duration_width = painter->fontMetrics().horizontalAdvance(duration) + 12;
        const auto label_rect = header.adjusted(6, 0, -duration_width, 0);
        painter->drawText(label_rect, Qt::AlignVCenter | Qt::AlignLeft,
                          painter->fontMetrics().elidedText(albumTitle(view, row), Qt::ElideRight,
                                                            label_rect.width()));
        painter->drawText(header.adjusted(6, 0, -6, 0), Qt::AlignVCenter | Qt::AlignRight,
                          duration);

        const QRect available =
            QRect{artwork_left, top + QueueItemDelegate::album_header_height, artwork_width,
                  std::max(0, group_bottom - top - QueueItemDelegate::album_header_height)}
                .adjusted(artwork_padding, artwork_padding, -artwork_padding, -artwork_padding);
        const auto extent = std::max(
            0, std::min({available.width(), available.height(), maximum_side_artwork_extent}));
        if (extent <= 0 || available.bottom() < 0) {
            continue;
        }
        const QRect target{available.left(), available.top(), extent, extent};
        const auto cover = model->index(row, column).data(track_album_artwork_role).value<QImage>();
        const auto album_rating = model->index(row, column).data(track_album_rating_role).toUInt();
        if (!cover.isNull()) {
            const auto fitted = cover.size().scaled(target.size(), Qt::KeepAspectRatio);
            const QRect centered{target.center().x() - fitted.width() / 2,
                                 target.center().y() - fitted.height() / 2, fitted.width(),
                                 fitted.height()};
            painter->drawImage(centered, cover);
            paintRatingOverlay(painter, centered, album_rating);
        } else {
            const auto icon =
                QIcon::fromTheme(QStringLiteral("media-optical-audio"),
                                 QApplication::style()->standardIcon(QStyle::SP_FileIcon));
            icon.paint(painter, target, Qt::AlignCenter, QIcon::Disabled);
            paintRatingOverlay(painter, target, album_rating);
        }
    }
}

[[nodiscard]] QString dropTargetLabel(const QTableView* view, const int insertion_row,
                                      const Qt::DropAction action) {
    const auto operation =
        action == Qt::CopyAction ? QStringLiteral("Copy") : QStringLiteral("Move");
    const auto rows = view->model() == nullptr ? 0 : view->model()->rowCount();
    return insertion_row >= rows
               ? QStringLiteral("%1 to end").arg(operation)
               : QStringLiteral("%1 here · position %2").arg(operation).arg(insertion_row + 1);
}

void paintDropTarget(QueueTableView* view, QPainter* painter, const int insertion_row,
                     const Qt::DropAction action) {
    if (insertion_row < 0 || view->model() == nullptr || view->viewport()->width() < 32 ||
        view->viewport()->height() < 16) {
        return;
    }
    const auto rows = view->model()->rowCount();
    int line_y = 3;
    if (rows > 0 && insertion_row <= 0) {
        line_y = view->rowViewportPosition(0);
    } else if (rows > 0 && insertion_row >= rows) {
        const auto last = rows - 1;
        line_y = view->rowViewportPosition(last) + view->rowHeight(last);
    } else if (rows > 0) {
        line_y = view->rowViewportPosition(insertion_row);
    }
    const auto width = view->viewport()->width();
    const auto height = view->viewport()->height();
    line_y = std::clamp(line_y, 3, height - 4);

    painter->setRenderHint(QPainter::Antialiasing);
    const auto accent = view->palette().highlight().color();
    auto halo = view->palette().base().color();
    halo.setAlpha(220);
    painter->fillRect(QRect{8, line_y - 3, std::max(0, width - 16), 7}, halo);
    painter->fillRect(QRect{8, line_y - 1, std::max(0, width - 16), 3}, accent);
    const QPolygon arrow{{2, line_y - 7}, {12, line_y}, {2, line_y + 7}};
    painter->setPen(Qt::NoPen);
    painter->setBrush(accent);
    painter->drawPolygon(arrow);

    auto badge_font = view->font();
    badge_font.setBold(true);
    painter->setFont(badge_font);
    const QFontMetrics metrics{badge_font};
    const auto text = dropTargetLabel(view, insertion_row, action);
    const auto badge_width = metrics.horizontalAdvance(text) + 18;
    const auto badge_height = metrics.height() + 8;
    const auto badge_left = std::max(16, width - badge_width - 12);
    auto badge_top = line_y + 7;
    if (badge_top + badge_height > height - 4) {
        badge_top = line_y - badge_height - 7;
    }
    badge_top = std::clamp(badge_top, 4, std::max(4, height - badge_height - 4));
    const QRect badge{badge_left, badge_top, std::min(badge_width, width - badge_left - 4),
                      badge_height};
    painter->drawRoundedRect(badge, 5, 5);
    painter->setPen(view->palette().highlightedText().color());
    painter->drawText(badge.adjusted(9, 0, -9, 0), Qt::AlignCenter, text);
}

[[nodiscard]] Qt::DropAction resolvedDropAction(const QDropEvent* event) {
    auto action = event->dropAction();
    if (action == Qt::IgnoreAction) {
        action = event->proposedAction();
    }
    return action == Qt::CopyAction ? Qt::CopyAction : Qt::MoveAction;
}

} // namespace

QueueTableView::QueueTableView(QWidget* parent) : QTableView(parent) {
    setProperty("trackknife-drop-insertion-row", -1);
    setProperty("trackknife-drop-target-label", QString{});
    // QVariant::toInt() maps an absent property to zero, which otherwise makes
    // an unconfigured view render its first row as permanently hovered.
    setProperty("trackknife-hover-row", -1);
    connect(horizontalHeader(), &QHeaderView::sectionMoved, this,
            [this](const int, const int, const int) { updateArtworkOverlayGeometry(); });
    connect(horizontalHeader(), &QHeaderView::sectionResized, this,
            [this](const int logical, const int, const int size) {
                updateArtworkOverlayGeometry();
                if (auto_fill_columns_ && !refitting_columns_) {
                    preferred_column_widths_.insert(logical, size);
                    refitColumnsToViewport();
                }
            });
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this,
            [this](const int) { updateArtworkOverlayGeometry(); });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](const int) { updateArtworkOverlay(); });
}

void QueueTableView::setModel(QAbstractItemModel* model) {
    clearDropTarget();
    for (const auto& connection : album_model_connections_) {
        disconnect(connection);
    }
    album_model_connections_.clear();
    album_group_start_rows_.clear();
    album_group_cache_valid_ = false;
    QTableView::setModel(model);
    if (model != nullptr) {
        const auto refresh = [this] { updateArtworkOverlay(); };
        album_model_connections_.push_back(
            connect(model, &QAbstractItemModel::dataChanged, this,
                    [this, refresh](const QModelIndex& first, const QModelIndex& last,
                                    const QList<int>& roles) {
                        refresh();
                        if (roles.isEmpty() || roles.contains(Qt::DisplayRole) ||
                            roles.contains(track_album_artist_role) ||
                            roles.contains(track_album_group_start_role)) {
                            album_group_cache_valid_ = false;
                            refreshAlbumRowGeometry(first.row() - 1, last.row() + 1);
                        }
                    }));
        album_model_connections_.push_back(
            connect(model, &QAbstractItemModel::modelReset, this, [this, refresh, model] {
                refresh();
                album_group_cache_valid_ = false;
                // QTableView and its vertical header finish their own reset
                // bookkeeping after modelReset observers return. Rebuilding
                // sections synchronously here can leave every post-drag row
                // except the first with stale/empty paint geometry.
                QMetaObject::invokeMethod(
                    this,
                    [this, model] {
                        if (album_grouping_enabled_ && this->model() == model) {
                            rebuildAlbumRowGeometry();
                        }
                    },
                    Qt::QueuedConnection);
            }));
        album_model_connections_.push_back(
            connect(model, &QAbstractItemModel::rowsRemoved, this,
                    [this, refresh](const QModelIndex&, const int first, const int) {
                        refresh();
                        album_group_cache_valid_ = false;
                        refreshAlbumRowGeometry(first - 1, first + 1);
                    }));
        album_model_connections_.push_back(
            connect(model, &QAbstractItemModel::rowsMoved, this, [this, refresh, model] {
                refresh();
                album_group_cache_valid_ = false;
                QMetaObject::invokeMethod(
                    this,
                    [this, model] {
                        if (album_grouping_enabled_ && this->model() == model) {
                            rebuildAlbumRowGeometry();
                        }
                    },
                    Qt::QueuedConnection);
            }));
        album_model_connections_.push_back(
            connect(model, &QAbstractItemModel::layoutChanged, this, [this, refresh] {
                refresh();
                album_group_cache_valid_ = false;
                if (album_grouping_enabled_) {
                    rebuildAlbumRowGeometry();
                }
            }));
    }
    if (album_grouping_enabled_) {
        rebuildAlbumRowGeometry();
    }
    updateArtworkOverlay();
}

void QueueTableView::rowsInserted(const QModelIndex& parent, const int start, const int end) {
    QTableView::rowsInserted(parent, start, end);
    updateArtworkOverlay();
    album_group_cache_valid_ = false;
    if (model() == nullptr || start < 0 || end < start) {
        return;
    }

    // QHeaderView finalizes inserted sections after this virtual callback
    // returns. A resize here is discarded, which hid the first already-enriched
    // CUE row beneath its 30 px album header. Regular files happened to receive
    // a later dataChanged refresh after probing. Persistent indexes keep a
    // localized deferred refresh correctly anchored if another insertion moves
    // these rows before the callback runs.
    const QPersistentModelIndex first_anchor{model()->index(start, 0)};
    const QPersistentModelIndex last_anchor{model()->index(end, 0)};
    const auto large_batch = end - start > 256;
    QMetaObject::invokeMethod(
        this,
        [this, first_anchor, last_anchor, large_batch] {
            if (!album_grouping_enabled_ || model() == nullptr || !first_anchor.isValid() ||
                !last_anchor.isValid() || first_anchor.model() != model() ||
                last_anchor.model() != model()) {
                return;
            }
            if (large_batch) {
                rebuildAlbumRowGeometry();
            } else {
                refreshAlbumRowGeometry(first_anchor.row() - 1, last_anchor.row() + 1);
            }
        },
        Qt::QueuedConnection);
}

void QueueTableView::setAlbumGroupingEnabled(const bool enabled) {
    if (album_grouping_enabled_ == enabled) {
        return;
    }
    album_grouping_enabled_ = enabled;
    rebuildAlbumRowGeometry();
}

void QueueTableView::rebuildAlbumRowGeometry() {
    if (verticalHeader() == nullptr) {
        return;
    }
    const auto default_height = verticalHeader()->defaultSectionSize();
    const auto minimum_height = verticalHeader()->minimumSectionSize();
    auto* row_header = verticalHeader();
    row_header->reset();
    row_header->setDefaultSectionSize(default_height);
    row_header->setMinimumSectionSize(minimum_height);
    row_header->setSectionResizeMode(QHeaderView::Fixed);
    // reset() rebuilds the header's section bookkeeping, but it deliberately
    // preserves explicit per-row sizes. Clear the old album-start overrides
    // before calculating the new ones (and when switching to a plain view).
    // Rows that became group starts through an incremental insertion are not
    // necessarily in the cached group list, so reset every current section.
    {
        const QSignalBlocker blocker{row_header};
        for (int row = 0; row < row_header->count(); ++row) {
            row_header->resizeSection(row, default_height);
        }
    }
    row_header->hide();
    if (!album_grouping_enabled_ || model() == nullptr || model()->rowCount() < 2) {
        updateGeometries();
        return;
    }

    setUpdatesEnabled(false);
    const QSignalBlocker blocker{row_header};
    if (!album_group_cache_valid_) {
        album_group_start_rows_.clear();
        album_group_start_rows_.reserve(
            static_cast<std::size_t>(std::max(1, model()->rowCount() / 10)));
        for (int row = 0; row < model()->rowCount(); ++row) {
            if (beginsAlbum(this, row)) {
                album_group_start_rows_.push_back(row);
            }
        }
        album_group_cache_valid_ = true;
    }
    for (const auto row : album_group_start_rows_) {
        row_header->resizeSection(row, default_height + QueueItemDelegate::album_header_height);
    }
    setUpdatesEnabled(true);
    // Section-size signals are deliberately batched above. QTableView therefore
    // needs one explicit scroll-range update after the final header heights.
    updateGeometries();
    viewport()->update();
}

void QueueTableView::refreshAlbumRowGeometry(const int first_row, const int last_row) {
    if (!album_grouping_enabled_ || model() == nullptr || model()->rowCount() == 0 ||
        verticalHeader() == nullptr) {
        return;
    }
    const auto first = std::max(0, first_row);
    const auto last = std::min(model()->rowCount() - 1, last_row);
    const auto default_height = verticalHeader()->defaultSectionSize();
    const QSignalBlocker blocker{verticalHeader()};
    for (int row = first; row <= last; ++row) {
        verticalHeader()->resizeSection(
            row,
            default_height + (beginsAlbum(this, row) ? QueueItemDelegate::album_header_height : 0));
    }
    // Section-size signals are deliberately batched above. QTableView therefore
    // needs one explicit scroll-range update after the final header heights.
    updateGeometries();
    viewport()->update();
}

void QueueTableView::setAlbumArtworkColumn(const int column) {
    album_artwork_column_ = column;
    updateArtworkOverlayGeometry();
}

void QueueTableView::setAutoFillColumns(QList<int> expanding_columns,
                                        QHash<int, int> preferred_widths,
                                        QHash<int, int> minimum_widths) {
    expanding_columns_ = std::move(expanding_columns);
    preferred_column_widths_ = std::move(preferred_widths);
    minimum_column_widths_ = std::move(minimum_widths);
    auto_fill_columns_ = true;
    refitColumnsToViewport();
}

void QueueTableView::refitColumnsToViewport() {
    if (!auto_fill_columns_ || model() == nullptr || viewport()->width() <= 0 ||
        refitting_columns_) {
        return;
    }
    QList<int> visible;
    visible.reserve(model()->columnCount());
    for (int visual = 0; visual < horizontalHeader()->count(); ++visual) {
        const auto logical = horizontalHeader()->logicalIndex(visual);
        if (!isColumnHidden(logical)) {
            visible.push_back(logical);
        }
    }
    if (visible.isEmpty()) {
        return;
    }
    QList<int> expanding;
    for (const auto column : expanding_columns_) {
        if (visible.contains(column)) {
            expanding.push_back(column);
        }
    }
    if (expanding.isEmpty()) {
        expanding.push_back(visible.back());
    }

    const auto minimum = [this](const int column) {
        return std::max(
            horizontalHeader()->minimumSectionSize(),
            minimum_column_widths_.value(column, horizontalHeader()->minimumSectionSize()));
    };
    const auto preferred = [this, &minimum](const int column) {
        return std::max(minimum(column), preferred_column_widths_.value(
                                             column, horizontalHeader()->sectionSize(column)));
    };

    QHash<int, int> target_widths;
    int fixed_total = 0;
    int expanding_minimum_total = 0;
    for (const auto column : visible) {
        if (expanding.contains(column)) {
            expanding_minimum_total += minimum(column);
        } else {
            const auto width = preferred(column);
            target_widths.insert(column, width);
            fixed_total += width;
        }
    }

    auto available_for_expanding = viewport()->width() - fixed_total;
    if (available_for_expanding < expanding_minimum_total) {
        // A narrow viewport may require compact columns to participate too.
        expanding = visible;
        target_widths.clear();
        expanding_minimum_total = 0;
        for (const auto column : expanding) {
            expanding_minimum_total += minimum(column);
        }
        available_for_expanding = viewport()->width();
    }

    const auto distributable = std::max(0, available_for_expanding - expanding_minimum_total);
    int total_weight = 0;
    for (const auto column : expanding) {
        total_weight += std::max(1, preferred(column) - minimum(column));
    }
    auto remaining = available_for_expanding;
    for (qsizetype index = 0; index < expanding.size(); ++index) {
        const auto column = expanding.at(index);
        int width = minimum(column);
        if (index + 1 == expanding.size()) {
            width = std::max(width, remaining);
        } else if (total_weight > 0) {
            const auto weight = std::max(1, preferred(column) - minimum(column));
            width += distributable * weight / total_weight;
        }
        target_widths.insert(column, width);
        remaining -= width;
    }

    refitting_columns_ = true;
    const QSignalBlocker blocker{horizontalHeader()};
    for (const auto column : visible) {
        horizontalHeader()->resizeSection(column, target_widths.value(column, minimum(column)));
    }
    refitting_columns_ = false;
    updateArtworkOverlayGeometry();
}

void QueueTableView::updateArtworkOverlayGeometry() {
    if (viewport() != nullptr) {
        viewport()->update();
    }
}

void QueueTableView::updateArtworkOverlay() {
    if (viewport() != nullptr) {
        viewport()->update();
    }
}

void QueueTableView::setReorderCallback(std::function<void(const QVariantList&, int)> callback) {
    reorder_callback_ = std::move(callback);
}

void QueueTableView::setExternalDropCallback(
    std::function<bool(QAbstractItemView*, const QVariantList&, int, Qt::DropAction)> callback) {
    external_drop_callback_ = std::move(callback);
}

void QueueTableView::setActivateCallback(std::function<void(const QModelIndex&)> callback) {
    activate_callback_ = std::move(callback);
}

void QueueTableView::setLocalFilesDropCallback(
    std::function<bool(const LocalFilesMimeData&, int)> callback) {
    local_files_drop_callback_ = std::move(callback);
}

void QueueTableView::setLocalUrlDropCallback(
    std::function<bool(const QList<QUrl>&, int)> callback) {
    local_url_drop_callback_ = std::move(callback);
}

int QueueTableView::dropInsertionRow(const QPoint& position,
                                     const DropIndicatorPosition indicator) const {
    const auto hovered = indexAt(position);
    if (!hovered.isValid()) {
        return model()->rowCount();
    }
    auto row = hovered.row();
    if (indicator == QAbstractItemView::BelowItem) {
        ++row;
    }
    return row;
}

int QueueTableView::resolvedDropInsertionRow(const QPoint& position) const {
    if (model() == nullptr) {
        return 0;
    }
    const auto hovered = indexAt(position);
    if (!hovered.isValid()) {
        return model()->rowCount();
    }
    const auto rect = visualRect(hovered);
    const auto* queue_delegate = qobject_cast<const QueueItemDelegate*>(itemDelegate());
    auto content_top = rect.top();
    if (queue_delegate != nullptr &&
        queue_delegate->isAlbumHeaderHit(hovered, position.y() - rect.top())) {
        return hovered.row();
    }
    if (beginsAlbum(this, hovered.row())) {
        content_top += QueueItemDelegate::album_header_height;
    }
    const auto content_height = std::max(1, rect.bottom() - content_top + 1);
    return position.y() >= content_top + content_height / 2 ? hovered.row() + 1 : hovered.row();
}

QModelIndex QueueTableView::moveCursor(const CursorAction action,
                                       const Qt::KeyboardModifiers modifiers) {
    if ((action == MoveHome || action == MoveEnd) &&
        (modifiers == Qt::NoModifier || modifiers == Qt::ShiftModifier) && model() != nullptr &&
        model()->rowCount() > 0 && model()->columnCount() > 0) {
        // Change the destination only; Qt owns range selection, its anchor,
        // and scrolling for both plain and Shift-modified navigation.
        const auto row = action == MoveHome ? 0 : model()->rowCount() - 1;
        const auto column = currentIndex().isValid() ? currentIndex().column() : 0;
        return model()->index(row, std::clamp(column, 0, model()->columnCount() - 1));
    }
    return QTableView::moveCursor(action, modifiers);
}

void QueueTableView::keyPressEvent(QKeyEvent* event) {
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
        event->modifiers() == Qt::NoModifier && currentIndex().isValid() && activate_callback_) {
        activate_callback_(currentIndex());
        event->accept();
        return;
    }
    QTableView::keyPressEvent(event);
}

void QueueTableView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const auto index = indexAt(event->position().toPoint());
        const auto* queue_delegate = qobject_cast<const QueueItemDelegate*>(itemDelegate());
        const auto item_rect = visualRect(index);
        if (queue_delegate != nullptr &&
            queue_delegate->isAlbumHeaderHit(index,
                                             event->position().toPoint().y() - item_rect.top())) {
            const auto [first, last] = queue_delegate->albumRowRange(index);
            const QItemSelection album{model()->index(first, 0),
                                       model()->index(last, model()->columnCount() - 1)};
            selectionModel()->select(album, QItemSelectionModel::ClearAndSelect |
                                                QItemSelectionModel::Rows);
            selectionModel()->setCurrentIndex(model()->index(first, 1),
                                              QItemSelectionModel::NoUpdate);
            event->accept();
            return;
        }
    }
    QTableView::mousePressEvent(event);
}

bool QueueTableView::handlesDrag(const QDropEvent* event) const {
    if (event->mimeData()->hasFormat(LocalFilesMimeData::mimeType())) {
        return local_files_drop_callback_ &&
               dynamic_cast<const LocalFilesMimeData*>(event->mimeData()) != nullptr;
    }
    if (local_url_drop_callback_ != nullptr && event->mimeData()->hasUrls()) {
        return true;
    }
    if (event->source() != this && external_drop_callback_ != nullptr &&
        qobject_cast<QAbstractItemView*>(event->source()) != nullptr) {
        return true;
    }
    return event->source() == this && reorder_callback_ != nullptr;
}

void QueueTableView::showDropTarget(const int insertion_row, const Qt::DropAction action) {
    drop_target_insertion_row_ =
        std::clamp(insertion_row, 0, model() == nullptr ? 0 : model()->rowCount());
    drop_target_action_ = action;
    setProperty("trackknife-drop-insertion-row", drop_target_insertion_row_);
    setProperty("trackknife-drop-target-label",
                dropTargetLabel(this, drop_target_insertion_row_, drop_target_action_));
    setAccessibleDescription(
        QStringLiteral("%1. The highlighted line is the exact track insertion position")
            .arg(property("trackknife-drop-target-label").toString()));
    viewport()->update();
}

void QueueTableView::clearDropTarget() {
    if (drop_target_insertion_row_ >= 0) {
        drop_target_insertion_row_ = -1;
        setProperty("trackknife-drop-insertion-row", -1);
        setProperty("trackknife-drop-target-label", QString{});
        setAccessibleDescription({});
        if (viewport() != nullptr) {
            viewport()->update();
        }
    }
}

void QueueTableView::finishDragPresentation() {
    clearDropTarget();
    stopAutoScroll();
    setState(QAbstractItemView::NoState);
    if (viewport() != nullptr) {
        viewport()->setUpdatesEnabled(true);
        setDirtyRegion(viewport()->rect());
        viewport()->update();
    }
    updateArtworkOverlay();
}

void QueueTableView::startDrag(const Qt::DropActions supported_actions) {
    if (model() == nullptr) {
        return;
    }
    auto indexes = selectedIndexes();
    indexes.erase(std::remove_if(indexes.begin(), indexes.end(),
                                 [](const QModelIndex& index) {
                                     return !index.flags().testFlag(Qt::ItemIsDragEnabled);
                                 }),
                  indexes.end());
    if (indexes.isEmpty()) {
        return;
    }
    auto* mime_data = model()->mimeData(indexes);
    if (mime_data == nullptr) {
        return;
    }

    QDrag drag{this};
    drag.setMimeData(mime_data);
    const auto row_count =
        selectionModel() == nullptr ? 0 : selectionModel()->selectedRows(0).size();
    const auto summary = row_count == 1 ? QStringLiteral("Move 1 track")
                                        : QStringLiteral("Move %1 tracks").arg(row_count);
    auto summary_font = font();
    summary_font.setBold(true);
    const QFontMetrics summary_metrics{summary_font};
    QPixmap summary_pixmap{summary_metrics.horizontalAdvance(summary) + 24,
                           summary_metrics.height() + 12};
    summary_pixmap.fill(Qt::transparent);
    {
        QPainter painter{&summary_pixmap};
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().highlight());
        painter.drawRoundedRect(summary_pixmap.rect().adjusted(1, 1, -1, -1), 6, 6);
        painter.setPen(palette().highlightedText().color());
        painter.setFont(summary_font);
        painter.drawText(summary_pixmap.rect(), Qt::AlignCenter, summary);
    }
    drag.setPixmap(summary_pixmap);
    drag.setHotSpot(QPoint{12, summary_pixmap.height() / 2});

    auto actions = supported_actions;
    if (dragDropMode() == QAbstractItemView::InternalMove) {
        actions &= ~Qt::CopyAction;
    }
    auto default_action = Qt::IgnoreAction;
    if (defaultDropAction() != Qt::IgnoreAction && actions.testFlag(defaultDropAction())) {
        default_action = defaultDropAction();
    } else if (actions.testFlag(Qt::CopyAction) &&
               dragDropMode() != QAbstractItemView::InternalMove) {
        default_action = Qt::CopyAction;
    }
    if (drag_executor_for_testing_) {
        static_cast<void>(drag_executor_for_testing_(&drag, actions, default_action));
    } else {
        static_cast<void>(drag.exec(actions, default_action));
    }
    // Typed callbacks own reordering/transfers. In contrast, Qt's default
    // startDrag() performs a second source cleanup after MoveAction when its
    // private model-drop flag is unset, which is invalid for this view.
    finishDragPresentation();
}

// The base implementations run first so Qt keeps tracking and painting the
// drop indicator; drags this view handles itself are then force-accepted
// regardless of the model's own drop verdict.
void QueueTableView::dragEnterEvent(QDragEnterEvent* event) {
    QTableView::dragEnterEvent(event);
    if (handlesDrag(event)) {
        setState(QAbstractItemView::DraggingState);
        showDropTarget(resolvedDropInsertionRow(event->position().toPoint()),
                       resolvedDropAction(event));
        event->acceptProposedAction();
    } else {
        clearDropTarget();
    }
}

void QueueTableView::dragMoveEvent(QDragMoveEvent* event) {
    QTableView::dragMoveEvent(event);
    if (handlesDrag(event)) {
        setState(QAbstractItemView::DraggingState);
        showDropTarget(resolvedDropInsertionRow(event->position().toPoint()),
                       resolvedDropAction(event));
        event->acceptProposedAction();
    } else {
        clearDropTarget();
    }
}

void QueueTableView::dragLeaveEvent(QDragLeaveEvent* event) {
    QTableView::dragLeaveEvent(event);
    finishDragPresentation();
}

void QueueTableView::dropEvent(QDropEvent* event) {
    const auto insertion_row = resolvedDropInsertionRow(event->position().toPoint());
    if (event->mimeData()->hasFormat(LocalFilesMimeData::mimeType())) {
        const auto* files = dynamic_cast<const LocalFilesMimeData*>(event->mimeData());
        if (files && local_files_drop_callback_ &&
            local_files_drop_callback_(*files, insertion_row)) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
        } else {
            event->ignore();
        }
        finishDragPresentation();
        return;
    }
    if (local_url_drop_callback_ != nullptr && event->mimeData()->hasUrls()) {
        if (local_url_drop_callback_(event->mimeData()->urls(), insertion_row)) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
            finishDragPresentation();
            return;
        }
    }
    auto* source_view = qobject_cast<QAbstractItemView*>(event->source());
    if (source_view != nullptr && source_view != this && external_drop_callback_ != nullptr) {
        auto selected = source_view->selectionModel()->selectedRows(0);
        std::ranges::sort(selected, {}, &QModelIndex::row);
        QVariantList rows;
        rows.reserve(selected.size());
        for (const auto& index : selected) {
            rows.push_back(index.row());
        }
        auto action = event->dropAction();
        if (action == Qt::IgnoreAction) {
            action = event->proposedAction();
        }
        if (external_drop_callback_(source_view, rows, insertion_row, action)) {
            event->setDropAction(action == Qt::MoveAction ? Qt::MoveAction : Qt::CopyAction);
            event->accept();
            finishDragPresentation();
            return;
        }
    }
    if (event->source() != this || !reorder_callback_ || selectionModel() == nullptr) {
        QTableView::dropEvent(event);
        finishDragPresentation();
        return;
    }
    auto selected = selectionModel()->selectedRows(0);
    std::ranges::sort(selected, {}, &QModelIndex::row);
    QVariantList rows;
    rows.reserve(selected.size());
    for (const auto& index : selected) {
        rows.push_back(index.row());
    }

    reorder_callback_(rows, insertion_row);
    event->setDropAction(Qt::MoveAction);
    event->accept();
    finishDragPresentation();
}

void QueueTableView::paintEvent(QPaintEvent* event) {
    QTableView::paintEvent(event);
    QPainter painter{viewport()};
    painter.setClipRegion(event->region());
    paintAlbumArtwork(this, &painter);
    paintDropTarget(this, &painter, drop_target_insertion_row_, drop_target_action_);
}

void QueueTableView::resizeEvent(QResizeEvent* event) {
    QTableView::resizeEvent(event);
    refitColumnsToViewport();
    updateArtworkOverlayGeometry();
}

void QueueTableView::scrollContentsBy(const int dx, const int dy) {
    QTableView::scrollContentsBy(dx, dy);
    updateArtworkOverlayGeometry();
}

} // namespace trackknife::ui
