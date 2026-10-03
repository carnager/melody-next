// SPDX-License-Identifier: GPL-3.0-only
#include "bench/trackknife_style.hpp"

#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QHeaderView>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QStyleFactory>
#include <QStyleOption>
#include <QTabBar>
#include <QTableView>

#include <algorithm>

namespace trackknife::bench {
namespace {

// A combo box's list item, drawn as a menu item (Fusion's own test).
[[nodiscard]] bool combo_popup_item(const QStyleOption* option, const QWidget* widget) {
    return qobject_cast<const QComboBox*>(widget) != nullptr ||
           (option->styleObject != nullptr &&
            option->styleObject->property("_q_isComboBoxPopupItem").toBool());
}
// Room for the tick before a combo box list item's text.
constexpr int combo_tick_column = 20;

// A rounded rectangle on whole pixels, its 1 px edge on the pixel centres.
void fill_rounded(QPainter* painter, const QRectF& rect, const QColor& fill, const qreal radius,
                  const QColor& edge = {}) {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    if (edge.isValid()) {
        painter->setPen(QPen{edge, 1.0});
        painter->setBrush(fill.isValid() ? QBrush{fill} : Qt::NoBrush);
        painter->drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    } else {
        painter->setPen(Qt::NoPen);
        painter->setBrush(fill);
        painter->drawRoundedRect(rect, radius, radius);
    }
    painter->restore();
}

[[nodiscard]] bool keyboard_focus(const QStyleOption* option) {
    return option->state.testFlag(QStyle::State_HasFocus) &&
           option->state.testFlag(QStyle::State_KeyboardFocusChange);
}

// The accent: a checked button, or the one a window names as its primary
// (QPushButton::isDefault) -- not every button that is the default for a
// moment because it has focus in a dialog (autoDefault): the accent is on
// the accepting button alone.
[[nodiscard]] bool accented(const QStyleOptionButton* button, const QWidget* widget) {
    if (button == nullptr) {
        return false;
    }
    if (button->state.testFlag(QStyle::State_On)) {
        return true;
    }
    if (const auto* push = qobject_cast<const QPushButton*>(widget)) {
        return push->isDefault();
    }
    return button->features.testFlag(QStyleOptionButton::DefaultButton);
}

// The palette with highlighted text as ordinary text: a chosen row is a
// quiet tint (Theme.selection, painted as the row's panel), not the accent,
// so its text stays as it is. The accent itself stays, for what is drawn in
// the row: a check box is still the accent's.
[[nodiscard]] QPalette quiet_selection(QPalette palette) {
    for (const auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        palette.setColor(group, QPalette::HighlightedText, palette.color(group, QPalette::Text));
    }
    return palette;
}

void draw_chevron(QPainter* painter, const QRectF& box, const QColor& color, const bool down) {
    const qreal size = std::min(box.width(), box.height()) * 0.5;
    const QPointF centre = box.center();
    QPainterPath path;
    if (down) {
        path.moveTo(centre.x() - size / 2, centre.y() - size / 4);
        path.lineTo(centre.x(), centre.y() + size / 4);
        path.lineTo(centre.x() + size / 2, centre.y() - size / 4);
    } else {
        path.moveTo(centre.x() - size / 2, centre.y() + size / 4);
        path.lineTo(centre.x(), centre.y() - size / 4);
        path.lineTo(centre.x() + size / 2, centre.y() + size / 4);
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(QPen{color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin});
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(path);
    painter->restore();
}

} // namespace

TrackknifeStyle::TrackknifeStyle() : QProxyStyle{QStyleFactory::create(QStringLiteral("Fusion"))} {
    setObjectName(QStringLiteral("trackknife"));
}

QColor TrackknifeStyle::mix(const QColor& a, const QColor& b, const qreal t) {
    const auto toward = [t](const float from, const float to) {
        return static_cast<float>(static_cast<qreal>(from) +
                                  (static_cast<qreal>(to) - static_cast<qreal>(from)) * t);
    };
    return QColor::fromRgbF(toward(a.redF(), b.redF()), toward(a.greenF(), b.greenF()),
                            toward(a.blueF(), b.blueF()), toward(a.alphaF(), b.alphaF()));
}

QColor TrackknifeStyle::hairline(const QPalette& p) {
    return mix(p.color(QPalette::Window), p.color(QPalette::WindowText), 0.13);
}
QColor TrackknifeStyle::border(const QPalette& p) {
    return mix(p.color(QPalette::Window), p.color(QPalette::WindowText), 0.22);
}
QColor TrackknifeStyle::raised(const QPalette& p) {
    return mix(p.color(QPalette::Window), p.color(QPalette::WindowText), 0.07);
}
QColor TrackknifeStyle::hovered(const QPalette& p) {
    return mix(p.color(QPalette::Window), p.color(QPalette::WindowText), 0.11);
}
QColor TrackknifeStyle::pressed(const QPalette& p) {
    return mix(p.color(QPalette::Window), p.color(QPalette::WindowText), 0.17);
}
QColor TrackknifeStyle::sunken(const QPalette& p) {
    return mix(p.color(QPalette::Window), p.color(QPalette::WindowText), 0.035);
}
QColor TrackknifeStyle::rowHover(const QPalette& p) {
    return mix(p.color(QPalette::Base), p.color(QPalette::Text), 0.05);
}
QColor TrackknifeStyle::selection(const QPalette& p) {
    return mix(p.color(QPalette::Active, QPalette::Base),
               p.color(QPalette::Active, QPalette::Highlight), 0.35);
}
QColor TrackknifeStyle::dim(const QPalette& p) {
    return mix(p.color(QPalette::WindowText), p.color(QPalette::Window), 0.45);
}

void TrackknifeStyle::polish(QWidget* widget) {
    QProxyStyle::polish(widget);
    widget->setAttribute(Qt::WA_Hover, true);
    // Columns read from the left; a model's own alignment
    // (a right-aligned length) still wins.
    if (auto* header = qobject_cast<QHeaderView*>(widget);
        header != nullptr && header->orientation() == Qt::Horizontal) {
        header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    }
    // No grid: rows are told apart by their spacing, as in Quick.
    if (auto* table = qobject_cast<QTableView*>(widget)) {
        table->setShowGrid(false);
    }
}

void TrackknifeStyle::unpolish(QWidget* widget) { QProxyStyle::unpolish(widget); }

void TrackknifeStyle::polish(QPalette& palette) { QProxyStyle::polish(palette); }

int TrackknifeStyle::pixelMetric(const PixelMetric metric, const QStyleOption* option,
                                 const QWidget* widget) const {
    switch (metric) {
    case PM_ButtonMargin:
        return 14;
    case PM_DefaultFrameWidth:
        return 1;
    case PM_IndicatorWidth:
    case PM_IndicatorHeight:
    case PM_ExclusiveIndicatorWidth:
    case PM_ExclusiveIndicatorHeight:
        return 16;
    case PM_CheckBoxLabelSpacing:
    case PM_RadioButtonLabelSpacing:
        return gap;
    case PM_ScrollBarExtent:
        return 12;
    case PM_ScrollBarSliderMin:
        return 28;
    case PM_SliderLength:
    case PM_SliderThickness:
    case PM_SliderControlThickness:
        return 14;
    case PM_TabBarTabHSpace:
        return 24;
    case PM_TabBarTabVSpace:
        return 12;
    case PM_TabBarBaseOverlap:
        return 0;
    case PM_SplitterWidth:
        return 5;
    case PM_ToolBarItemSpacing:
    case PM_LayoutHorizontalSpacing:
    case PM_LayoutVerticalSpacing:
        return gap;
    case PM_MenuHMargin:
    case PM_MenuVMargin:
        return gap_small;
    case PM_MenuPanelWidth:
        return 1;
    case PM_ButtonShiftHorizontal:
    case PM_ButtonShiftVertical:
        return 0;
    default:
        return QProxyStyle::pixelMetric(metric, option, widget);
    }
}

int TrackknifeStyle::styleHint(const StyleHint hint, const QStyleOption* option,
                               const QWidget* widget, QStyleHintReturn* returned) const {
    switch (hint) {
    case SH_DialogButtonBox_ButtonsHaveIcons:
        return 0;
    case SH_ItemView_ShowDecorationSelected:
        return 1;
    case SH_ScrollBar_Transient:
        return 0;
    case SH_ScrollBar_ContextMenu:
        return 1;
    default:
        return QProxyStyle::styleHint(hint, option, widget, returned);
    }
}

QSize TrackknifeStyle::sizeFromContents(const ContentsType type, const QStyleOption* option,
                                        const QSize& contents, const QWidget* widget) const {
    switch (type) {
    case CT_PushButton: {
        auto size = QProxyStyle::sizeFromContents(type, option, contents, widget);
        const auto* button = qstyleoption_cast<const QStyleOptionButton*>(option);
        const bool text = button != nullptr && !button->text.isEmpty();
        size.setWidth(std::max(
            contents.width() + 2 * 14 +
                (button != nullptr && button->features.testFlag(QStyleOptionButton::HasMenu) ? 16
                                                                                             : 0),
            text ? 72 : control_height));
        size.setHeight(std::max(control_height, contents.height() + 12));
        return size;
    }
    case CT_ToolButton: {
        auto size = QProxyStyle::sizeFromContents(type, option, contents, widget);
        size.setHeight(std::max(size.height(), control_height));
        size.setWidth(std::max(size.width(), control_height));
        return size;
    }
    case CT_LineEdit:
    case CT_SpinBox: {
        auto size = QProxyStyle::sizeFromContents(type, option, contents, widget);
        size.setHeight(std::max(size.height(), control_height));
        return size;
    }
    case CT_ComboBox: {
        auto size = QProxyStyle::sizeFromContents(type, option, contents, widget);
        size.setHeight(std::max(size.height(), control_height));
        size.setWidth(size.width() + gap);
        return size;
    }
    case CT_TabBarTab: {
        auto size = QProxyStyle::sizeFromContents(type, option, contents, widget);
        size.setHeight(std::max(size.height(), 32));
        return size;
    }
    case CT_CheckBox:
    case CT_RadioButton: {
        auto size = QProxyStyle::sizeFromContents(type, option, contents, widget);
        size.setHeight(std::max(size.height(), 24));
        return size;
    }
    case CT_MenuItem: {
        auto size = QProxyStyle::sizeFromContents(type, option, contents, widget);
        const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
        if (item != nullptr && item->menuItemType != QStyleOptionMenuItem::Separator) {
            size.setHeight(std::max(size.height(), 26));
            if (combo_popup_item(option, widget) &&
                item->checkType != QStyleOptionMenuItem::NotCheckable) {
                size.rwidth() += combo_tick_column;
            }
        }
        return size;
    }
    default:
        return QProxyStyle::sizeFromContents(type, option, contents, widget);
    }
}

QRect TrackknifeStyle::subControlRect(const ComplexControl control,
                                      const QStyleOptionComplex* option, const SubControl sub,
                                      const QWidget* widget) const {
    if (control == CC_ScrollBar) {
        const auto* bar = qstyleoption_cast<const QStyleOptionSlider*>(option);
        if (bar == nullptr) {
            return QProxyStyle::subControlRect(control, option, sub, widget);
        }
        // No arrows: the whole length is the groove, the handle in it.
        const QRect whole = bar->rect;
        const bool horizontal = bar->orientation == Qt::Horizontal;
        const int length = horizontal ? whole.width() : whole.height();
        const auto range = static_cast<qint64>(bar->maximum) - bar->minimum;
        int handle = length;
        if (range > 0) {
            handle = static_cast<int>(static_cast<qint64>(bar->pageStep) * length /
                                      (range + bar->pageStep));
            handle = std::clamp(
                handle, std::min(pixelMetric(PM_ScrollBarSliderMin, bar, widget), length), length);
        }
        const int start = sliderPositionFromValue(bar->minimum, bar->maximum, bar->sliderPosition,
                                                  length - handle, bar->upsideDown);
        const auto along = [&](const int from, const int size) {
            return horizontal ? QRect{whole.x() + from, whole.y(), size, whole.height()}
                              : QRect{whole.x(), whole.y() + from, whole.width(), size};
        };
        switch (sub) {
        case SC_ScrollBarAddLine:
        case SC_ScrollBarSubLine:
            return {};
        case SC_ScrollBarGroove:
            return whole;
        case SC_ScrollBarSlider:
            return along(start, handle);
        case SC_ScrollBarSubPage:
            return along(0, start);
        case SC_ScrollBarAddPage:
            return along(start + handle, length - start - handle);
        default:
            return {};
        }
    }
    return QProxyStyle::subControlRect(control, option, sub, widget);
}

void TrackknifeStyle::drawPrimitive(const PrimitiveElement element, const QStyleOption* option,
                                    QPainter* painter, const QWidget* widget) const {
    const auto& palette = option->palette;
    const QRectF rect = option->rect;
    switch (element) {
    case PE_PanelButtonCommand: {
        const auto* button = qstyleoption_cast<const QStyleOptionButton*>(option);
        // Flat, as icon buttons are: a fill only under the pointer.
        if (button != nullptr && button->features.testFlag(QStyleOptionButton::Flat)) {
            const bool flat_down = option->state.testFlag(State_Sunken);
            const bool flat_hover =
                option->state.testFlag(State_MouseOver) && option->state.testFlag(State_Enabled);
            if (flat_down || flat_hover) {
                fill_rounded(painter, rect, flat_down ? pressed(palette) : hovered(palette),
                             radius);
            }
            return;
        }
        // The accent stays on a disabled default button, faded.
        const bool accent =
            button != nullptr ? accented(button, widget) : option->state.testFlag(State_On);
        const bool down = option->state.testFlag(State_Sunken);
        const bool hover =
            option->state.testFlag(State_MouseOver) && option->state.testFlag(State_Enabled);
        auto highlight = palette.color(QPalette::Active, QPalette::Highlight);
        if (!option->state.testFlag(State_Enabled)) {
            highlight.setAlphaF(0.5F);
        }
        const auto fill = accent  ? (down    ? highlight.darker(115)
                                     : hover ? highlight.lighter(108)
                                             : highlight)
                          : down  ? pressed(palette)
                          : hover ? hovered(palette)
                                  : raised(palette);
        fill_rounded(painter, rect, fill, radius, keyboard_focus(option) ? highlight : QColor{});
        return;
    }
    case PE_PanelButtonBevel:
    case PE_PanelButtonTool: {
        const bool down = option->state.testFlag(State_Sunken);
        const bool on = option->state.testFlag(State_On);
        const bool hover =
            option->state.testFlag(State_MouseOver) && option->state.testFlag(State_Enabled);
        // A tool button that is not auto-raised stands as a button does.
        if (element == PE_PanelButtonTool && !option->state.testFlag(State_AutoRaise) && !on) {
            fill_rounded(painter, rect,
                         down    ? pressed(palette)
                         : hover ? hovered(palette)
                                 : raised(palette),
                         radius);
            return;
        }
        if (!down && !on && !hover) {
            return;
        }
        auto checked = palette.color(QPalette::Highlight);
        checked.setAlphaF(0.18F);
        fill_rounded(painter, rect,
                     down ? pressed(palette)
                     : on ? checked
                          : hovered(palette),
                     radius);
        return;
    }
    case PE_FrameButtonTool:
    case PE_FrameDefaultButton:
    case PE_FrameFocusRect:
        return;
    case PE_PanelLineEdit: {
        const auto* frame = qstyleoption_cast<const QStyleOptionFrame*>(option);
        if (frame != nullptr && frame->lineWidth <= 0) {
            // Inside a spin box or combo box, which draws the frame. Any
            // other frameless edit -- a table cell's editor -- still needs
            // its ground, or the cell's text shows through what is typed.
            const auto* parent = widget != nullptr ? widget->parentWidget() : nullptr;
            if (qobject_cast<const QAbstractSpinBox*>(parent) == nullptr &&
                qobject_cast<const QComboBox*>(parent) == nullptr) {
                painter->fillRect(rect, palette.color(QPalette::Base));
            }
            return;
        }
        const bool read_only = option->state.testFlag(State_ReadOnly);
        const bool focus = option->state.testFlag(State_HasFocus);
        fill_rounded(painter, rect, read_only ? sunken(palette) : palette.color(QPalette::Base),
                     radius, focus ? palette.color(QPalette::Highlight) : border(palette));
        return;
    }
    case PE_FrameLineEdit:
        return;
    case PE_IndicatorCheckBox: {
        const bool on = option->state.testFlag(State_On);
        const bool partial = option->state.testFlag(State_NoChange);
        const QRectF box = QRectF{rect.x(), rect.y() + (rect.height() - 16) / 2, 16, 16};
        const auto highlight = palette.color(QPalette::Active, QPalette::Highlight);
        if (on || partial) {
            fill_rounded(painter, box, highlight, 3);
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setPen(QPen{palette.color(QPalette::Active, QPalette::HighlightedText), 1.8,
                                 Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin});
            if (partial) {
                painter->drawLine(QPointF{box.left() + 4.5, box.center().y()},
                                  QPointF{box.right() - 4.5, box.center().y()});
            } else {
                QPainterPath tick;
                tick.moveTo(box.left() + 4, box.center().y() + 0.5);
                tick.lineTo(box.left() + 7, box.bottom() - 4.5);
                tick.lineTo(box.right() - 3.5, box.top() + 5);
                painter->drawPath(tick);
            }
            painter->restore();
        } else {
            const bool hover = option->state.testFlag(State_MouseOver);
            fill_rounded(painter, box, palette.color(QPalette::Base), 3,
                         hover || keyboard_focus(option) ? highlight : border(palette));
        }
        if (!option->state.testFlag(State_Enabled)) {
            fill_rounded(
                painter, box,
                [&] {
                    auto veil = palette.color(QPalette::Window);
                    veil.setAlphaF(0.5F);
                    return veil;
                }(),
                3);
        }
        return;
    }
    case PE_IndicatorRadioButton: {
        const bool on = option->state.testFlag(State_On);
        const QRectF box = QRectF{rect.x(), rect.y() + (rect.height() - 16) / 2, 16, 16};
        const auto highlight = palette.color(QPalette::Active, QPalette::Highlight);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        if (on) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(highlight);
            painter->drawEllipse(box);
            painter->setBrush(palette.color(QPalette::Active, QPalette::HighlightedText));
            painter->drawEllipse(box.center(), 3.0, 3.0);
        } else {
            painter->setPen(
                QPen{option->state.testFlag(State_MouseOver) ? highlight : border(palette), 1.0});
            painter->setBrush(palette.color(QPalette::Base));
            painter->drawEllipse(box.adjusted(0.5, 0.5, -0.5, -0.5));
        }
        painter->restore();
        return;
    }
    case PE_IndicatorArrowDown:
    case PE_IndicatorArrowUp:
    case PE_IndicatorSpinDown:
    case PE_IndicatorSpinUp:
    case PE_IndicatorSpinMinus:
    case PE_IndicatorSpinPlus: {
        const bool down = element == PE_IndicatorArrowDown || element == PE_IndicatorSpinDown ||
                          element == PE_IndicatorSpinMinus;
        auto colour = palette.color(QPalette::ButtonText);
        if (!option->state.testFlag(State_Enabled)) {
            colour.setAlphaF(0.5F);
        }
        draw_chevron(painter, QRectF{rect}.adjusted(1, 1, -1, -1), colour, down);
        return;
    }
    case PE_FrameTabWidget:
        return;
    case PE_FrameTabBarBase: {
        painter->fillRect(
            QRect{option->rect.left(), option->rect.bottom(), option->rect.width(), 1},
            hairline(palette));
        return;
    }
    case PE_Frame:
    case PE_FrameWindow: {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen{hairline(palette), 1.0});
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
        painter->restore();
        return;
    }
    case PE_FrameGroupBox: {
        painter->fillRect(QRect{option->rect.left(), option->rect.top(), option->rect.width(), 1},
                          hairline(palette));
        return;
    }
    case PE_PanelMenu:
    case PE_FrameMenu: {
        if (element == PE_PanelMenu) {
            painter->fillRect(option->rect, palette.color(QPalette::Base));
        }
        painter->save();
        painter->setPen(QPen{hairline(palette), 1.0});
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(QRectF{option->rect}.adjusted(0.5, 0.5, -0.5, -0.5));
        painter->restore();
        return;
    }
    case PE_PanelTipLabel: {
        painter->fillRect(option->rect, palette.color(QPalette::Base));
        painter->save();
        painter->setPen(QPen{hairline(palette), 1.0});
        painter->drawRect(QRectF{option->rect}.adjusted(0.5, 0.5, -0.5, -0.5));
        painter->restore();
        return;
    }
    case PE_PanelItemViewItem: {
        const auto* item = qstyleoption_cast<const QStyleOptionViewItem*>(option);
        if (item != nullptr && item->backgroundBrush.style() != Qt::NoBrush) {
            painter->fillRect(option->rect, item->backgroundBrush);
        }
        // A list whose check boxes say what is chosen shows no second
        // selection; only where the keyboard is, outlined.
        if (widget != nullptr && widget->property(checks_show_selection).toBool()) {
            if (option->state.testFlag(State_MouseOver) && option->state.testFlag(State_Enabled)) {
                fill_rounded(painter, rect.adjusted(2, 1, -2, -1), rowHover(palette), radius);
            }
            if (option->state.testFlag(State_HasFocus)) {
                fill_rounded(painter, rect.adjusted(2, 1, -2, -1), QColor{}, radius,
                             palette.color(QPalette::Active, QPalette::Highlight));
            }
            return;
        }
        if (option->state.testFlag(State_Selected)) {
            painter->fillRect(option->rect, selection(palette));
        } else if (option->state.testFlag(State_MouseOver) &&
                   option->state.testFlag(State_Enabled)) {
            painter->fillRect(option->rect, rowHover(palette));
        }
        return;
    }
    case PE_PanelItemViewRow: {
        const auto* item = qstyleoption_cast<const QStyleOptionViewItem*>(option);
        if (item != nullptr && item->features.testFlag(QStyleOptionViewItem::Alternate)) {
            painter->fillRect(option->rect, palette.color(QPalette::AlternateBase));
        }
        return;
    }
    default:
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }
}

