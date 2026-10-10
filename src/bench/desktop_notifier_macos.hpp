// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>

#include <functional>

class QObject;

namespace trackknife::bench {

// ADR-0275: one now-playing notification through the macOS notification
// center, replacing the one before it. `done` runs on `context`'s thread with
// an empty string, or why nothing was shown.
void postMacNotification(QObject* context, const QString& summary, const QString& body,
                         const QString& image, std::function<void(const QString&)> done);

} // namespace trackknife::bench
