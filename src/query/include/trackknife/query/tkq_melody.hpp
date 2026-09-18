// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/query/tkq.hpp"

#include <string>

namespace trackknife::query {

// A tkq-1 query translated into the Melody server's MPD filter dialect
// (melody repo, docs/protocol.md). The expression is a single AND-joined
// filter usable with search/find/searchalbums; the sort is a Melody
// `sort` argument ("album", "-rating", ...) or empty.
struct MelodyTranslatedQuery {
    std::string filter_expression;
    std::string sort;
};

// Translates the supported tkq subset one-to-one: field HAS/IS terms, the
// `*` word search, numeric comparisons on the rating and technical
// pseudo-fields, rating PRESENT, and simple single-field sorts. Everything
// the server's grammar cannot express — OR/NOT trees, tkfmt expression
// predicates, MISSING, numeric comparisons on ordinary tags — is a typed
// error naming the unsupported construct, never a silently broadened query.
[[nodiscard]] core::Result<MelodyTranslatedQuery>
translate_tkq_to_melody(const CompiledTkq& compiled);

} // namespace trackknife::query