void TrackknifeStyle::drawControl(const ControlElement element, const QStyleOption* option,
                                  QPainter* painter, const QWidget* widget) const {
    const auto& palette = option->palette;
    switch (element) {
    case CE_PushButtonBevel: {
        proxy()->drawPrimitive(PE_PanelButtonCommand, option, painter, widget);
        const auto* button = qstyleoption_cast<const QStyleOptionButton*>(option);
        if (button != nullptr && button->features.testFlag(QStyleOptionButton::HasMenu)) {
            const int size = 10;
            const QRect arrow{option->rect.right() - 14 - size + 4,
                              option->rect.center().y() - size / 2, size, size};
            QStyleOption indicator = *option;
            indicator.rect = arrow;
            if (accented(button, widget)) {
                indicator.palette.setColor(QPalette::ButtonText,
                                           palette.color(QPalette::HighlightedText));
            }
            proxy()->drawPrimitive(PE_IndicatorArrowDown, &indicator, painter, widget);
        }
        return;
    }
    case CE_PushButtonLabel: {
        const auto* button = qstyleoption_cast<const QStyleOptionButton*>(option);
        if (button == nullptr) {
            break;
        }
        QStyleOptionButton label = *button;
        const bool accent = accented(button, widget);
        if (accent) {
            auto text = palette.color(QPalette::Active, QPalette::HighlightedText);
            label.palette.setColor(QPalette::Active, QPalette::ButtonText, text);
            label.palette.setColor(QPalette::Inactive, QPalette::ButtonText, text);
            text.setAlphaF(0.6F);
            label.palette.setColor(QPalette::Disabled, QPalette::ButtonText, text);
        }
        if (button->features.testFlag(QStyleOptionButton::HasMenu)) {
            label.rect.setRight(label.rect.right() - 12);
        }
        QProxyStyle::drawControl(element, &label, painter, widget);
        return;
    }
    case CE_TabBarTabShape: {
        const auto* tab = qstyleoption_cast<const QStyleOptionTab*>(option);
        if (tab == nullptr ||
            (tab->shape != QTabBar::RoundedNorth && tab->shape != QTabBar::TriangularNorth)) {
            break;
        }
        const bool selected = option->state.testFlag(State_Selected);
        if (option->state.testFlag(State_MouseOver) && !selected) {
            fill_rounded(painter, QRectF{option->rect}.adjusted(0, 2, 0, -2), hovered(palette),
                         radius);
        }
        if (selected) {
            const QRect line{option->rect.left() + 6, option->rect.bottom() - 1,
                             option->rect.width() - 12, 2};
            fill_rounded(painter, line, palette.color(QPalette::Active, QPalette::Highlight), 1);
        }
        return;
    }
    case CE_TabBarTabLabel: {
        const auto* tab = qstyleoption_cast<const QStyleOptionTab*>(option);
        if (tab == nullptr) {
            break;
        }
        QStyleOptionTab label = *tab;
        if (!option->state.testFlag(State_Selected)) {
            label.palette.setColor(QPalette::WindowText, dim(palette));
            label.palette.setColor(QPalette::ButtonText, dim(palette));
        }
        QProxyStyle::drawControl(element, &label, painter, widget);
        return;
    }
    case CE_HeaderSection: {
        painter->fillRect(option->rect, palette.color(QPalette::Base));
        painter->fillRect(
            QRect{option->rect.left(), option->rect.bottom(), option->rect.width(), 1},
            hairline(palette));
        const auto* header = qstyleoption_cast<const QStyleOptionHeader*>(option);
        if (header != nullptr && header->orientation == Qt::Horizontal &&
            header->position != QStyleOptionHeader::End &&
            header->position != QStyleOptionHeader::OnlyOneSection) {
            painter->fillRect(
                QRect{option->rect.right(), option->rect.top() + 6, 1, option->rect.height() - 12},
                hairline(palette));
        }
        return;
    }
    case CE_HeaderEmptyArea:
        painter->fillRect(option->rect, palette.color(QPalette::Base));
        painter->fillRect(
            QRect{option->rect.left(), option->rect.bottom(), option->rect.width(), 1},
            hairline(palette));
        return;
    case CE_ItemViewItem: {
        const auto* item = qstyleoption_cast<const QStyleOptionViewItem*>(option);
        if (item == nullptr) {
            break;
        }
        QStyleOptionViewItem quiet = *item;
        quiet.palette = quiet_selection(quiet.palette);
        QProxyStyle::drawControl(element, &quiet, painter, widget);
        return;
    }
    case CE_MenuItem: {
        const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
        if (item == nullptr) {
            break;
        }
        QStyleOptionMenuItem quiet = *item;
        quiet.palette = quiet_selection(quiet.palette);
        painter->fillRect(option->rect, palette.color(QPalette::Base));
        if (option->state.testFlag(State_Selected) && option->state.testFlag(State_Enabled) &&
            item->menuItemType != QStyleOptionMenuItem::Separator) {
            fill_rounded(painter, QRectF{option->rect}.adjusted(2, 1, -2, -1), selection(palette),
                         radius);
            quiet.state.setFlag(State_Selected, false);
        }
        // A chosen item is ticked: no box, no dot.
        if (item->checkType != QStyleOptionMenuItem::NotCheckable &&
            item->menuItemType != QStyleOptionMenuItem::Separator) {
            if (item->checked) {
                const QRectF tick_box{QRectF{option->rect}.left() + 7,
                                      QRectF{option->rect}.center().y() - 5, 10, 10};
                painter->save();
                painter->setRenderHint(QPainter::Antialiasing, true);
                auto ink = palette.color(QPalette::Text);
                if (!option->state.testFlag(State_Enabled)) {
                    ink.setAlphaF(0.5F);
                }
                painter->setPen(QPen{ink, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin});
                QPainterPath tick;
                tick.moveTo(tick_box.left(), tick_box.center().y());
                tick.lineTo(tick_box.left() + 3.5, tick_box.bottom() - 1);
                tick.lineTo(tick_box.right(), tick_box.top() + 1);
                painter->drawPath(tick);
                painter->restore();
            }
            quiet.checked = false;
            quiet.checkType = QStyleOptionMenuItem::NotCheckable;
        }
        // Fusion leaves a combo box's list no column for the tick; the
        // text moves over to make one.
        if (combo_popup_item(option, widget) &&
            item->checkType != QStyleOptionMenuItem::NotCheckable) {
            quiet.rect.setLeft(quiet.rect.left() + combo_tick_column);
        }
        if (item->menuItemType == QStyleOptionMenuItem::Separator) {
            const int y = option->rect.center().y();
            painter->fillRect(
                QRect{option->rect.left() + gap, y, option->rect.width() - 2 * gap, 1},
                hairline(palette));
            return;
        }
        QProxyStyle::drawControl(element, &quiet, painter, widget);
        return;
    }
    case CE_MenuBarItem: {
        const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
        if (item == nullptr) {
            break;
        }
        QStyleOptionMenuItem quiet = *item;
        painter->fillRect(option->rect, palette.color(QPalette::Window));
        if (option->state.testFlag(State_Selected) || option->state.testFlag(State_Sunken)) {
            fill_rounded(painter, QRectF{option->rect}.adjusted(1, 2, -1, -2),
                         option->state.testFlag(State_Sunken) ? pressed(palette) : hovered(palette),
                         radius);
        }
        quiet.state.setFlag(State_Selected, false);
        quiet.state.setFlag(State_Sunken, false);
        QProxyStyle::drawControl(element, &quiet, painter, widget);
        return;
    }
    case CE_MenuBarEmptyArea:
        painter->fillRect(option->rect, palette.color(QPalette::Window));
        return;
    case CE_Splitter:
        painter->fillRect(
            option->state.testFlag(State_Horizontal)
                ? QRect{option->rect.center().x(), option->rect.top(), 1, option->rect.height()}
                : QRect{option->rect.left(), option->rect.center().y(), option->rect.width(), 1},
            hairline(palette));
        return;
    case CE_ProgressBarGroove: {
        const QRectF track{QRectF{option->rect}.left(), QRectF{option->rect}.center().y() - 2,
                           QRectF{option->rect}.width(), 4};
        fill_rounded(painter, track, hovered(palette), 2);
        return;
    }
    case CE_ProgressBarContents: {
        const auto* bar = qstyleoption_cast<const QStyleOptionProgressBar*>(option);
        if (bar == nullptr) {
            break;
        }
        const auto range = static_cast<qreal>(bar->maximum) - bar->minimum;
        const qreal done =
            range > 0 ? (static_cast<qreal>(bar->progress) - bar->minimum) / range : 0.25;
        const QRectF track{QRectF{option->rect}.left(), QRectF{option->rect}.center().y() - 2,
                           QRectF{option->rect}.width() * std::clamp(done, 0.0, 1.0), 4};
        fill_rounded(painter, track, palette.color(QPalette::Highlight), 2);
        return;
    }
    default:
        break;
    }
    QProxyStyle::drawControl(element, option, painter, widget);
}

