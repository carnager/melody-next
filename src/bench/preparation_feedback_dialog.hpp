// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/preparation_feedback.hpp"

#include <QString>

#include <vector>

class QDialog;
class QWidget;

namespace trackknife::bench {

// Compact window used only when an Apply cannot finish silently: a blocked
// preparation, a stopped run, or per-file failures. Success never opens it.
[[nodiscard]] QDialog*
createPreparationFeedbackDialog(const QString& window_title, const QString& summary,
                                const std::vector<PreparationFeedbackRow>& rows, QWidget* parent);

} // namespace trackknife::bench
