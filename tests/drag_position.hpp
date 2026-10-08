// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QDragEnterEvent>
#include <QMimeData>
#include <QPoint>
#include <QPointF>

#include <type_traits>

// Where a test's drag happens, as this Qt's drag events take it: a QPointF
// where they have that constructor -- Qt 6.12 deprecates the QPoint one --
// else the QPoint, all that Qt 6.8 (Debian trixie, CI) has. Asked of the
// constructors themselves.
[[nodiscard]] inline auto dragPosition(const QPoint& point) {
    if constexpr (std::is_constructible_v<QDragEnterEvent, QPointF, Qt::DropActions,
                                          const QMimeData*, Qt::MouseButtons,
                                          Qt::KeyboardModifiers>) {
        return QPointF(point);
    } else {
        return point;
    }
}
