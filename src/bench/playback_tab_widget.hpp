// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QPainterPath>
#include <QPixmap>
#include <QStyleOptionTab>
#include <QStylePainter>
#include <QTabBar>
#include <QTabWidget>

namespace trackknife::bench {

inline QIcon playbackSpeakerIcon(const QPalette& palette) {
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto color = palette.color(QPalette::Highlight);
    painter.setPen(QPen(color, 2.5, Qt::SolidLine, Qt::RoundCap));
    painter.setBrush(color);
    QPainterPath speaker;
    speaker.moveTo(5, 12);
    speaker.lineTo(11, 12);
    speaker.lineTo(18, 6);
    speaker.lineTo(18, 26);
    speaker.lineTo(11, 20);
    speaker.lineTo(5, 20);
    speaker.closeSubpath();
    painter.drawPath(speaker);
    painter.setBrush(Qt::NoBrush);
    painter.drawArc(QRectF(16, 8, 10, 16), -60 * 16, 120 * 16);
    painter.drawArc(QRectF(15, 3, 16, 26), -60 * 16, 120 * 16);
    return QIcon(pixmap);
}

// Draw selection independently of playback identity, even under themes that
// override tab label colors. The icon identifies playback; the underline browsing.
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
            if (index == currentIndex()) {
                painter.fillRect(option.rect.adjusted(1, 1, -1, -1),
                                 palette().color(QPalette::Window).lighter(115));
                painter.fillRect(QRect(option.rect.left() + 2, option.rect.bottom() - 2,
                                       option.rect.width() - 4, 3),
                                 palette().color(QPalette::Highlight));
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
            painter.setPen(palette().color(QPalette::WindowText));
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
