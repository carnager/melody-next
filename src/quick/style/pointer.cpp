// SPDX-License-Identifier: GPL-3.0-only
#include "pointer.hpp"

#include <QCursor>

namespace trackknife::quick {

QPointF Pointer::at(QQuickItem* item) {
    if (item == nullptr) {
        return {};
    }
    return item->mapFromGlobal(QCursor::pos());
}

} // namespace trackknife::quick
