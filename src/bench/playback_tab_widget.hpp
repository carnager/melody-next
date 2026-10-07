// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "uicommon/application_style.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QCursor>
#include <QMouseEvent>
#include <QPainterPath>
#include <QPixmap>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleOptionTab>
#include <QStylePainter>
#include <QTabBar>
#include <QTabWidget>

#include <functional>

namespace trackknife::bench {

inline QIcon playbackSpeakerIcon(const QPalette& palette) {
    // Drawn on a 32-unit grid at 32 and 64 px, so it is sharp on a HiDPI
    // screen too (ADR-0251).
    QIcon icon;
    for (const int size : {32, 64}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(size / 32.0, size / 32.0);
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
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

// A tab's close button: a small cross, brighter when pointed at. Drawn here
// rather than by the style, which elsewhere boxes it, colours it or puts it
// on the other side of the tab.
class TabCloseButton final : public QAbstractButton {
  public:
    explicit TabCloseButton(QWidget* parent) : QAbstractButton(parent) {
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::ArrowCursor);
        setToolTip(QStringLiteral("Close tab"));
        setAccessibleName(QStringLiteral("Close tab"));
        resize(sizeHint());
    }
    [[nodiscard]] QSize sizeHint() const override { return {16, 16}; }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(
            QPen(palette().color(underMouse() ? QPalette::Text : QPalette::PlaceholderText), 1.8,
                 Qt::SolidLine, Qt::RoundCap));
        const auto centre = QRectF(rect()).center();
        constexpr qreal arm = 3.0;
        painter.drawLine(centre + QPointF(-arm, -arm), centre + QPointF(arm, arm));
        painter.drawLine(centre + QPointF(-arm, arm), centre + QPointF(arm, -arm));
    }
    void enterEvent(QEnterEvent* event) override {
        QAbstractButton::enterEvent(event);
        update();
    }
    void leaveEvent(QEvent* event) override {
        QAbstractButton::leaveEvent(event);
        update();
    }
};

// Tabs as in the mockup: the current one is filled with the list's own
// ground so it reads as the top of the list below it; the others are plain
// text, quieter. The tab that is playing carries an accent dot, whichever
// tab is being browsed -- selection and playback stay independent, even
// under themes that override tab label colours. A tab's icon, where it has
// one, says which engine it plays on.
//
// The bar owns its geometry too: a tab is as wide as what is drawn in it, and
// its close button is the bar's own, on the right -- so it looks the same
// under every style, the macOS one included. The measures are those it had
// under the style it was designed with.
class PlaybackTabBar final : public QTabBar {
  public:
    // `closable`: each tab has a close button -- the lists'; the library
    // sources beside them have none, and their data names an engine, not
    // whether the tab is playing.
    explicit PlaybackTabBar(QWidget* parent = nullptr, const bool closable = true)
        : QTabBar(parent), closable_(closable) {
        // On the style the application uses: a proxy made without one would
        // wrap the desktop's default instead.
        auto* placement = new ButtonPlacement(ui::createApplicationStyle());
        placement->setParent(this);
        setStyle(placement);
    }

  protected:
    // Room either side of what a tab shows, and for its close button --
    // kept when a pinned tab hides it, so pinning does not resize the tab.
    static constexpr int lead = 12;
    static constexpr int trail = 10;
    static constexpr int close_room = 13;
    // Room around a tab's content, which is centred in it.
    static constexpr int slack = 12;
    static constexpr int dot_width = 11;
    static constexpr int icon_gap = 5;
    // The close button's centre, from the tab's right edge.
    static constexpr int close_inset = 14;
    // Above and below the text together.
    static constexpr int height_padding = 16;

