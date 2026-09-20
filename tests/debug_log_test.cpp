// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/debug_log.hpp"

#include <QDebug>
#include <QString>

#include <cstdio>

namespace {
int debug_messages = 0;
int warning_messages = 0;
void capture(QtMsgType type, const QMessageLogContext&, const QString&) {
    if (type == QtDebugMsg)
        ++debug_messages;
    if (type == QtWarningMsg)
        ++warning_messages;
}
} // namespace

int main() {
    qunsetenv("QT_LOGGING_RULES");
    qunsetenv("QT_LOGGING_CONF");
    const auto previous_handler = qInstallMessageHandler(capture);
    qCDebug(tkDebug) << "ordinary startup";
    qCWarning(tkDebug) << "warning remains visible";
    const auto quiet_by_default = debug_messages == 0 && warning_messages == 1;

    trackknife::ui::enableDebugLogging();
    qCDebug(tkDebug) << "explicit debug startup";
    const auto enabled_explicitly = debug_messages == 1;
    qInstallMessageHandler(previous_handler);
    if (!quiet_by_default || !enabled_explicitly) {
        std::fputs("Debug output must be opt-in; warnings must remain visible\n", stderr);
        return 1;
    }
    return 0;
}
