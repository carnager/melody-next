// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/titleformat/compiler.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::query {

// ADR-0150: tkq is Trackknife's own library query dialect. The surface is
// foobar-inspired; this specification is normative and no external
// compatibility is promised. Persisted queries must carry the exact source
// together with the dialect identity below.
inline constexpr std::string_view tkq_dialect = "tkq";
inline constexpr int tkq_dialect_version = 1;
inline constexpr int tkq_compiler_schema = 1;

struct TkqLimits {
    std::size_t maximum_source_bytes{4'096U};
    std::size_t maximum_nodes{256U};
    std::size_t maximum_words{64U};
};

enum class TkqComparison : std::uint8_t { has, is, greater, less, equal, present, missing };

// A predicate's left-hand side: a plain field name, the `*` wildcard, or an
// embedded tkfmt-1 expression whose evaluated text feeds the operator.
enum class TkqOperandKind : std::uint8_t { field, any_field, expression };

struct TkqPredicate {
    TkqOperandKind operand{TkqOperandKind::field};
    // Lowercased as written; index-level canonicalization is the planner's
    // concern so the parser stays free of metadata vocabulary.
    std::string field;
    std::size_t program_index{0U};
    TkqComparison comparison{TkqComparison::has};
    // Comparison text as written and simple-lowercased, plus the lowered
    // words for HAS; numeric operators carry the parsed integer instead.
    std::string text;
    std::string normalized;
    std::vector<std::string> words;
    std::int64_t number{0};
};

enum class TkqNodeKind : std::uint8_t { predicate, and_node, or_node, not_node };

struct TkqNode {
    TkqNodeKind kind{TkqNodeKind::predicate};
    std::size_t predicate_index{0U};
    std::vector<std::size_t> children;
};

enum class TkqSortDirection : std::uint8_t { ascending, descending };

struct TkqSort {
    TkqSortDirection direction{TkqSortDirection::ascending};
    // Exact tkfmt-1 source, compiled in the sort context.
    std::string source;
    titleformat::Program program;
};

struct CompiledTkq {
    // The exact original source and the dialect identity: everything a
    // persisted query must retain (compatibility.md).
    std::string source;
    std::string dialect{tkq_dialect};
    int dialect_version{tkq_dialect_version};
    int compiler_schema{tkq_compiler_schema};
    // ALL matches every indexed track and carries no expression tree.
    bool match_all{false};
    std::vector<TkqPredicate> predicates;
    std::vector<TkqNode> nodes;
    std::size_t root{0U};
    // Embedded tkfmt-1 expression predicates, referenced by program_index.
    std::vector<titleformat::Program> programs;
    std::optional<TkqSort> sort;

    // Lowercased field names the query reads directly, for future
    // incremental invalidation; tkfmt dependencies come from the programs.
    [[nodiscard]] std::vector<std::string> field_dependencies() const;
};

// Strict compilation with positioned failures; a structured query never
// silently degrades into a word search. Bare words without any reserved
// token compile to an all-word `* HAS` query.
[[nodiscard]] core::Result<CompiledTkq> compile_tkq(std::string_view source,
                                                    const TkqLimits& limits = {});

// Compiles the input as a plain all-word search regardless of any
// keywords or quotes it contains — the word-search surfaces share the
// evaluation pipeline without inheriting tkq structure (ADR-0153).
[[nodiscard]] core::Result<CompiledTkq> compile_tkq_word_search(std::string_view source,
                                                                const TkqLimits& limits = {});

} // namespace trackknife::query
