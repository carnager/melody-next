// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QLoggingCategory>

// Diagnostic logging for the parts that are hard to see from the outside:
// server commands and their replies, playback-context switches, and what the
// workspace restored at startup. Off unless `--debug` (or the usual
// QT_LOGGING_RULES) turns the category on, so ordinary runs stay quiet.
Q_DECLARE_LOGGING_CATEGORY(tkDebug)

namespace trackknife::ui {

// Turns the category on and stamps every line with a timestamp, so a log
// pasted into a bug report carries its own ordering.
void enableDebugLogging();

} // namespace trackknife::ui
