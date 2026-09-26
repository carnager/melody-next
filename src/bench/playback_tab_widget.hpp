// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QCursor>
#include <QMouseEvent>
#include <QPainterPath>
#include <QPixmap>
#include <QStyleOptionTab>
#include <QStylePainter>
#include <QTabBar>
#include <QTabWidget>

#include <functional>

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

// Tabs as in the mockup: the current one is filled with the list's own
// ground so it reads as the top of the list below it; the others are plain
// text, quieter. The tab that is playing carries an accent dot, whichever
// tab is being browsed -- selection and playback stay independent, even
// under themes that override tab label colours. A tab's icon, where it has
// one, says which engine it plays on.
class PlaybackTabBar final : public QTabBar {
  public:
    using QTabBar::QTabBar;

  protected:
    void paintEvent(QPaintEvent*) override {
        QStylePainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto hovered = tabAt(mapFromGlobal(QCursor::pos()));
        const auto draw = [this, &painter, hovered](int index) {
            QStyleOptionTab option;
            initStyleOption(&option, index);
            const bool current = index == currentIndex();
            const auto tab = option.rect.adjusted(1, 3, -1, 0);
            if (current || index == hovered) {
                QPainterPath shape;
                shape.addRoundedRect(QRectF(tab).adjusted(0, 0, 0, 6), 5, 5);
                painter.save();
                painter.setClipRect(tab);
                auto fill = palette().color(QPalette::Base);
                if (!current) {
                    fill.setAlpha(110);
                }
                painter.fillPath(shape, fill);
                painter.restore();
            }
            auto rect = option.rect.adjusted(12, 3, -10, 0);
            if (!option.leftButtonSize.isEmpty())
                rect.setLeft(rect.left() + option.leftButtonSize.width() + 4);
            if (!option.rightButtonSize.isEmpty())
                rect.setRight(rect.right() - option.rightButtonSize.width() - 4);
            const bool playing = tabData(index).toBool();
            const auto dot_width = playing ? 11 : 0;
            const auto icon_size =
                option.icon.isNull() ? QSize{} : option.icon.actualSize(QSize{14, 14});
            const auto icon_width = icon_size.isEmpty() ? 0 : icon_size.width() + 5;
            const auto text = fontMetrics().elidedText(
                option.text, Qt::ElideRight, qMax(0, rect.width() - icon_width - dot_width));
            const auto width = fontMetrics().horizontalAdvance(text) + icon_width + dot_width;
            auto x = rect.left() + qMax(0, (rect.width() - width) / 2);
            if (playing) {
                painter.save();
                painter.setPen(Qt::NoPen);
                painter.setBrush(palette().color(QPalette::Highlight));
                painter.drawEllipse(QPointF(x + 3.5, rect.center().y() + 1), 3.5, 3.5);
                painter.restore();
                x += dot_width;
            }
            if (!icon_size.isEmpty()) {
                option.icon.paint(
                    &painter,
                    QRect(QPoint(x, rect.center().y() - icon_size.height() / 2 + 1), icon_size),
                    Qt::AlignCenter, current ? QIcon::Normal : QIcon::Disabled);
                x += icon_width;
            }
            painter.save();
            painter.setPen(palette().color(current ? QPalette::Text : QPalette::PlaceholderText));
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
    void enterEvent(QEnterEvent* event) override {
        QTabBar::enterEvent(event);
        update();
    }
    void leaveEvent(QEvent* event) override {
        QTabBar::leaveEvent(event);
        update();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        QTabBar::mouseMoveEvent(event);
        update();
    }
};

class PlaybackTabWidget final : public QTabWidget {
  public:
    explicit PlaybackTabWidget(QWidget* parent = nullptr) : QTabWidget(parent) {
        setTabBar(new PlaybackTabBar(this));
    }
    // Told of every tab added or removed.
    std::function<void()> tabs_changed;

  protected:
    void tabInserted(int index) override {
        QTabWidget::tabInserted(index);
        if (tabs_changed) {
            tabs_changed();
        }
    }
    void tabRemoved(int index) override {
        QTabWidget::tabRemoved(index);
        if (tabs_changed) {
            tabs_changed();
        }
    }
};
} // namespace trackknife::bench
