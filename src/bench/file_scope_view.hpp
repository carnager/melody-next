// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "bench/trackknife_style.hpp"

#include <QApplication>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QTableView>
#include <algorithm>
#include <map>
#include <optional>
#include <vector>

namespace trackknife::bench {

// Selection remains the single editing scope; checkmarks replace its solid fill.
class FileScopeDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QString displayText(const QVariant& value, const QLocale& locale) const override {
        const auto prefix = property("relative-prefix").toString();
        const auto text = value.toString();
        return !prefix.isEmpty() && text.startsWith(prefix)
                   ? text.mid(prefix.size())
                   : QStyledItemDelegate::displayText(value, locale);
    }

    // ADR-0332: the album a row begins, drawn above it; none for one album.
    std::map<int, QString> headers;
    int header_height{0};

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyleOptionViewItem cell{option};
        if (const auto header = headers.find(index.row()); header != headers.end()) {
            auto band = option.rect;
            band.setHeight(header_height);
            cell.rect.setTop(option.rect.top() + header_height);
            painter->save();
            auto font = option.font;
            font.setBold(true);
            painter->setFont(font);
            painter->setPen(TrackknifeStyle::dim(option.palette));
            const auto text = band.adjusted(10, header_height / 3, -8, 0);
            painter->drawText(
                text, Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics{font}.elidedText(header->second, Qt::ElideRight, text.width()));
            painter->restore();
        }
        initStyleOption(&cell, index);
        cell.features |= QStyleOptionViewItem::HasCheckIndicator;
        cell.checkState =
            option.state.testFlag(QStyle::State_Selected) ? Qt::Checked : Qt::Unchecked;
        cell.state &= ~QStyle::State_Selected;
        cell.rect.adjust(8, 2, -8, -2);
        const auto* widget = option.widget;
        const auto* style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &cell, painter, widget);
    }
};

class FileScopeView final : public QTableView {
  public:
    // One album's files, as rows.
    struct Album {
        QString title;
        std::vector<int> rows;
    };

    explicit FileScopeView(QWidget* parent) : QTableView(parent) {
        delegate_ = new FileScopeDelegate(this);
        setItemDelegate(delegate_);
        // An album's title is a button: the hand over it.
        setMouseTracking(true);
        verticalHeader()->setDefaultSectionSize(std::max(32, fontMetrics().height() + 12));
        setToolTip(QStringLiteral(
            "Checked files receive new edits. Click a checkbox or press Space to toggle a file. "
            "Click a filename to select it; use Shift/Ctrl for multiple files. "
            "Apply still saves all staged edits."));
    }

    // ADR-0332: with files of several albums, each album's title above its
    // first file; clicking it selects the album's files, Ctrl adds them.
    void setAlbums(std::vector<Album> albums) {
        for (const auto& [row, title] : delegate_->headers) {
            if (row < model()->rowCount()) {
                verticalHeader()->resizeSection(row, verticalHeader()->defaultSectionSize());
            }
        }
        delegate_->headers.clear();
        albums_.clear();
        album_of_header_.clear();
        delegate_->header_height = fontMetrics().height() + 12;
        if (albums.size() > 1U) {
            albums_ = std::move(albums);
            for (std::size_t at = 0U; at < albums_.size(); ++at) {
                const auto& rows = albums_[at].rows;
                if (rows.empty() || rows.front() >= model()->rowCount()) {
                    continue;
                }
                delegate_->headers[rows.front()] = albums_[at].title;
                album_of_header_[rows.front()] = at;
                verticalHeader()->resizeSection(rows.front(),
                                                verticalHeader()->defaultSectionSize() +
                                                    delegate_->header_height);
            }
        }
        viewport()->update();
    }

    [[nodiscard]] const std::map<int, QString>& albumHeaders() const { return delegate_->headers; }

    // The album whose title is at `point`, its files selected.
    bool selectAlbumAt(const QPoint& point, const bool add) {
        const auto album = albumAt(point);
        if (!album) {
            return false;
        }
        QItemSelection rows;
        for (const auto row : albums_[*album].rows) {
            rows.select(model()->index(row, 0), model()->index(row, model()->columnCount() - 1));
        }
        selectionModel()->select(
            rows, (add ? QItemSelectionModel::Select : QItemSelectionModel::ClearAndSelect) |
                      QItemSelectionModel::Rows);
        selectionModel()->setCurrentIndex(indexAt(point), QItemSelectionModel::NoUpdate);
        return true;
    }

  protected:
    void mouseMoveEvent(QMouseEvent* event) override {
        if (albumAt(event->position().toPoint())) {
            viewport()->setCursor(Qt::PointingHandCursor);
        } else {
            viewport()->unsetCursor();
        }
        QTableView::mouseMoveEvent(event);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton &&
            selectAlbumAt(event->position().toPoint(),
                          event->modifiers().testFlag(Qt::ControlModifier))) {
            event->accept();
            return;
        }
        QTableView::mousePressEvent(event);
    }

    QItemSelectionModel::SelectionFlags
    selectionCommand(const QModelIndex& index, const QEvent* event = nullptr) const override {
        if (index.isValid() && event && event->type() == QEvent::MouseButtonPress) {
            const auto* mouse = static_cast<const QMouseEvent*>(event);
            const auto indicator_width = style()->pixelMetric(QStyle::PM_IndicatorWidth);
            if (mouse->button() == Qt::LeftButton && mouse->modifiers() == Qt::NoModifier &&
                mouse->position().x() < visualRect(index).left() + 8 + indicator_width + 8)
                return QItemSelectionModel::Toggle | QItemSelectionModel::Rows;
        }
        return QTableView::selectionCommand(index, event);
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Space && event->modifiers() == Qt::NoModifier &&
            currentIndex().isValid()) {
            selectionModel()->select(currentIndex(),
                                     QItemSelectionModel::Toggle | QItemSelectionModel::Rows);
            event->accept();
            return;
        }
        QTableView::keyPressEvent(event);
    }

  private:
    // The album whose title is at `point`.
    [[nodiscard]] std::optional<std::size_t> albumAt(const QPoint& point) const {
        const auto index = indexAt(point);
        const auto header = album_of_header_.find(index.row());
        if (!index.isValid() || header == album_of_header_.end() ||
            point.y() >= visualRect(index).top() + delegate_->header_height) {
            return std::nullopt;
        }
        return header->second;
    }

    FileScopeDelegate* delegate_{nullptr};
    std::vector<Album> albums_;
    std::map<int, std::size_t> album_of_header_;
};

} // namespace trackknife::bench
