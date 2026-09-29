// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>

namespace trackknife::bench {

// One row of preparation feedback: the affected file and a short problem or
// outcome description. Full texts stay reachable through tooltips.
struct PreparationFeedbackRow {
    QString file;
    QString detail;
};

} // namespace trackknife::bench
