// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QApplication>
#include <QHeaderView>
#include <QPainter>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleOptionHeader>

namespace trackknife::ui {

// A track list's column header as it was designed under Kvantum, drawn the
// same under every style: flat on the list's own ground, bold labels a step
// quieter than its text, short separators between the columns, no rule below.
// Other styles raise it into a panel with full-height lines and a dark edge.
// Colours come from the palette, so a light theme gets its own.
class FlatHeaderStyle final : public QProxyStyle {
  public:
    using QProxyStyle::QProxyStyle;

    // How strongly the text colour shows through the ground.
    static constexpr double label_share = 0.586;
    static constexpr double separator_share = 0.157;
    // Above and below the label together.
    static constexpr int height_padding = 14;
    // A separator's length, centred in the section.
    static constexpr int separator_length = 18;
    static constexpr int label_margin = 6;

    [[nodiscard]] static QColor blend(const QPalette& palette, const double share) {
        const auto ground = palette.color(QPalette::Base);
        const auto text = palette.color(QPalette::Text);
        const auto mix = [share](const int from, const int to) {
            return static_cast<int>(from + ((to - from) * share) + 0.5);
        };
        return QColor::fromRgb(mix(ground.red(), text.red()), mix(ground.green(), text.green()),
                               mix(ground.blue(), text.blue()));
    }
    [[nodiscard]] static QFont labelFont(const QWidget* widget) {
        auto font = widget != nullptr ? widget->font() : QApplication::font();
        font.setBold(true);
        return font;
    }

    void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                     const QWidget* widget) const override {
        if (element == CE_HeaderEmptyArea) {
            painter->fillRect(option->rect, option->palette.color(QPalette::Base));
            return;
        }
        const auto* header = qstyleoption_cast<const QStyleOptionHeader*>(option);
        if (element != CE_Header || header == nullptr) {
            QProxyStyle::drawControl(element, option, painter, widget);
            return;
        }
        const auto& rect = header->rect;
        painter->save();
        painter->fillRect(rect, header->palette.color(QPalette::Base));
        // Between this column and the next.
        if (header->position != QStyleOptionHeader::End &&
            header->position != QStyleOptionHeader::OnlyOneSection) {
            const auto top = rect.top() + ((rect.height() - separator_length) / 2);
            painter->fillRect(QRect(rect.right(), top, 1, separator_length),
                              blend(header->palette, separator_share));
        }
        auto label = rect.adjusted(label_margin, 0, -label_margin, 0);
        if (header->sortIndicator != QStyleOptionHeader::None) {
            auto arrow = *header;
            arrow.rect = subElementRect(SE_HeaderArrow, header, widget);
            proxy()->drawPrimitive(PE_IndicatorHeaderArrow, &arrow, painter, widget);
            label.setRight(std::min(label.right(), arrow.rect.left() - label_margin));
        }
        const auto font = labelFont(widget);
        painter->setFont(font);
        painter->setPen(blend(header->palette, label_share));
        painter->drawText(label, static_cast<int>(header->textAlignment | Qt::AlignVCenter),
                          QFontMetrics(font).elidedText(header->text, Qt::ElideRight,
                                                        label.width()));
        painter->restore();
    }

    [[nodiscard]] QSize sizeFromContents(ContentsType type, const QStyleOption* option,
                                         const QSize& size, const QWidget* widget) const override {
        auto measured = QProxyStyle::sizeFromContents(type, option, size, widget);
        if (type != CT_HeaderSection) {
            return measured;
        }
        const QFontMetrics metrics(labelFont(widget));
        if (const auto* header = qstyleoption_cast<const QStyleOptionHeader*>(option)) {
            measured.setWidth(std::max(measured.width(), metrics.horizontalAdvance(header->text) +
                                                             (2 * label_margin) + 2));
        }
        measured.setHeight(metrics.height() + height_padding);
        return measured;
    }

    // On the style the application uses: a proxy made without one would
    // wrap the desktop's default instead.
    static void install(QHeaderView* header) {
        auto* style = new FlatHeaderStyle(QStyleFactory::create(QApplication::style()->name()));
        style->setParent(header);
        header->setStyle(style);
    }
};

} // namespace trackknife::ui
