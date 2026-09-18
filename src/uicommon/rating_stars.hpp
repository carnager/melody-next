// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QColor>
#include <QRect>
#include <QString>
#include <QWidgetAction>

class QPainter;

namespace trackknife::ui {

// The one star color used everywhere ratings render (ADR-0179).
[[nodiscard]] inline QColor ratingStarColor() { return QColor{245, 197, 24}; }

// Paints a 0-10 rating as filled star shapes (half-star steps) on a
// translucent band along the bottom edge of an album cover. Does nothing for
// unrated values or covers too small to stay legible.
void paintRatingOverlay(QPainter* painter, const QRect& cover, unsigned rating);

// The accessible menu label for a rating ("Unrate", "1 star", … "5 stars").
[[nodiscard]] QString ratingMenuLabel(unsigned rating);

// One Rate submenu row painted as a full five-star strip: filled yellow
// stars up to the value, muted outlines for the rest, plus the shared menu
// hover band and check mark. Triggering closes the owning menu. Stays a
// plain QAction for callers: object names, checked state, data(), and
// triggered() behave as usual.
class RatingMenuAction final : public QWidgetAction {
    Q_OBJECT

  public:
    explicit RatingMenuAction(unsigned rating, QObject* parent = nullptr);
    [[nodiscard]] unsigned rating() const noexcept { return rating_; }

  protected:
    QWidget* createWidget(QWidget* parent) override;

  private:
    unsigned rating_;
};

} // namespace trackknife::ui
