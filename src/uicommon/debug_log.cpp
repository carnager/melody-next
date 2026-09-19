// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/debug_log.hpp"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(tkDebug, "trackknife.debug")

namespace trackknife::ui {

void enableDebugLogging() {
    QLoggingCategory::setFilterRules(QStringLiteral("trackknife.debug=true"));
    qSetMessagePattern(QStringLiteral("%{time hh:mm:ss.zzz} %{category}: %{message}"));
}

} // namespace trackknife::ui
