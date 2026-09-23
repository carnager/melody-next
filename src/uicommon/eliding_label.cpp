// SPDX-License-Identifier: GPL-3.0-only

#include "eliding_label.hpp"

#include <QPainter>
#include <QStyle>
#include <QStyleOption>

namespace trackknife::ui {

ElidingLabel::ElidingLabel(QWidget* parent) : QLabel(parent) {
    setTextFormat(Qt::PlainText);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
}

QSize ElidingLabel::minimumSizeHint() const {
    return {0, fontMetrics().height()};
}

QSize ElidingLabel::sizeHint() const {
    return {fontMetrics().horizontalAdvance(text()), fontMetrics().height()};
}

void ElidingLabel::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter{this};
    const auto area = contentsRect();
    const auto shown = fontMetrics().elidedText(text(), Qt::ElideRight, area.width());
    QStyleOption option;
    option.initFrom(this);
    style()->drawItemText(&painter, area, static_cast<int>(alignment()), option.palette,
                          isEnabled(), shown, foregroundRole());
}

} // namespace trackknife::ui