void TrackknifeStyle::drawComplexControl(const ComplexControl control,
                                         const QStyleOptionComplex* option, QPainter* painter,
                                         const QWidget* widget) const {
    const auto& palette = option->palette;
    switch (control) {
    case CC_ScrollBar: {
        const auto* bar = qstyleoption_cast<const QStyleOptionSlider*>(option);
        if (bar == nullptr) {
            break;
        }
        const auto handle = proxy()->subControlRect(control, option, SC_ScrollBarSlider, widget);
        if (handle.isEmpty() || bar->maximum == bar->minimum) {
            return;
        }
        const bool horizontal = bar->orientation == Qt::Horizontal;
        const bool active = option->activeSubControls.testFlag(SC_ScrollBarSlider);
        const bool down = active && option->state.testFlag(State_Sunken);
        const auto thickness = active || option->state.testFlag(State_MouseOver) ? 8.0 : 6.0;
        QRectF thumb = handle;
        if (horizontal) {
            thumb = QRectF{thumb.left() + 2, thumb.center().y() - thickness / 2, thumb.width() - 4,
                           thickness};
        } else {
            thumb = QRectF{thumb.center().x() - thickness / 2, thumb.top() + 2, thickness,
                           thumb.height() - 4};
        }
        const auto window = palette.color(QPalette::Window);
        const auto text = palette.color(QPalette::WindowText);
        fill_rounded(painter, thumb, mix(window, text, down ? 0.5 : 0.3), thickness / 2);
        return;
    }
    case CC_ComboBox: {
        const auto* combo = qstyleoption_cast<const QStyleOptionComboBox*>(option);
        if (combo == nullptr) {
            break;
        }
        const bool down = option->state.testFlag(State_Sunken) || option->state.testFlag(State_On);
        const bool hover =
            option->state.testFlag(State_MouseOver) && option->state.testFlag(State_Enabled);
        const bool focus = option->state.testFlag(State_HasFocus);
        if (combo->editable) {
            fill_rounded(painter, option->rect, palette.color(QPalette::Base), radius,
                         focus ? palette.color(QPalette::Highlight) : border(palette));
        } else {
            fill_rounded(painter, option->rect,
                         down    ? pressed(palette)
                         : hover ? hovered(palette)
                                 : raised(palette),
                         radius,
                         keyboard_focus(option) ? palette.color(QPalette::Highlight) : QColor{});
        }
        QStyleOption arrow = *option;
        arrow.rect = proxy()->subControlRect(control, option, SC_ComboBoxArrow, widget);
        arrow.rect = QRect{arrow.rect.center().x() - 5, arrow.rect.center().y() - 5, 10, 10};
        proxy()->drawPrimitive(PE_IndicatorArrowDown, &arrow, painter, widget);
        return;
    }
    case CC_SpinBox: {
        const auto* spin = qstyleoption_cast<const QStyleOptionSpinBox*>(option);
        if (spin == nullptr) {
            break;
        }
        if (spin->frame) {
            fill_rounded(painter, option->rect, palette.color(QPalette::Base), radius,
                         option->state.testFlag(State_HasFocus) ? palette.color(QPalette::Highlight)
                                                                : border(palette));
        }
        if (spin->buttonSymbols == QAbstractSpinBox::NoButtons) {
            return;
        }
        for (const auto& [sub, primitive] : {std::pair{SC_SpinBoxUp, PE_IndicatorSpinUp},
                                             std::pair{SC_SpinBoxDown, PE_IndicatorSpinDown}}) {
            QStyleOption arrow = *option;
            const auto area = proxy()->subControlRect(control, option, sub, widget);
            if (option->activeSubControls.testFlag(sub)) {
                fill_rounded(painter, QRectF{area}.adjusted(1, 1, -1, -1),
                             option->state.testFlag(State_Sunken) ? pressed(palette)
                                                                  : hovered(palette),
                             radius);
            }
            arrow.rect = QRect{area.center().x() - 4, area.center().y() - 4, 8, 8};
            proxy()->drawPrimitive(primitive, &arrow, painter, widget);
        }
        return;
    }
    case CC_Slider: {
        const auto* slider = qstyleoption_cast<const QStyleOptionSlider*>(option);
        if (slider == nullptr) {
            break;
        }
        const auto groove = proxy()->subControlRect(control, option, SC_SliderGroove, widget);
        const auto handle = proxy()->subControlRect(control, option, SC_SliderHandle, widget);
        const bool horizontal = slider->orientation == Qt::Horizontal;
        const QRectF track = horizontal
                                 ? QRectF{QRectF{groove}.left(), QRectF{groove}.center().y() - 2,
                                          QRectF{groove}.width(), 4}
                                 : QRectF{QRectF{groove}.center().x() - 2, QRectF{groove}.top(), 4,
                                          QRectF{groove}.height()};
        fill_rounded(painter, track, hovered(palette), 2);
        const auto highlight = palette.color(QPalette::Active, QPalette::Highlight);
        const QRectF filled =
            horizontal ? QRectF{track.left(), track.top(),
                                QRectF{handle}.center().x() - track.left(), track.height()}
                       : QRectF{track.left(), QRectF{handle}.center().y(), track.width(),
                                track.bottom() - QRectF{handle}.center().y()};
        fill_rounded(painter, filled, highlight, 2);
        const QPointF centre = QRectF{handle}.center();
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen{palette.color(QPalette::Base), 2.0});
        painter->setBrush(option->state.testFlag(State_Sunken) ? highlight.darker(110) : highlight);
        painter->drawEllipse(centre, 6.0, 6.0);
        painter->restore();
        return;
    }
    default:
        break;
    }
    QProxyStyle::drawComplexControl(control, option, painter, widget);
}

