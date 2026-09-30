// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QObject>
#include <QPointF>
#include <QQuickItem>
#include <QtQmlIntegration>



namespace trackknife::quick {

// Where the pointer is, in an item's own coordinates: a tooltip shows by it,
// as the desktop's do, not centred over its whole item.
class Pointer final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

  public:
    using QObject::QObject;

    Q_INVOKABLE [[nodiscard]] static QPointF at(QQuickItem* item);
};

} // namespace trackknife::quick
