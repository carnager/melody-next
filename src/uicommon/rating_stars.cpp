// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/rating_stars.hpp"

#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace trackknife::ui {
namespace {

[[nodiscard]] QPainterPath starPath(const QPointF& center, const qreal outer_radius) {
    QPainterPath path;
    const auto inner_radius = outer_radius * 0.42;
    for (int point = 0; point < 10; ++point) {
        const auto radius = point % 2 == 0 ? outer_radius : inner_radius;
        const auto angle =
            -std::numbers::pi / 2.0 + point * std::numbers::pi / 5.0;
        const QPointF vertex{center.x() + radius * std::cos(angle),
                             center.y() + radius * std::sin(angle)};
        if (point == 0) {
            path.moveTo(vertex);
        } else {
            path.lineTo(vertex);
        }
    }
    path.closeSubpath();
    return path;
}

} // namespace

void paintRatingOverlay(QPainter* painter, const QRect& cover, const unsigned rating) {
    if (painter == nullptr || rating == 0U || rating > 10U || cover.width() < 24 ||
        cover.height() < 24) {
        return;
    }
    const auto band_height = std::clamp(cover.height() / 6, 9, 18);
    const QRect band{cover.left(), cover.bottom() - band_height + 1, cover.width(), band_height};
    const auto full_stars = rating / 2U;
    const auto half_star = rating % 2U != 0U;
    const auto star_slots = static_cast<int>(full_stars + (half_star ? 1U : 0U));
    const auto star_extent =
        std::min(band_height - 2, (band.width() - 4) / std::max(1, star_slots));
    if (star_extent < 4) {
        return;
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->fillRect(band, QColor{0, 0, 0, 140});
    painter->setPen(Qt::NoPen);
    painter->setBrush(ratingStarColor());
    const auto total_width = star_slots * star_extent;
    auto left = band.center().x() - total_width / 2.0;
    const auto center_y = band.center().y() + 0.5;
    const auto radius = star_extent / 2.0;
    for (int slot = 0; slot < star_slots; ++slot) {
        const QPointF center{left + radius, center_y};
        const auto star = starPath(center, radius);
        if (half_star && slot + 1 == star_slots) {
            painter->save();
            painter->setClipRect(QRectF{center.x() - radius, static_cast<qreal>(band.top()),
                                        radius, static_cast<qreal>(band.height())});
            painter->drawPath(star);
            painter->restore();
        } else {
            painter->drawPath(star);
        }
        left += star_extent;
    }
    painter->restore();
}

namespace {

class RatingMenuItem final : public QWidget {
  public:
    RatingMenuItem(RatingMenuAction* action, QWidget* parent) : QWidget(parent), action_(action) {
        setMouseTracking(true);
        setAttribute(Qt::WA_Hover, true);
    }

    [[nodiscard]] QSize sizeHint() const override {
        return {check_margin + 5 * star_extent + 4 * star_gap + right_margin, 26};
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter{this};
        painter.setRenderHint(QPainter::Antialiasing, true);
        const auto hovered = underMouse();
        if (hovered) {
            auto band = palette().color(QPalette::Highlight);
            painter.fillRect(rect(), band);
        }
        if (action_ != nullptr && action_->isChecked()) {
            const QRect mark{6, height() / 2 - 6, 12, 12};
            painter.save();
            painter.setPen(
                QPen{palette().color(hovered ? QPalette::HighlightedText : QPalette::Text), 1.6});
            painter.setBrush(Qt::NoBrush);
            painter.drawLine(mark.left(), mark.center().y(), mark.center().x(), mark.bottom());
            painter.drawLine(mark.center().x(), mark.bottom(), mark.right(), mark.top());
            painter.restore();
        }
        const auto filled = action_ != nullptr ? action_->rating() / 2U : 0U;
        auto left = static_cast<qreal>(check_margin);
        const auto radius = star_extent / 2.0;
        const auto center_y = height() / 2.0;
        for (unsigned star = 0U; star < 5U; ++star) {
            const QPointF center{left + radius, center_y};
            const auto path = starPath(center, radius);
            if (star < filled) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(ratingStarColor());
            } else {
                auto outline = ratingStarColor();
                outline.setAlpha(130);
                painter.setPen(QPen{outline, 1.2});
                painter.setBrush(Qt::NoBrush);
            }
            painter.drawPath(path);
            left += star_extent + star_gap;
        }
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && action_ != nullptr) {
            if (auto* menu = qobject_cast<QMenu*>(parentWidget())) {
                menu->close();
            }
            action_->trigger();
        }
        QWidget::mouseReleaseEvent(event);
    }

    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }

  private:
    static constexpr int check_margin = 24;
    static constexpr int star_extent = 16;
    static constexpr int star_gap = 3;
    static constexpr int right_margin = 14;

    RatingMenuAction* action_;
};

} // namespace

RatingMenuAction::RatingMenuAction(const unsigned rating, QObject* parent)
    : QWidgetAction(parent), rating_(std::min(rating, 10U)) {
    setText(ratingMenuLabel(rating_));
    setData(rating_);
    setCheckable(true);
}

QWidget* RatingMenuAction::createWidget(QWidget* parent) {
    return new RatingMenuItem{this, parent};
}

QString ratingMenuLabel(const unsigned rating) {
    if (rating == 0U) {
        return QStringLiteral("Unrate");
    }
    if (rating > 10U) {
        return {};
    }
    const auto half = rating % 2U != 0U;
    const auto stars = rating / 2U;
    if (half) {
        return stars == 0U ? QStringLiteral("½ star")
                           : QStringLiteral("%1½ stars").arg(stars);
    }
    return stars == 1U ? QStringLiteral("1 star") : QStringLiteral("%1 stars").arg(stars);
}

} // namespace trackknife::ui