Band::Band(const Edge edge, QWidget* parent) : QWidget{parent}, edge_{edge} {}

void Band::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter{this};
    painter.fillRect(rect(), TrackknifeStyle::sunken(palette()));
    if (edge_ == Edge::top) {
        painter.fillRect(QRect{0, 0, width(), 1}, TrackknifeStyle::hairline(palette()));
    } else if (edge_ == Edge::bottom) {
        painter.fillRect(QRect{0, height() - 1, width(), 1}, TrackknifeStyle::hairline(palette()));
    }
}

SegmentedTabBar::SegmentedTabBar(QWidget* parent) : QTabBar{parent} {
    setDrawBase(false);
    setExpanding(true);
    setUsesScrollButtons(false);
    setMouseTracking(true);
    lift_.setDuration(180);
    lift_.setEasingCurve(QEasingCurve::OutCubic);
    connect(&lift_, &QVariantAnimation::valueChanged, this, [this] { update(); });
    connect(this, &QTabBar::currentChanged, this, [this](const int index) {
        const auto to = static_cast<qreal>(std::max(0, slotOf(index)));
        const auto from = lift_.state() == QAbstractAnimation::Running
                              ? lift_.currentValue().toReal()
                          : lift_.endValue().isValid() ? lift_.endValue().toReal()
                                                       : to;
        lift_.stop();
        lift_.setStartValue(from);
        lift_.setEndValue(to);
        lift_.start();
    });
}

