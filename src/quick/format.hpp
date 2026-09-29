// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>

#include <cmath>

namespace trackknife::quick {

// "4:12", "1:02:07".
[[nodiscard]] inline QString formatDuration(const qreal seconds) {
    const auto total = static_cast<qint64>(std::max<qreal>(0, std::floor(seconds)));
    const auto hours = total / 3600;
    const auto minutes = (total / 60) % 60;
    const auto rest = total % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(rest, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(minutes).arg(rest, 2, 10, QLatin1Char('0'));
}

} // namespace trackknife::quick
