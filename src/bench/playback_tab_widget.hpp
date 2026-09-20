// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QStyleOptionTab>
#include <QStylePainter>
#include <QTabBar>
#include <QTabWidget>

namespace trackknife::bench {

// Some native themes ignore QTabBar::setTabTextColor. Keep their tab shapes,
// but draw active labels ourselves so playback identity remains visible.
class PlaybackTabBar final : public QTabBar {
  public:
    using QTabBar::QTabBar;

  protected:
    void paintEvent(QPaintEvent*) override {
        QStylePainter painter(this);
        const auto draw = [this, &painter](int index) {
            QStyleOptionTab option;
            initStyleOption(&option, index);
            painter.drawControl(QStyle::CE_TabBarTabShape, option);
            if (!tabData(index).toBool()) {
                painter.drawControl(QStyle::CE_TabBarTabLabel, option);
                return;
            }
            auto rect = option.rect.adjusted(10, 0, -10, 0);
            if (!option.leftButtonSize.isEmpty())
                rect.setLeft(rect.left() + option.leftButtonSize.width() + 4);
            if (!option.rightButtonSize.isEmpty())
                rect.setRight(rect.right() - option.rightButtonSize.width() - 4);
            const auto icon_size =
                option.icon.isNull() ? QSize{} : option.icon.actualSize(option.iconSize);
            const auto icon_width = icon_size.isEmpty() ? 0 : icon_size.width() + 4;
            const auto text = fontMetrics().elidedText(option.text, Qt::ElideRight,
                                                       qMax(0, rect.width() - icon_width));
            const auto width = fontMetrics().horizontalAdvance(text) + icon_width;
            auto x = rect.left() + qMax(0, (rect.width() - width) / 2);
            if (!icon_size.isEmpty()) {
                option.icon.paint(
                    &painter,
                    QRect(QPoint(x, rect.center().y() - icon_size.height() / 2), icon_size));
                x += icon_width;
            }
            painter.save();
            painter.setPen(palette().color(QPalette::Highlight));
            painter.drawText(QRect(x, rect.top(), qMax(0, rect.right() - x + 1), rect.height()),
                             Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, text);
            painter.restore();
        };
        for (int index = 0; index < count(); ++index)
            if (index != currentIndex() && isTabVisible(index))
                draw(index);
        if (currentIndex() >= 0 && isTabVisible(currentIndex()))
            draw(currentIndex());
    }
};

class PlaybackTabWidget final : public QTabWidget {
  public:
    explicit PlaybackTabWidget(QWidget* parent = nullptr) : QTabWidget(parent) {
        setTabBar(new PlaybackTabBar(this));
    }
};
} // namespace trackknife::bench
