// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/capture_pattern.hpp"
#include "trackknife/metadata/staged_patch.hpp"
#include "trackknife/titleformat/compiler.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace trackknife::metadata {

enum class MetadataFieldMatchMode : std::uint8_t {
    logical,
    exact_native,
};

struct MetadataSetValuesAction {
    std::string target_field;
    std::vector<std::string> values;

    friend bool operator==(const MetadataSetValuesAction&,
                           const MetadataSetValuesAction&) = default;
};

struct MetadataAddValuesAction {
    std::string target_field;
    std::vector<std::string> values;

    friend bool operator==(const MetadataAddValuesAction&,
                           const MetadataAddValuesAction&) = default;
};

struct MetadataRemoveFieldAction {
    std::string target_field;
    MetadataFieldMatchMode match_mode{MetadataFieldMatchMode::logical};

    friend bool operator==(const MetadataRemoveFieldAction&,
                           const MetadataRemoveFieldAction&) = default;
};

// Removes the target only when the pure tkfmt-1 condition evaluates to a
// non-empty string against the working document at this point in the chain.
struct MetadataRemoveFieldIfAction {
    std::string target_field;
    titleformat::DialectVersion dialect;
    std::string condition;
    MetadataFieldMatchMode match_mode{MetadataFieldMatchMode::logical};

    friend bool operator==(const MetadataRemoveFieldIfAction&,
                           const MetadataRemoveFieldIfAction&) = default;
};

// Removes every logical metadata field named in fields. This is a compact,
// reusable cleanup action; field matching is case-insensitive and uses the
// same canonical identities as the rest of the transformation engine.
struct MetadataBlocklistFieldsAction {
    std::vector<std::string> fields;

    friend bool operator==(const MetadataBlocklistFieldsAction&,
                           const MetadataBlocklistFieldsAction&) = default;
};

// Removes every logical metadata field except those named in fields.
struct MetadataAllowlistFieldsAction {
    std::vector<std::string> fields;

    friend bool operator==(const MetadataAllowlistFieldsAction&,
                           const MetadataAllowlistFieldsAction&) = default;
};

enum class MetadataValueTransformKind : std::uint8_t {
    trim_ascii,
    lowercase,
    uppercase,
    capitalize_first,
};

struct MetadataTransformValuesAction {
    std::string target_field;
    MetadataValueTransformKind transform{MetadataValueTransformKind::trim_ascii};

    friend bool operator==(const MetadataTransformValuesAction&,
                           const MetadataTransformValuesAction&) = default;
};

struct MetadataFormatValueAction {
    std::string target_field;
    titleformat::DialectVersion dialect;
    std::string source;

    friend bool operator==(const MetadataFormatValueAction&,
                           const MetadataFormatValueAction&) = default;
};

struct MetadataCopyFieldAction {
    std::string target_field;
    std::string source_field;

    friend bool operator==(const MetadataCopyFieldAction&,
                           const MetadataCopyFieldAction&) = default;
};

struct MetadataSplitValuesAction {
    std::string target_field;
    std::string separator;

    friend bool operator==(const MetadataSplitValuesAction&,
                           const MetadataSplitValuesAction&) = default;
};

struct MetadataJoinValuesAction {
    std::string target_field;
    std::string separator;

    friend bool operator==(const MetadataJoinValuesAction&,
                           const MetadataJoinValuesAction&) = default;
};

// Removes only values that exactly match the UTF-8 byte sequence. If every
// value matches, the field becomes missing.
struct MetadataRemoveMatchingValuesAction {
    std::string target_field;
    std::string match;

    friend bool operator==(const MetadataRemoveMatchingValuesAction&,
                           const MetadataRemoveMatchingValuesAction&) = default;
};

// Replaces each exact matching value with the complete ordered replacement
// sequence. Matching is case-sensitive and performs no normalization.
struct MetadataReplaceMatchingValuesAction {
    std::string target_field;
    std::string match;
    std::vector<std::string> replacement_values;

    friend bool operator==(const MetadataReplaceMatchingValuesAction&,
                           const MetadataReplaceMatchingValuesAction&) = default;
};

// Assigns one decimal value per selected item in ascending selection order.
// Padding 0 writes the shortest decimal form; positive padding is a minimum
// width and never truncates a larger number.
struct MetadataNumberSelectedItemsAction {
    std::string target_field;
    std::uint32_t start{1U};
    std::uint32_t padding{0U};

    friend bool operator==(const MetadataNumberSelectedItemsAction&,
                           const MetadataNumberSelectedItemsAction&) = default;
};

// Numbers selected items in selection order with an independent counter
// per evaluated tkfmt-1 group value, so every album or disc restarts at
// start. An empty group result shares one counter.
struct MetadataNumberGroupedItemsAction {
    std::string target_field;
    titleformat::DialectVersion dialect;
    std::string group_expression;
    std::uint32_t start{1U};
    std::uint32_t padding{0U};

