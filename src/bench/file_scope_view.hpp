// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QApplication>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QStyledItemDelegate>
#include <QTableView>
#include <algorithm>

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

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyleOptionViewItem cell{option};
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
    explicit FileScopeView(QWidget* parent) : QTableView(parent) {
        setItemDelegate(new FileScopeDelegate(this));
        verticalHeader()->setDefaultSectionSize(std::max(32, fontMetrics().height() + 12));
        setToolTip(QStringLiteral(
            "Checked files receive new edits. Click a checkbox or press Space to toggle a file. "
            "Click a filename to select it; use Shift/Ctrl for multiple files. "
            "Apply still saves all staged edits."));
    }

  protected:
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
};

} // namespace trackknife::bench
