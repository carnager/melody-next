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
// pseudo-fields, rating PRESENT, and simple single-field sorts.
//
// When the server advertises the `filtergrammar` command (grammar level 2,
// melody repo docs/protocol.md), pass full_grammar = true: OR and NOT
// trees, PRESENT/MISSING on ordinary tags (the MPD empty-value forms), and
// numeric comparisons on ordinary tags then translate too. Everything the
// target grammar cannot express — always including tkfmt expression
// predicates — is a typed error naming the unsupported construct, never a
// silently broadened query.
[[nodiscard]] core::Result<MelodyTranslatedQuery>
translate_tkq_to_melody(const CompiledTkq& compiled, bool full_grammar = false,
                        bool history_filters = false, bool history_sort = false);

} // namespace trackknife::query
