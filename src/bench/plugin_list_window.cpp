// SPDX-License-Identifier: GPL-3.0-only

#include "bench/plugin_list_window.hpp"

#include <QClipboard>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace trackknife::bench {
namespace {

constexpr int detail_role = Qt::UserRole + 1;
constexpr int url_role = Qt::UserRole;

[[nodiscard]] QColor mixed(const QColor& from, const QColor& to, const int percent) {
    const auto mix = [percent](const int a, const int b) {
        return (a * (100 - percent) + b * percent) / 100;
    };
    return QColor::fromRgb(mix(from.red(), to.red()), mix(from.green(), to.green()),
                           mix(from.blue(), to.blue()));
}

[[nodiscard]] QFont smaller(const QFont& font) {
    auto small = font;
    small.setPointSizeF(std::max(7.0, font.pointSizeF() - 1.0));
    return small;
}

// A row as Up Next draws one: the text, its detail smaller beneath, and an
// arrow at the end of one with a link, which opens it.
class PluginListDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        const QFontMetrics metrics{option.font};
        const QFontMetrics small{smaller(option.font)};
        const bool detail = !index.data(detail_role).toString().isEmpty();
        size.setHeight(metrics.height() + (detail ? small.height() + 2 : 0) + 14);
        return size;
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const auto& palette = option.palette;
        const auto base = palette.color(QPalette::Base);
        const auto text_color = palette.color(QPalette::Text);
        painter->save();
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        painter->fillRect(option.rect, selected
                                           ? mixed(base, palette.color(QPalette::Highlight), 32)
                                       : hovered ? mixed(base, text_color, 6)
                                                 : base);
        if (index.row() > 0) {
            painter->setPen(mixed(base, text_color, 10));
            painter->drawLine(option.rect.left() + 10, option.rect.top(), option.rect.right() - 10,
                              option.rect.top());
        }

        const auto area = option.rect.adjusted(12, 7, -12, -7);
        const bool link = !index.data(url_role).toString().isEmpty();
        const auto arrow = link ? arrowRect(option.rect) : QRect{};
        const auto width = std::max(0, (link ? arrow.left() - 8 : area.right()) - area.left());
        const QFontMetrics metrics{option.font};
        const auto small_font = smaller(option.font);
        const QFontMetrics small{small_font};
        painter->setFont(option.font);
        painter->setPen(text_color);
        painter->drawText(QRect{area.left(), area.top(), width, metrics.height()},
                          Qt::AlignLeft | Qt::AlignVCenter,
                          metrics.elidedText(index.data().toString(), Qt::ElideRight, width));
        if (const auto detail = index.data(detail_role).toString(); !detail.isEmpty()) {
            painter->setFont(small_font);
            painter->setPen(palette.color(QPalette::PlaceholderText));
            painter->drawText(
                QRect{area.left(), area.top() + metrics.height() + 2, width, small.height()},
                Qt::AlignLeft | Qt::AlignVCenter, small.elidedText(detail, Qt::ElideRight, width));
        }
        if (link) {
            painter->setFont(option.font);
            painter->setPen(hovered || selected ? palette.color(QPalette::Highlight)
                                                : palette.color(QPalette::PlaceholderText));
            painter->drawText(arrow, Qt::AlignCenter, QStringLiteral("\u2197"));
        }
        painter->restore();
    }

    bool editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option,
                     const QModelIndex& index) override {
        if (event->type() == QEvent::MouseButtonRelease) {
            const auto* click = static_cast<QMouseEvent*>(event);
            const auto url = index.data(url_role).toString();
            if (click->button() == Qt::LeftButton && !url.isEmpty() &&
                arrowRect(option.rect).contains(click->position().toPoint())) {
                QDesktopServices::openUrl(QUrl{url});
                return true;
            }
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }

  private:
    [[nodiscard]] static QRect arrowRect(const QRect& row) {
        constexpr int extent = 28;
        return QRect{row.right() - 8 - extent, row.center().y() - extent / 2, extent, extent};
    }
};

} // namespace

PluginListWindow::PluginListWindow(const QString& plugin, const PluginList& list, QWidget* parent)
    : QDialog(parent), list_(list) {
    setObjectName(QStringLiteral("bench-plugin-list"));
    setWindowTitle(plugin);
    setModal(false);
    resize(520, 420);
    auto* layout = new QVBoxLayout(this);
    auto* heading = new QLabel(list.title.isEmpty() ? plugin : list.title, this);
    heading->setObjectName(QStringLiteral("bench-plugin-list-title"));
    auto font = heading->font();
    font.setWeight(QFont::DemiBold);
    font.setPointSizeF(font.pointSizeF() * 1.15);
    heading->setFont(font);
    heading->setWordWrap(true);
    layout->addWidget(heading);

    rows_ = new QListWidget(this);
    rows_->setObjectName(QStringLiteral("bench-plugin-list-rows"));
    rows_->setItemDelegate(new PluginListDelegate(rows_));
    rows_->setMouseTracking(true);
    rows_->setUniformItemSizes(false);
    rows_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    for (const auto& item : list.items) {
        auto* row = new QListWidgetItem(item.text, rows_);
        row->setData(detail_role, item.detail);
        if (!item.url.isEmpty()) {
            row->setToolTip(item.url);
            row->setData(url_role, item.url);
        }
    }
    if (list.items.empty()) {
        rows_->addItem(QStringLiteral("Nothing"));
        rows_->item(0)->setFlags(Qt::NoItemFlags);
    }
    connect(rows_, &QListWidget::itemActivated, this, [](QListWidgetItem* row) {
        if (const auto url = row->data(url_role).toString(); !url.isEmpty()) {
            QDesktopServices::openUrl(QUrl{url});
        }
    });
    layout->addWidget(rows_, 1);
    if (std::ranges::any_of(list.items, [](const auto& item) { return !item.url.isEmpty(); })) {
        auto* hint =
            new QLabel(QStringLiteral("Double-click a row or click its arrow to open it."), this);
        hint->setForegroundRole(QPalette::PlaceholderText);
        layout->addWidget(hint);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* copy = buttons->addButton(QStringLiteral("Copy"), QDialogButtonBox::ActionRole);
    copy->setObjectName(QStringLiteral("bench-plugin-list-copy"));
    connect(copy, &QPushButton::clicked, this,
            [this] { QGuiApplication::clipboard()->setText(text()); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);
}

QString PluginListWindow::text() const {
    QStringList lines;
    if (!list_.title.isEmpty()) {
        lines << list_.title;
    }
    for (const auto& item : list_.items) {
        lines << (item.detail.isEmpty() ? item.text
                                        : QStringLiteral("%1 -- %2").arg(item.text, item.detail));
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace trackknife::bench
