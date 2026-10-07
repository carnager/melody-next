// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/metadata/rule_script_import.hpp"
#include "trackknife/metadata/transformation.hpp"

#include <span>
#include <string>
#include <string_view>

namespace trackknife::metadata {

// ADR-0241: a tagging script as text, in Trackknife's own terms -- one
// statement per typed step, each a $-call whose values and conditions are
// tkfmt-1 and whose capture patterns are tkcapture-1:
//
//   $copy(DATE,ORIGINALYEAR)
//   $set(FMPS_RATING,$decimal(%rating%,5,1))
//   $if($not(%rating%),$delete(FMPS_RATING))
//
// Every typed action has a statement, and export is exact: what it writes
// imports back to the same actions, or it refuses. Nothing is executed; a
// statement only names the typed action it stands for. Picard-style scripts
// stay the Paste script import (ADR-0065), a separate language.
[[nodiscard]] MetadataRuleScriptImportResult
import_native_rule_script(std::string_view source,
                          const MetadataRuleScriptImportLimits& limits = {});

[[nodiscard]] core::Result<std::string>
export_native_rule_script(std::span<const MetadataTransformationAction> actions);

} // namespace trackknife::metadata
