// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/query/tkq.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace trackknife::persistence {

// ADR-0153: everything tkq evaluation needs to know about one row,
// independent of where the row lives — the library planner's candidate
// scan and tab-scoped searches share these semantics exactly.
struct TkqRowFacts {
    // Canonical field name -> values in order, original beside the
    // simple-lowercased form.
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> fields;
    // Lowercased denormalized search text for `* HAS` (title, artist,
    // album, artist values), mirroring the index's search_track.
    std::string search_text;
    // Display fallbacks for tkfmt expression predicates when the tag is
    // absent (filename-derived titles, "Unknown artist", ...).
    std::string title;
    std::string artist;
    std::string album;
    std::string date;
    std::string codec;
    std::int64_t sample_rate{0};
    std::int64_t bits{0};
    std::int64_t channels{0};
    std::int64_t duration_ms{-1};
};

struct TkqRowTechnicals {
    std::string codec;
    int sample_rate{0};
    int bits{0};
    int channels{0};
};

// Builds row facts from an ordered metadata document plus optional
// probe technicals; empty values are not indexed, oversized values are
// kept (rows are already bounded upstream).
[[nodiscard]] TkqRowFacts make_tkq_row_facts(const metadata::MetadataDocument& document,
                                             const std::string& title, const std::string& artist,
                                             const std::string& album,
                                             std::optional<std::int64_t> duration_ms,
                                             const std::optional<TkqRowTechnicals>& technicals);

// Exact in-memory evaluation of a compiled query against one row.
[[nodiscard]] bool tkq_matches(const query::CompiledTkq& compiled, const TkqRowFacts& facts,
                               const core::CancellationToken& cancellation = {});

// Evaluates the query's SORT BY key for one row (lowercased); an error
// carries the tkfmt failure.
[[nodiscard]] core::Result<std::string>
tkq_sort_key(const query::CompiledTkq& compiled, const TkqRowFacts& facts,
             const core::CancellationToken& cancellation = {});

} // namespace trackknife::persistence