std::vector<int> SegmentedTabBar::shown() const {
    std::vector<int> indexes;
    for (int index = 0; index < count(); ++index) {
        if (isTabVisible(index)) {
            indexes.push_back(index);
        }
    }
    return indexes;
}

int SegmentedTabBar::slotOf(const int index) const {
    const auto indexes = shown();
    const auto found = std::ranges::find(indexes, index);
    return found != indexes.end() ? static_cast<int>(found - indexes.begin()) : -1;
}

qreal SegmentedTabBar::segment() const {
    const auto places = shown().size();
    return places > 0U ? (width() - 4) / static_cast<qreal>(places) : 0.0;
}

QSize SegmentedTabBar::tabSizeHint(const int /*index*/) const {
    // The same width for every tab shown, so where QTabBar takes a click to
    // be is where the segment is drawn.
    const auto places = static_cast<int>(shown().size());
    return {places > 0 ? std::max(1, (width() - 4) / places) : 0, 26};
}

QSize SegmentedTabBar::minimumTabSizeHint(const int index) const {
    return {fontMetrics().horizontalAdvance(tabText(index)) + 2 * TrackknifeStyle::gap, 26};
}

QSize SegmentedTabBar::sizeHint() const {
    int width = 4;
    for (const auto index : shown()) {
        width += minimumTabSizeHint(index).width();
    }
    return {width, 26};
}