    friend bool operator==(const MetadataNumberGroupedItemsAction&,
                           const MetadataNumberGroupedItemsAction&) = default;
};

// Keeps at most the first character_count Unicode scalar values of every
// existing value. Shorter and empty values are retained exactly; a missing
// target field remains missing.
struct MetadataKeepFirstCharactersAction {
    std::string target_field;
    std::uint32_t character_count{1U};

    friend bool operator==(const MetadataKeepFirstCharactersAction&,
                           const MetadataKeepFirstCharactersAction&) = default;
};

enum class MetadataCaptureSourceKind : std::uint8_t {
    filename = 0,
    full_path = 1,
    formatted = 2,
    field = 3,
};

// Recognizes one complete source with tkcapture-1 and replaces every named
// logical target captured by the pattern. The source payload is required only
// for formatted and field sources.
struct MetadataCaptureValuesAction {
    CapturePatternDialectVersion dialect;
    MetadataCaptureSourceKind source_kind{MetadataCaptureSourceKind::filename};
    std::string source;
    std::string pattern;

    friend bool operator==(const MetadataCaptureValuesAction&,
                           const MetadataCaptureValuesAction&) = default;
};

using MetadataTransformationAction =
    std::variant<MetadataSetValuesAction, MetadataAddValuesAction, MetadataRemoveFieldAction,
                 MetadataRemoveFieldIfAction, MetadataTransformValuesAction,
                 MetadataFormatValueAction, MetadataCopyFieldAction, MetadataSplitValuesAction,
                 MetadataJoinValuesAction, MetadataRemoveMatchingValuesAction,
                 MetadataReplaceMatchingValuesAction, MetadataNumberSelectedItemsAction,
                 MetadataKeepFirstCharactersAction, MetadataCaptureValuesAction,
                 MetadataNumberGroupedItemsAction, MetadataBlocklistFieldsAction,
                 MetadataAllowlistFieldsAction>;

struct MetadataTransformationChain {
    std::uint32_t schema_version{1U};
    std::string name;
    std::vector<MetadataTransformationAction> actions;

    friend bool operator==(const MetadataTransformationChain&,
                           const MetadataTransformationChain&) = default;
};

struct MetadataTransformationCellPreview {
    std::size_t item_index{0U};
    std::size_t last_action_index{0U};
    std::string canonical_field;
    std::string display_field;
    MetadataFieldMatchMode match_mode{MetadataFieldMatchMode::logical};
    // Missing is distinct from a present field containing one empty string.
    std::optional<std::vector<std::string>> before;
    std::optional<std::vector<std::string>> after;

    friend bool operator==(const MetadataTransformationCellPreview&,
                           const MetadataTransformationCellPreview&) = default;
};

struct MetadataTransformationPreview {
    MetadataTransformationChain chain;
    std::vector<std::size_t> item_indexes;
    std::vector<MetadataTransformationCellPreview> cells;
    std::size_t changed_item_count{0U};
    // Target cells whose final state equals their input state. These counts
    // let presentation explain an empty preview without retaining unchanged
    // values in the preview payload.
    std::size_t unchanged_present_cell_count{0U};
    std::size_t unchanged_missing_cell_count{0U};

    friend bool operator==(const MetadataTransformationPreview&,
                           const MetadataTransformationPreview&) = default;
};

struct MetadataTransformationLimits {
    std::size_t items{100'000U};
    std::size_t actions{256U};
    std::size_t addressed_cells{100'000U};
    std::size_t values_per_cell{16'384U};
    std::size_t total_preview_text_bytes{64U * 1'024U * 1'024U};
    std::size_t field_name_bytes{1'024U};
    std::size_t chain_name_bytes{1'024U};
    std::size_t action_text_bytes{1U * 1'024U * 1'024U};
    std::uint32_t maximum_character_count{1'000'000U};
};

// Validates all persisted chain data, including action payloads and compiled
// formatting expressions, without evaluating it against a selection.
[[nodiscard]] core::Result<void>
validate_metadata_transformation_chain(const MetadataTransformationChain& chain,
                                       const MetadataTransformationLimits& limits = {});

// Evaluates an ordered declarative chain against the current staged draft.
// Later actions see earlier results. The returned preview is still pure data;
// callers must explicitly stage its changed cells before ordinary write-plan
// revalidation and Apply can observe them.
[[nodiscard]] core::Result<MetadataTransformationPreview> plan_metadata_transformation(
    const StagedMetadataSelection& selection, const StagedMetadataPatchSet& current_draft,
    std::span<const std::size_t> item_indexes, MetadataTransformationChain chain,
    const core::CancellationToken& cancellation = {},
    const MetadataTransformationLimits& limits = {});

} // namespace trackknife::metadata
