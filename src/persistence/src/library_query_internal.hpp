// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <string>

namespace trackknife::persistence::internal {

// Technical pseudo-fields resolve to typed index columns; nullptr means
// the canonical name is an ordinary tag field. Shared by the SQL planner
// and the in-memory row evaluation (ADR-0150/0153).
[[nodiscard]] const char* tkq_technical_column(const std::string& canonical);

// Index-level canonicalization of a query field name.
[[nodiscard]] std::string tkq_canonical_field(const std::string& field);

} // namespace trackknife::persistence::internal