    // Whatever the style, a tab's button goes where this bar says.
    class ButtonPlacement final : public QProxyStyle {
      public:
        using QProxyStyle::QProxyStyle;
        [[nodiscard]] QRect subElementRect(SubElement element, const QStyleOption* option,
                                           const QWidget* widget) const override {
            const auto* tab = qstyleoption_cast<const QStyleOptionTab*>(option);
            if (tab == nullptr ||
                (element != SE_TabBarTabRightButton && element != SE_TabBarTabLeftButton)) {
                return QProxyStyle::subElementRect(element, option, widget);
            }
            if (element == SE_TabBarTabLeftButton) {
                return {};
            }
            const auto size = tab->rightButtonSize;
            const auto& rect = tab->rect;
            return {QPoint(rect.x() + rect.width() - close_inset - size.width() / 2,
                           rect.y() + (rect.height() - size.height()) / 2),
                    size};
        }
        [[nodiscard]] int styleHint(StyleHint hint, const QStyleOption* option,
                                    const QWidget* widget,
                                    QStyleHintReturn* returned) const override {
            if (hint == SH_TabBar_CloseButtonPosition) {
                return QTabBar::RightSide;
            }
            return QProxyStyle::styleHint(hint, option, widget, returned);
        }
    };

    // A list tab marked as the one playing (its data true).
    [[nodiscard]] bool playing(int index) const {
        return closable_ && tabData(index).typeId() == QMetaType::Bool && tabData(index).toBool();
    }
    [[nodiscard]] int contentWidth(int index) const {
        int width = fontMetrics().horizontalAdvance(tabText(index));
        if (playing(index)) {
            width += dot_width;
        }
        if (const auto icon = tabIcon(index); !icon.isNull()) {
            width += icon.actualSize(QSize{14, 14}).width() + icon_gap;
        }
        return width;
    }
    [[nodiscard]] QSize tabSizeHint(int index) const override {
        return {lead + contentWidth(index) + slack + (closable_ ? close_room : 0) + trail,
                fontMetrics().height() + height_padding};
    }
    void tabInserted(int index) override {
        QTabBar::tabInserted(index);
        if (!closable_) {
            return;
        }
        auto* close = new TabCloseButton(this);
        connect(close, &QAbstractButton::clicked, this, [this, close] {
            for (int each = 0; each < count(); ++each) {
                if (tabButton(each, QTabBar::RightSide) == close) {
                    emit tabCloseRequested(each);
                    return;
                }
            }
        });
        setTabButton(index, QTabBar::RightSide, close);
    }
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
            const auto rect =
                option.rect.adjusted(lead, 3, -(trail + (closable_ ? close_room : 0)), 0);
            const bool marked = playing(index);
            const auto dot = marked ? dot_width : 0;
            const auto icon_size =
                option.icon.isNull() ? QSize{} : option.icon.actualSize(QSize{14, 14});
            const auto icon_width = icon_size.isEmpty() ? 0 : icon_size.width() + icon_gap;
            // The tab's own text, cut only when the bar is too narrow for every
            // tab: the option's is cut to the room the style would give it, and
            // this is measured as tabSizeHint measures.
            const auto room = qMax(0, rect.width() - icon_width - dot);
            const auto text = fontMetrics().horizontalAdvance(tabText(index)) <= room
                                  ? tabText(index)
                                  : fontMetrics().elidedText(tabText(index), Qt::ElideRight, room);
            const auto width = fontMetrics().horizontalAdvance(text) + icon_width + dot;
            auto x = rect.left() + qMax(0, (rect.width() - width) / 2);
            if (marked) {
                painter.save();
                painter.setPen(Qt::NoPen);
                painter.setBrush(palette().color(QPalette::Highlight));
                painter.drawEllipse(QPointF(x + 3.5, rect.center().y() + 1), 3.5, 3.5);
                painter.restore();
                x += dot;
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

  private:
    bool closable_{true};
};

class PlaybackTabWidget final : public QTabWidget {
  public:
    explicit PlaybackTabWidget(QWidget* parent = nullptr) : QTabWidget(parent) {
        setTabBar(new PlaybackTabBar(this));
        // The bar's own close buttons ask it; QTabWidget passes that on only
        // while it is set closable -- which would give each tab the style's
        // button as well -- so the request is passed on here.
        connect(tabBar(), &QTabBar::tabCloseRequested, this, &QTabWidget::tabCloseRequested);
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
