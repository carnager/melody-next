// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "uicommon/application_style.hpp"

#include <QApplication>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleOptionViewItem>
#include <QWidget>

namespace trackknife::ui {

// For a view whose delegate paints selection itself, as the track lists and
// the library do: before each cell the view has the style paint the row's
// panel, and some styles -- Fusion, macOS -- fill a selected row's with the
// highlight there, under cells the delegate means to keep clear (the cover
// gutter, a tree's indent). The panel is painted as for a row not selected;
// everything else is the application's style.
class DelegateSelectionStyle final : public QProxyStyle {
  public:
    using QProxyStyle::QProxyStyle;

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                       const QWidget* widget) const override {
        if (element == PE_PanelItemViewRow) {
            if (const auto* row = qstyleoption_cast<const QStyleOptionViewItem*>(option)) {
                auto unselected = *row;
                unselected.state &= ~State_Selected;
                QProxyStyle::drawPrimitive(element, &unselected, painter, widget);
                return;
            }
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }

    // On the style the application uses: a proxy made without one would
    // wrap the desktop's default instead.
    static void install(QWidget* view) {
        auto* style =
            new DelegateSelectionStyle(createApplicationStyle());
        style->setParent(view);
        view->setStyle(style);
    }
};

} // namespace trackknife::ui
