// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QProxyStyle>
#include <QWidget>

#include <cstdint>

namespace trackknife::bench {

// ADR-0250: Trackknife's look, one set of measures and colours for every
// window -- 28 px controls, 4 px corners, flat fills mixed from the palette,
// the accent for default and checked buttons, underlined tabs, thin scroll
// bars. It began as the Qt Quick window's (ADR-0252 retired that window).
// Fusion beneath, for everything not drawn here.
class TrackknifeStyle final : public QProxyStyle {
    Q_OBJECT

  public:
    TrackknifeStyle();

    // A view property: its check boxes show what is chosen, so the style
    // draws no selection fill, only an outline where the keyboard is.
    static constexpr const char* checks_show_selection = "trackknife-checks-show-selection";

    // The measures: spacing, corners, control and row heights.
    static constexpr int gap_small = 4;
    static constexpr int gap = 8;
    static constexpr int gap_large = 16;
    static constexpr int radius = 4;
    static constexpr int popup_radius = 6;
    static constexpr int control_height = 28;
    static constexpr int row_height = 28;

    // The colours, each mixed from a palette.
    [[nodiscard]] static QColor mix(const QColor& a, const QColor& b, qreal t);
    [[nodiscard]] static QColor hairline(const QPalette& palette);
    [[nodiscard]] static QColor border(const QPalette& palette);
    [[nodiscard]] static QColor raised(const QPalette& palette);
    [[nodiscard]] static QColor hovered(const QPalette& palette);
    [[nodiscard]] static QColor pressed(const QPalette& palette);
    [[nodiscard]] static QColor sunken(const QPalette& palette);
    [[nodiscard]] static QColor rowHover(const QPalette& palette);
    [[nodiscard]] static QColor selection(const QPalette& palette);
    [[nodiscard]] static QColor dim(const QPalette& palette);

    void polish(QWidget* widget) override;
    void unpolish(QWidget* widget) override;
    void polish(QPalette& palette) override;

    [[nodiscard]] int pixelMetric(PixelMetric metric, const QStyleOption* option = nullptr,
                                  const QWidget* widget = nullptr) const override;
    [[nodiscard]] int styleHint(StyleHint hint, const QStyleOption* option = nullptr,
                                const QWidget* widget = nullptr,
                                QStyleHintReturn* returned = nullptr) const override;
    [[nodiscard]] QSize sizeFromContents(ContentsType type, const QStyleOption* option,
                                         const QSize& contents,
                                         const QWidget* widget = nullptr) const override;
    [[nodiscard]] QRect subControlRect(ComplexControl control, const QStyleOptionComplex* option,
                                       SubControl sub,
                                       const QWidget* widget = nullptr) const override;

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                       const QWidget* widget = nullptr) const override;
    void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                     const QWidget* widget = nullptr) const override;
    void drawComplexControl(ComplexControl control, const QStyleOptionComplex* option,
                            QPainter* painter, const QWidget* widget = nullptr) const override;
};

// A header, footer or side area, shaded: a step off the window (sunken),
// with a hairline toward the content on one edge.
class Band final : public QWidget {
  public:
    enum class Edge : std::uint8_t { none, top, bottom };
    explicit Band(Edge edge, QWidget* parent = nullptr);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    Edge edge_;
};

} // namespace trackknife::bench
