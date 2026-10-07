// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/rating_stars.hpp"

#include <QApplication>
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
        const auto angle = -std::numbers::pi / 2.0 + point * std::numbers::pi / 5.0;
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
            painter->setClipRect(QRectF{center.x() - radius, static_cast<qreal>(band.top()), radius,
                                        static_cast<qreal>(band.height())});
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
            // Every menu open, as a plain entry closes them: the Rate
            // submenu alone left the menu it came from open, to be rated
            // from again.
            while (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
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

namespace {

constexpr int strip_star = 16;
constexpr int strip_gap = 3;
constexpr int strip_margin = 2;

} // namespace

RatingStrip::RatingStrip(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setAccessibleName(QStringLiteral("Rating"));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void RatingStrip::setRating(const unsigned rating) {
    const auto clamped = std::min(rating, 10U);
    if (clamped == rating_) {
        return;
    }
    rating_ = clamped;
    setToolTip(rating_ == 0U ? QStringLiteral("Not rated")
                             : QStringLiteral("%1 of 10").arg(rating_));
    update();
}

QSize RatingStrip::sizeHint() const {
    return {2 * strip_margin + 5 * strip_star + 4 * strip_gap, strip_star + 2 * strip_margin};
}

unsigned RatingStrip::starAt(const qreal x) const {
    const auto offset = x - strip_margin;
    if (offset < 0.0) {
        return 0U;
    }
    const auto star = static_cast<int>(offset / (strip_star + strip_gap));
    const auto within = offset - star * (strip_star + strip_gap);
    if (star > 4 || within > strip_star) {
        return 0U;
    }
    return static_cast<unsigned>(star + 1) * 2U - (within < strip_star / 2.0 ? 1U : 0U);
}

unsigned RatingStrip::valueAt(const qreal x) const {
    const auto value = starAt(x);
    return value == rating_ ? 0U : value;
}

void RatingStrip::paintEvent(QPaintEvent*) {
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);
    // Hovered: what a click would set; else what is set.
    const auto shown = hovered_ != 0U ? hovered_ : rating_;
    const auto radius = strip_star / 2.0;
    const auto center_y = height() / 2.0;
    auto outline = ratingStarColor();
    outline.setAlpha(130);
    auto fill = ratingStarColor();
    if (hovered_ != 0U) {
        fill.setAlpha(190);
    }
    for (unsigned star = 0U; star < 5U; ++star) {
        const auto left =
            static_cast<qreal>(strip_margin) + star * static_cast<qreal>(strip_star + strip_gap);
        const QPointF center{left + radius, center_y};
        const auto path = starPath(center, radius);
        const auto whole = (star + 1U) * 2U;
        painter.setPen(QPen{outline, 1.2});
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
        if (shown >= whole - 1U) {
            painter.save();
            if (shown == whole - 1U) {
                painter.setClipRect(QRectF{left, 0.0, radius, static_cast<qreal>(height())});
            }
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill);
            painter.drawPath(path);
            painter.restore();
        }
    }
}

void RatingStrip::mouseMoveEvent(QMouseEvent* event) {
    const auto value = starAt(event->position().x());
    if (value != hovered_) {
        hovered_ = value;
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void RatingStrip::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && starAt(event->position().x()) != 0U) {
        const auto value = valueAt(event->position().x());
        hovered_ = 0U;
        setRating(value);
        emit rated(value);
    }
    QWidget::mouseReleaseEvent(event);
}

void RatingStrip::leaveEvent(QEvent* event) {
    hovered_ = 0U;
    update();
    QWidget::leaveEvent(event);
}

RatingMenuAction::RatingMenuAction(const unsigned rating, QObject* parent)
    : QWidgetAction(parent), rating_(std::min(rating, 10U)) {
    setText(ratingMenuLabel(rating_));
    setData(rating_);
    setCheckable(true);
}

QWidget* RatingMenuAction::createWidget(QWidget* parent) {
    return new RatingMenuItem{this, parent};
}

} // namespace trackknife::ui