QSize SegmentedTabBar::minimumSizeHint() const { return sizeHint(); }

void SegmentedTabBar::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);
    const auto& colours = palette();
    const QRectF box = QRectF{rect()}.adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen{TrackknifeStyle::hairline(colours), 1.0});
    painter.setBrush(colours.color(QPalette::Base));
    painter.drawRoundedRect(box, TrackknifeStyle::radius + 1, TrackknifeStyle::radius + 1);
    const auto indexes = shown();
    if (indexes.empty()) {
        return;
    }
    const auto step = segment();
    const auto current = slotOf(currentIndex());
    // Settled, it is where the current tab is now: a tab hidden or shown
    // since moves it without an animation.
    const auto at = lift_.state() == QAbstractAnimation::Running
                        ? lift_.currentValue().toReal()
                        : static_cast<qreal>(current);
    if (current >= 0) {
        const QRectF lift{2 + at * step + 0.5, 2.5, step - 1, height() - 5.0};
        painter.setPen(QPen{TrackknifeStyle::hairline(colours), 1.0});
        painter.setBrush(TrackknifeStyle::raised(colours));
        painter.drawRoundedRect(lift, TrackknifeStyle::radius, TrackknifeStyle::radius);
    }
    auto small = font();
    small.setPointSizeF(small.pointSizeF() * 0.9);
    painter.setFont(small);
    for (std::size_t slot = 0; slot < indexes.size(); ++slot) {
        const auto index = indexes[slot];
        const QRectF segment_rect{2 + static_cast<qreal>(slot) * step, 2, step, height() - 4.0};
        const bool lit = index == currentIndex() || index == hovered_;
        painter.setPen(lit ? colours.color(QPalette::WindowText) : TrackknifeStyle::dim(colours));
        painter.drawText(segment_rect, Qt::AlignCenter,
                         QFontMetrics{small}.elidedText(tabText(index), Qt::ElideRight,
                                                        static_cast<int>(step) - 8));
    }
}

void SegmentedTabBar::mouseMoveEvent(QMouseEvent* event) {
    const auto step = segment();
    const auto indexes = shown();
    const auto slot = step > 0 ? static_cast<int>((event->position().x() - 2) / step) : -1;
    const auto hovered =
        slot >= 0 && slot < static_cast<int>(indexes.size()) ? indexes[static_cast<std::size_t>(slot)]
                                                             : -1;
    if (hovered != hovered_) {
        hovered_ = hovered;
        update();
    }
    QTabBar::mouseMoveEvent(event);
}

void SegmentedTabBar::leaveEvent(QEvent* event) {
    hovered_ = -1;
    update();
    QTabBar::leaveEvent(event);
}

} // namespace trackknife::bench
