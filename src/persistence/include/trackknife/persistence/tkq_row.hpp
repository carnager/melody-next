// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/query/tkq.hpp"
#include "trackknife/titleformat/compiler.hpp"

#include <array>
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
    // ADR-0179: stored 0-10 ratings joined by content identity; -1 when
    // unrated. Like the technicals, the `rating`/`albumrating` pseudo-fields
    // shadow same-named file tags.
    std::int64_t rating{-1};
    std::int64_t album_rating{-1};
    // When the track, and its album's newest track, came into the library
    // (Unix seconds); 0 when not known. Read as days since by the
    // `dayssinceadded` and `albumdayssinceadded` pseudo-fields.
    std::int64_t added{0};
    std::int64_t album_added{0};
    // Explicit HISTORY accessors; absence means unavailable, not unplayed.
    // playcount, lastplayed, dayssinceplayed, and their three album equivalents.
    std::optional<std::array<std::int64_t, 6>> history;
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

// Formats one row with a compiled tkfmt-1 program: its tags as fields, its
// technicals through $info. `host` holds the fields the caller owns -- now
// playing's playback_time, say -- keyed by canonical name; they win over a
// same-named tag.
[[nodiscard]] core::Result<std::string>
tkq_format(const titleformat::Program& program, const TkqRowFacts& facts,
           const std::map<std::string, std::string>& host = {},
           const core::CancellationToken& cancellation = {});

} // namespace trackknife::persistence
