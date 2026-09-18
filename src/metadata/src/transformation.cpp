// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/transformation.hpp"

#include "trackknife/core/unicode.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/metadata/flac_mapping.hpp"
#include "trackknife/titleformat/evaluator.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace trackknife::metadata {
namespace {

using Values = std::vector<std::string>;
using OptionalValues = std::optional<Values>;
using WorkingDocument = std::unordered_map<std::string, Values>;

struct WorkingNativeField {
    std::string canonical_field;
    Values values;
    FieldProvenance provenance{FieldProvenance::embedded};
    std::size_t order{0U};
};

using WorkingNativeDocument = std::unordered_map<std::string, WorkingNativeField>;

struct PreparedCaptureField {
    std::string canonical_field;
    std::string exact_native_field;
    std::string native_canonical_field;
    std::string display_field;
    MetadataFieldMatchMode match_mode{MetadataFieldMatchMode::logical};
};

struct PreparedAction {
    const MetadataTransformationAction* action{nullptr};
    std::string canonical_field;
    std::string exact_native_field;
    std::string canonical_source_field;
    std::string exact_native_source_field;
    std::string display_field;
    std::optional<titleformat::Program> program;
    std::optional<CapturePatternProgram> capture_program;
    std::vector<PreparedCaptureField> capture_fields;
    std::vector<std::string> filter_fields;
};

struct Target {
    std::string canonical_field;
    std::string exact_native_field;
    std::string display_field;
    MetadataFieldMatchMode match_mode{MetadataFieldMatchMode::logical};
    std::size_t last_action_index{0U};
};

struct PreparedChain {
    std::vector<PreparedAction> actions;
    std::vector<Target> targets;
};

[[nodiscard]] core::Error transformation_error(const core::ErrorCode code, std::string message,
                                               const std::optional<std::size_t> action = {},
                                               const std::optional<std::size_t> item = {}) {
    core::Error error{.code = code, .message = std::move(message), .context = {}};
    if (action) {
        error.context.push_back({.key = "action", .value = std::to_string(*action)});
    }
    if (item) {
        error.context.push_back({.key = "item", .value = std::to_string(*item)});
    }
    return error;
}

[[nodiscard]] core::Result<void> validate_utf8(const std::string_view value,
                                               const std::string_view description,
                                               const std::optional<std::size_t> action = {}) {
    if (const auto valid = core::unicodeCodePointCount(value); !valid) {
        return std::unexpected(
            transformation_error(core::ErrorCode::invalid_argument,
                                 std::string{description} + " must be valid UTF-8", action));
    }
    return {};
}

[[nodiscard]] const std::string& target_field(const MetadataTransformationAction& action) {
    return std::visit(
        [](const auto& typed) -> const std::string& {
            using Action = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Action, MetadataCaptureValuesAction>) {
                static const std::string empty;
                return empty;
            } else if constexpr (std::is_same_v<Action, MetadataBlocklistFieldsAction> ||
                                 std::is_same_v<Action, MetadataAllowlistFieldsAction>) {
                static const std::string empty;
                return empty;
            } else {
                return typed.target_field;
            }
        },
        action);
}

[[nodiscard]] MetadataFieldMatchMode field_match_mode(const MetadataTransformationAction& action) {
    return std::visit(
        [](const auto& typed) {
            using Action = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Action, MetadataRemoveFieldAction> ||
                          std::is_same_v<Action, MetadataRemoveFieldIfAction>) {
                return typed.match_mode;
            }
            return MetadataFieldMatchMode::logical;
        },
        action);
}

[[nodiscard]] OptionalValues values_for(const WorkingDocument& document,
                                        const std::string& canonical_field) {
    const auto found = document.find(canonical_field);
    return found == document.end() ? std::nullopt : OptionalValues{found->second};
}

[[nodiscard]] OptionalValues values_for(const WorkingDocument& document,
                                        const WorkingNativeDocument& native_document,
                                        const Target& target) {
    if (target.match_mode == MetadataFieldMatchMode::logical) {
        return values_for(document, target.canonical_field);
    }
    const auto found = native_document.find(target.exact_native_field);
    return found == native_document.end() ? std::nullopt : OptionalValues{found->second.values};
}

[[nodiscard]] unsigned provenance_precedence(const FieldProvenance provenance) noexcept {
    switch (provenance) {
    case FieldProvenance::cached_snapshot:
        return 0U;
    case FieldProvenance::annotation:
        return 1U;
    case FieldProvenance::embedded:
    case FieldProvenance::stream:
        return 2U;
    case FieldProvenance::segment:
        return 3U;
    case FieldProvenance::sidecar:
        return 4U;
    }
    return 0U;
}

void rebuild_logical_field(WorkingDocument& document, const WorkingNativeDocument& native_document,
                           const std::string& canonical_field) {
    std::optional<unsigned> selected_precedence;
    Values values;
    std::vector<const WorkingNativeField*> fields;
    fields.reserve(native_document.size());
    for (const auto& [native_name, field] : native_document) {
        static_cast<void>(native_name);
        if (field.canonical_field != canonical_field) {
            continue;
        }
        fields.push_back(&field);
    }
    std::ranges::sort(fields, {}, &WorkingNativeField::order);
    for (const auto* field : fields) {
        const auto precedence = provenance_precedence(field->provenance);
        if (!selected_precedence || precedence > *selected_precedence) {
            selected_precedence = precedence;
            values = field->values;
        } else if (precedence == *selected_precedence) {
            values.insert(values.end(), field->values.begin(), field->values.end());
        }
    }
    if (selected_precedence) {
        document[canonical_field] = std::move(values);
    } else {
        document.erase(canonical_field);
    }
}

void apply_draft_to_native(WorkingNativeDocument& native_document, const StagedMetadataField& field,
                           const StagedMetadataPatch& patch) {
    const auto next_order = [&native_document] {
        std::size_t result = 0U;
        for (const auto& [name, existing] : native_document) {
            static_cast<void>(name);
            result = std::max(result, existing.order + 1U);
        }
        return result;
    }();
    if (field.exact_native_name) {
        if (patch.kind == StagedMetadataPatchKind::remove_field) {
            native_document.erase(*field.exact_native_name);
        } else {
            const auto existing = native_document.find(*field.exact_native_name);
            const auto native_canonical =
                existing == native_document.end()
                    ? canonicalize_native_field_name(*field.exact_native_name)
                    : existing->second.canonical_field;
            native_document[*field.exact_native_name] = WorkingNativeField{
                .canonical_field = native_canonical,
                .values = patch.values,
                .provenance = FieldProvenance::sidecar,
                .order = next_order,
            };
        }
        return;
    }
    std::erase_if(native_document, [&field](const auto& entry) {
        return entry.second.canonical_field == field.canonical_name;
    });
    if (patch.kind == StagedMetadataPatchKind::replace_values) {
        native_document[field.canonical_name] = WorkingNativeField{
            .canonical_field = field.canonical_name,
            .values = patch.values,
            .provenance = FieldProvenance::sidecar,
            .order = next_order,
        };
    }
}

[[nodiscard]] std::string trim_ascii(std::string value) {
    constexpr auto whitespace = std::string_view{" \t\n\r\f\v"};
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1U);
}

[[nodiscard]] Values split_exact(const std::string_view value, const std::string_view separator) {
    Values result;
    std::size_t start = 0U;
    while (true) {
        const auto found = value.find(separator, start);
        if (found == std::string_view::npos) {
            result.emplace_back(value.substr(start));
            return result;
        }
        result.emplace_back(value.substr(start, found - start));
        start = found + separator.size();
    }
}

class TransformationEvaluationContext final : public titleformat::EvaluationContext {
  public:
    explicit TransformationEvaluationContext(const WorkingDocument& document)
        : document_(document) {}

    [[nodiscard]] titleformat::FormatContextKind kind() const noexcept override {
        return titleformat::FormatContextKind::metadata_transformation;
    }

    [[nodiscard]] std::optional<std::string>
    resolveField(const std::string_view name) const override {
        const auto values = resolveMetadata(name);
        if (!values) {
            return std::nullopt;
        }
        std::string joined;
        for (std::size_t index = 0U; index < values->size(); ++index) {
            if (index > 0U) {
                joined += "; ";
            }
            joined += (*values)[index];
        }
        return joined;
    }

    [[nodiscard]] std::optional<MetadataValues>
    resolveMetadata(const std::string_view name) const override {
        const auto found = document_.find(canonicalize_field_name(name));
        return found == document_.end() ? std::nullopt : std::optional{found->second};
    }

  private:
    const WorkingDocument& document_;
};

[[nodiscard]] core::Result<std::string> transform_value(std::string value,
                                                        const MetadataValueTransformKind transform,
                                                        const std::size_t action_index,
                                                        const std::size_t item_index) {
    switch (transform) {
    case MetadataValueTransformKind::trim_ascii:
        return trim_ascii(std::move(value));
    case MetadataValueTransformKind::lowercase: {
        auto lowered = core::unicodeSimpleLower(value);
        if (!lowered) {
            auto error = std::move(lowered.error());
            error.context.push_back({.key = "action", .value = std::to_string(action_index)});
            error.context.push_back({.key = "item", .value = std::to_string(item_index)});
            return std::unexpected(std::move(error));
        }
        return lowered;
    }
    case MetadataValueTransformKind::uppercase: {
        auto uppered = core::unicodeSimpleUpper(value);
        if (!uppered) {
            auto error = std::move(uppered.error());
            error.context.push_back({.key = "action", .value = std::to_string(action_index)});
            error.context.push_back({.key = "item", .value = std::to_string(item_index)});
            return std::unexpected(std::move(error));
        }
        return uppered;
    }
    case MetadataValueTransformKind::capitalize_first: {
        auto first_end = core::unicodeByteOffset(value, 1U);
        if (!first_end) {
            auto error = std::move(first_end.error());
            error.context.push_back({.key = "action", .value = std::to_string(action_index)});
            error.context.push_back({.key = "item", .value = std::to_string(item_index)});
            return std::unexpected(std::move(error));
        }
        auto capitalized = core::unicodeSimpleUpper(std::string_view{value}.substr(0U, *first_end));
        if (!capitalized) {
            auto error = std::move(capitalized.error());
            error.context.push_back({.key = "action", .value = std::to_string(action_index)});
            error.context.push_back({.key = "item", .value = std::to_string(item_index)});
            return std::unexpected(std::move(error));
        }
        capitalized->append(value.substr(*first_end));
        return capitalized;
    }
    }
    return std::unexpected(transformation_error(core::ErrorCode::invariant,
                                                "unknown metadata value transformation",
                                                action_index, item_index));
}

[[nodiscard]] core::Result<std::string>
evaluate_action_program(const PreparedAction& prepared, const WorkingDocument& document,
                        const std::size_t action_index, const std::size_t item_index,
                        const core::CancellationToken& cancellation) {
    if (!prepared.program) {
        return std::unexpected(transformation_error(
            core::ErrorCode::invariant, "compiled metadata transformation expression is missing",
            action_index, item_index));
    }
    const TransformationEvaluationContext context{document};
    auto evaluated =
        titleformat::evaluate(*prepared.program, context,
                              titleformat::EvaluationOptions{.maximum_steps = 100'000U,
                                                             .maximum_output_bytes = 1024U * 1024U,
                                                             .maximum_expanded_results = 1U,
                                                             .cancellation = cancellation});
    if (!evaluated) {
        auto error = std::move(evaluated.error());
        error.context.push_back({.key = "action", .value = std::to_string(action_index)});
        error.context.push_back({.key = "item", .value = std::to_string(item_index)});
        return std::unexpected(std::move(error));
    }
    if (auto valid = validate_utf8(evaluated->text, "metadata transformation expression result",
                                   action_index);
        !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    return std::move(evaluated->text);
}

[[nodiscard]] std::optional<std::string> filename_capture_source(const std::string_view raw_path,
                                                                 const std::size_t parent_count) {
    const std::filesystem::path path{std::string{raw_path}};
    const auto filename = path.filename();
    if (filename.empty()) {
        return std::nullopt;
    }
    std::vector<std::string> components;
    components.reserve(parent_count + 1U);
    components.push_back(filename.stem().native());
    auto parent = path.parent_path();
    for (std::size_t index = 0U; index < parent_count; ++index) {
        const auto component = parent.filename();
        if (component.empty()) {
            return std::nullopt;
        }
        components.push_back(component.native());
        parent = parent.parent_path();
    }
    std::ranges::reverse(components);
    std::string result;
    for (std::size_t index = 0U; index < components.size(); ++index) {
        if (index > 0U) {
            result.push_back('/');
        }
        result += components[index];
    }
    return result;
}

[[nodiscard]] core::Result<void>
apply_action(const PreparedAction& prepared, WorkingDocument& document,
             WorkingNativeDocument& native_document, const std::size_t action_index,
             const std::size_t item_index, const std::size_t selection_position,
             std::map<std::string, std::uint64_t>& group_counters, const std::string_view raw_path,
             const core::CancellationToken& cancellation,
             const MetadataTransformationLimits& limits) {
    return std::visit(
        [&](const auto& action) -> core::Result<void> {
            using Action = std::decay_t<decltype(action)>;
            if constexpr (std::is_same_v<Action, MetadataSetValuesAction>) {
                document[prepared.canonical_field] = action.values;
            } else if constexpr (std::is_same_v<Action, MetadataAddValuesAction>) {
                auto& values = document[prepared.canonical_field];
                if (values.size() > limits.values_per_cell ||
                    action.values.size() > limits.values_per_cell - values.size()) {
                    return std::unexpected(transformation_error(
                        core::ErrorCode::limit_exceeded,
                        "metadata add-values transformation exceeds the per-cell value limit",
                        action_index, item_index));
                }
                values.insert(values.end(), action.values.begin(), action.values.end());
            } else if constexpr (std::is_same_v<Action, MetadataRemoveFieldAction>) {
                if (action.match_mode == MetadataFieldMatchMode::exact_native) {
                    native_document.erase(prepared.exact_native_field);
                    rebuild_logical_field(document, native_document, prepared.canonical_field);
                } else {
                    document.erase(prepared.canonical_field);
                }
            } else if constexpr (std::is_same_v<Action, MetadataRemoveFieldIfAction>) {
                auto condition = evaluate_action_program(prepared, document, action_index,
                                                         item_index, cancellation);
                if (!condition) {
                    return std::unexpected(std::move(condition.error()));
                }
                if (!condition->empty()) {
                    if (action.match_mode == MetadataFieldMatchMode::exact_native) {
                        native_document.erase(prepared.exact_native_field);
                        rebuild_logical_field(document, native_document, prepared.canonical_field);
                    } else {
                        document.erase(prepared.canonical_field);
                    }
                }
            } else if constexpr (std::is_same_v<Action, MetadataBlocklistFieldsAction> ||
                                 std::is_same_v<Action, MetadataAllowlistFieldsAction>) {
                const auto should_remove = [&](const std::string& canonical) {
                    const auto listed = std::ranges::find(prepared.filter_fields, canonical) !=
                                        prepared.filter_fields.end();
                    if constexpr (std::is_same_v<Action, MetadataBlocklistFieldsAction>) {
                        return listed;
                    }
                    return !listed;
                };
                std::erase_if(document,
                              [&](const auto& entry) { return should_remove(entry.first); });
                std::erase_if(native_document, [&](const auto& entry) {
                    return should_remove(entry.second.canonical_field);
                });
            } else if constexpr (std::is_same_v<Action, MetadataTransformValuesAction>) {
                const auto found = document.find(prepared.canonical_field);
                if (found == document.end()) {
                    return {};
                }
                Values transformed;
                transformed.reserve(found->second.size());
                for (auto value : found->second) {
                    auto result = transform_value(std::move(value), action.transform, action_index,
                                                  item_index);
                    if (!result) {
                        return std::unexpected(std::move(result.error()));
                    }
                    transformed.push_back(std::move(*result));
                }
                found->second = std::move(transformed);
            } else if constexpr (std::is_same_v<Action, MetadataCopyFieldAction>) {
                const auto source = document.find(prepared.canonical_source_field);
                if (source == document.end()) {
                    document.erase(prepared.canonical_field);
                } else {
                    document[prepared.canonical_field] = source->second;
                }
            } else if constexpr (std::is_same_v<Action, MetadataSplitValuesAction>) {
                const auto found = document.find(prepared.canonical_field);
                if (found == document.end()) {
                    return {};
                }
                Values split_values;
                for (const auto& value : found->second) {
                    auto components = split_exact(value, action.separator);
                    if (split_values.size() > limits.values_per_cell ||
                        components.size() > limits.values_per_cell - split_values.size()) {
                        return std::unexpected(transformation_error(
                            core::ErrorCode::limit_exceeded,
                            "metadata split transformation exceeds the per-cell value limit",
                            action_index, item_index));
                    }
                    split_values.insert(split_values.end(),
                                        std::make_move_iterator(components.begin()),
                                        std::make_move_iterator(components.end()));
                }
                found->second = std::move(split_values);
            } else if constexpr (std::is_same_v<Action, MetadataJoinValuesAction>) {
                const auto found = document.find(prepared.canonical_field);
                if (found == document.end()) {
                    return {};
                }
                std::string joined;
                for (std::size_t index = 0U; index < found->second.size(); ++index) {
                    if (index > 0U) {
                        joined += action.separator;
                    }
                    joined += found->second[index];
                }
                found->second = {std::move(joined)};
            } else if constexpr (std::is_same_v<Action, MetadataRemoveMatchingValuesAction>) {
                const auto found = document.find(prepared.canonical_field);
                if (found == document.end()) {
                    return {};
                }
                std::erase(found->second, action.match);
                if (found->second.empty()) {
                    document.erase(found);
                }
            } else if constexpr (std::is_same_v<Action, MetadataReplaceMatchingValuesAction>) {
                const auto found = document.find(prepared.canonical_field);
                if (found == document.end()) {
                    return {};
                }
                Values replaced;
                for (const auto& value : found->second) {
                    const auto additions =
                        value == action.match ? action.replacement_values.size() : std::size_t{1U};
                    if (replaced.size() > limits.values_per_cell ||
                        additions > limits.values_per_cell - replaced.size()) {
                        return std::unexpected(transformation_error(
                            core::ErrorCode::limit_exceeded,
                            "metadata replace-matching transformation exceeds the per-cell "
                            "value limit",
                            action_index, item_index));
                    }
                    if (value == action.match) {
                        replaced.insert(replaced.end(), action.replacement_values.begin(),
                                        action.replacement_values.end());
                    } else {
                        replaced.push_back(value);
                    }
                }
                found->second = std::move(replaced);
            } else if constexpr (std::is_same_v<Action, MetadataNumberGroupedItemsAction>) {
                auto group = evaluate_action_program(prepared, document, action_index, item_index,
                                                     cancellation);
                if (!group) {
                    return std::unexpected(std::move(group.error()));
                }
                const auto position = group_counters[*group]++;
                const auto number = static_cast<std::uint64_t>(action.start) + position;
                if (number > std::numeric_limits<std::uint32_t>::max()) {
                    return std::unexpected(transformation_error(
                        core::ErrorCode::limit_exceeded,
                        "metadata numbering exceeds the supported integer range", action_index,
                        item_index));
                }
                auto value = std::to_string(number);
                if (value.size() < action.padding) {
                    value.insert(0U, static_cast<std::size_t>(action.padding) - value.size(), '0');
                }
                document[prepared.canonical_field] = {std::move(value)};
            } else if constexpr (std::is_same_v<Action, MetadataNumberSelectedItemsAction>) {
                const auto number = static_cast<std::uint64_t>(action.start) + selection_position;
                if (number > std::numeric_limits<std::uint32_t>::max()) {
                    return std::unexpected(transformation_error(
                        core::ErrorCode::limit_exceeded,
                        "metadata numbering exceeds the supported integer range", action_index,
                        item_index));
                }
                auto value = std::to_string(number);
                if (value.size() < action.padding) {
                    value.insert(0U, static_cast<std::size_t>(action.padding) - value.size(), '0');
                }
                document[prepared.canonical_field] = {std::move(value)};
            } else if constexpr (std::is_same_v<Action, MetadataKeepFirstCharactersAction>) {
                const auto found = document.find(prepared.canonical_field);
                if (found == document.end()) {
                    return {};
                }
                for (auto& value : found->second) {
                    auto end = core::unicodeByteOffset(value, action.character_count);
                    if (!end) {
                        auto error = std::move(end.error());
                        error.context.push_back(
                            {.key = "action", .value = std::to_string(action_index)});
                        error.context.push_back(
                            {.key = "item", .value = std::to_string(item_index)});
                        return std::unexpected(std::move(error));
                    }
                    value.resize(*end);
                }
            } else if constexpr (std::is_same_v<Action, MetadataFormatValueAction>) {
                auto evaluated = evaluate_action_program(prepared, document, action_index,
                                                         item_index, cancellation);
                if (!evaluated) {
                    return std::unexpected(std::move(evaluated.error()));
                }
                document[prepared.canonical_field] = {std::move(*evaluated)};
            } else if constexpr (std::is_same_v<Action, MetadataCaptureValuesAction>) {
                if (!prepared.capture_program) {
                    return std::unexpected(transformation_error(
                        core::ErrorCode::invariant, "compiled capture pattern is missing",
                        action_index, item_index));
                }
                std::vector<std::string> sources;
                switch (action.source_kind) {
                case MetadataCaptureSourceKind::filename: {
                    auto source = filename_capture_source(
                        raw_path, prepared.capture_program->literal_path_separator_count);
                    if (!source) {
                        return std::unexpected(transformation_error(
                            core::ErrorCode::invalid_argument,
                            "capture filename source does not have enough parent components",
                            action_index, item_index));
                    }
                    sources.push_back(std::move(*source));
                    break;
                }
                case MetadataCaptureSourceKind::full_path:
                    sources.emplace_back(raw_path);
                    break;
                case MetadataCaptureSourceKind::formatted: {
                    auto source = evaluate_action_program(prepared, document, action_index,
                                                          item_index, cancellation);
                    if (!source) {
                        return std::unexpected(std::move(source.error()));
                    }
                    sources.push_back(std::move(*source));
                    break;
                }
                case MetadataCaptureSourceKind::field: {
                    if (!prepared.exact_native_source_field.empty()) {
                        const auto source =
                            native_document.find(prepared.exact_native_source_field);
                        if (source == native_document.end()) {
                            return std::unexpected(transformation_error(
                                core::ErrorCode::invalid_argument,
                                "capture source field is missing in the working metadata",
                                action_index, item_index));
                        }
                        sources = source->second.values;
                        break;
                    }
                    const auto logical_source = document.find(prepared.canonical_source_field);
                    if (logical_source == document.end()) {
                        return std::unexpected(transformation_error(
                            core::ErrorCode::invalid_argument,
                            "capture source field is missing in the working metadata", action_index,
                            item_index));
                    }
                    sources = logical_source->second;
                    break;
                }
                }
                if (sources.empty()) {
                    return std::unexpected(transformation_error(
                        core::ErrorCode::invalid_argument,
                        "capture source field has no ordered values", action_index, item_index));
                }

                struct CapturedTarget {
                    const PreparedCaptureField* field{nullptr};
                    Values values;
                };
                std::vector<CapturedTarget> captured;
                captured.reserve(prepared.capture_fields.size());
                for (const auto& source : sources) {
                    auto matched = match_capture_pattern(*prepared.capture_program, source);
                    if (!matched) {
                        auto error = std::move(matched.error());
                        error.context.push_back(
                            {.key = "action", .value = std::to_string(action_index)});
                        error.context.push_back(
                            {.key = "item", .value = std::to_string(item_index)});
                        return std::unexpected(std::move(error));
                    }
                    if (matched->kind == CapturePatternMatchKind::unmatched) {
                        return std::unexpected(transformation_error(
                            core::ErrorCode::invalid_argument,
                            "capture pattern does not match the complete source", action_index,
                            item_index));
                    }
                    if (matched->kind == CapturePatternMatchKind::ambiguous) {
                        return std::unexpected(
                            transformation_error(core::ErrorCode::conflict,
                                                 "capture pattern has more than one possible parse",
                                                 action_index, item_index));
                    }
                    if (matched->values.size() != prepared.capture_fields.size()) {
                        return std::unexpected(transformation_error(
                            core::ErrorCode::invariant,
                            "capture pattern result does not match its prepared targets",
                            action_index, item_index));
                    }
                    for (std::size_t index = 0U; index < matched->values.size(); ++index) {
                        const auto& field = prepared.capture_fields[index];
                        const auto found =
                            std::ranges::find_if(captured, [&field](const auto& item) {
                                return item.field->match_mode == field.match_mode &&
                                       item.field->canonical_field == field.canonical_field &&
                                       item.field->exact_native_field == field.exact_native_field;
                            });
                        auto* target = found == captured.end()
                                           ? &captured.emplace_back(
                                                 CapturedTarget{.field = &field, .values = {}})
                                           : &*found;
                        if (target->values.size() >= limits.values_per_cell) {
                            return std::unexpected(transformation_error(
                                core::ErrorCode::limit_exceeded,
                                "metadata capture exceeds the per-cell value limit", action_index,
                                item_index));
                        }
                        target->values.push_back(std::move(matched->values[index].value));
                    }
                }
                for (auto& target : captured) {
                    if (target.field->match_mode == MetadataFieldMatchMode::logical) {
                        document[target.field->canonical_field] = std::move(target.values);
                        continue;
                    }
                    std::size_t next_order = 0U;
                    for (const auto& [name, existing] : native_document) {
                        static_cast<void>(name);
                        next_order = std::max(next_order, existing.order + 1U);
                    }
                    native_document[target.field->exact_native_field] = WorkingNativeField{
                        .canonical_field = target.field->native_canonical_field,
                        .values = std::move(target.values),
                        .provenance = FieldProvenance::sidecar,
                        .order = next_order,
                    };
                }
            }
            return {};
        },
        *prepared.action);
}

[[nodiscard]] core::Result<PreparedChain>
prepare_chain(const MetadataTransformationChain& chain,
              const MetadataTransformationLimits& limits) {
    if (chain.schema_version != 1U || chain.actions.empty() ||
        chain.actions.size() > limits.actions || chain.name.size() > limits.chain_name_bytes) {
        return std::unexpected(transformation_error(
            core::ErrorCode::invalid_argument,
            "metadata transformation requires schema 1 and a bounded non-empty action chain"));
    }
    if (auto valid = validate_utf8(chain.name, "metadata transformation name"); !valid) {
        return std::unexpected(std::move(valid.error()));
    }

    PreparedChain result;
    result.actions.reserve(chain.actions.size());
    result.targets.reserve(chain.actions.size());
    const auto add_target = [&result](std::string canonical, std::string exact_native,
                                      std::string display, const MetadataFieldMatchMode match_mode,
                                      const std::size_t action_index) -> core::Result<void> {
        if (std::ranges::any_of(result.targets, [&](const auto& candidate) {
                if (candidate.canonical_field != canonical || candidate.match_mode == match_mode) {
                    return false;
                }
                const auto& exact_name = match_mode == MetadataFieldMatchMode::exact_native
                                             ? exact_native
                                             : candidate.exact_native_field;
                const auto identity = resolve_text_property_identity(exact_name);
                return identity.conventional && identity.canonical_name == canonical;
            })) {
            return std::unexpected(transformation_error(
                core::ErrorCode::invalid_argument,
                "metadata transformation cannot mix logical and exact-native actions for one "
                "field",
                action_index));
        }
        const auto existing = std::ranges::find_if(result.targets, [&](const auto& candidate) {
            return candidate.match_mode == match_mode && candidate.canonical_field == canonical &&
                   candidate.exact_native_field == exact_native;
        });
        if (existing == result.targets.end()) {
            result.targets.push_back(Target{.canonical_field = std::move(canonical),
                                            .exact_native_field = std::move(exact_native),
                                            .display_field = std::move(display),
                                            .match_mode = match_mode,
                                            .last_action_index = action_index});
        } else {
            existing->last_action_index = action_index;
        }
        return {};
    };
    for (std::size_t action_index = 0U; action_index < chain.actions.size(); ++action_index) {
        const auto& action = chain.actions[action_index];
        const auto* blocklist = std::get_if<MetadataBlocklistFieldsAction>(&action);
        const auto* allowlist = std::get_if<MetadataAllowlistFieldsAction>(&action);
        if (blocklist != nullptr || allowlist != nullptr) {
            const auto& fields = blocklist != nullptr ? blocklist->fields : allowlist->fields;
            if (fields.empty() || fields.size() > limits.actions) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::invalid_argument,
                    "metadata field filter requires a bounded non-empty field list", action_index));
            }
            PreparedAction prepared_action{
                .action = &action,
                .canonical_field = {},
                .exact_native_field = {},
                .canonical_source_field = {},
                .exact_native_source_field = {},
                .display_field = {},
                .program = std::nullopt,
                .capture_program = std::nullopt,
                .capture_fields = {},
                .filter_fields = {},
            };
            prepared_action.filter_fields.reserve(fields.size());
            for (const auto& field : fields) {
                const auto canonical = canonicalize_field_name(field);
                if (field.empty() || field.size() > limits.field_name_bytes || canonical.empty()) {
                    return std::unexpected(transformation_error(
                        core::ErrorCode::invalid_argument,
                        "metadata field filter contains an empty or oversized field name",
                        action_index));
                }
                if (auto valid = validate_utf8(field, "metadata field filter name", action_index);
                    !valid) {
                    return std::unexpected(std::move(valid.error()));
                }
                if (std::ranges::find(prepared_action.filter_fields, canonical) ==
                    prepared_action.filter_fields.end()) {
                    prepared_action.filter_fields.push_back(canonical);
                }
            }
            result.actions.push_back(std::move(prepared_action));
            continue;
        }
        if (const auto* capture = std::get_if<MetadataCaptureValuesAction>(&action)) {
            if (capture->source_kind != MetadataCaptureSourceKind::filename &&
                capture->source_kind != MetadataCaptureSourceKind::full_path &&
                capture->source_kind != MetadataCaptureSourceKind::formatted &&
                capture->source_kind != MetadataCaptureSourceKind::field) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::invalid_argument,
                    "metadata capture uses an unknown source kind", action_index));
            }
            if ((capture->source_kind == MetadataCaptureSourceKind::filename ||
                 capture->source_kind == MetadataCaptureSourceKind::full_path) &&
                !capture->source.empty()) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::invalid_argument,
                    "filename and full-path capture sources cannot have a source argument",
                    action_index));
            }
            if ((capture->source_kind == MetadataCaptureSourceKind::formatted ||
                 capture->source_kind == MetadataCaptureSourceKind::field) &&
                (capture->source.empty() || capture->source.size() > limits.action_text_bytes)) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::invalid_argument,
                    "formatted and field captures require a bounded source argument",
                    action_index));
            }
            if (auto valid =
                    validate_utf8(capture->source, "metadata capture source", action_index);
                !valid) {
                return std::unexpected(std::move(valid.error()));
            }
            auto capture_program = compile_capture_pattern(
                capture->pattern, capture->dialect,
                CapturePatternLimits{.pattern_bytes = limits.action_text_bytes,
                                     .source_bytes = limits.action_text_bytes,
                                     .tokens = limits.actions,
                                     .captures = limits.actions,
                                     .match_steps = 4'000'000U});
            if (!capture_program) {
                auto error = std::move(capture_program.error());
                error.context.push_back({.key = "action", .value = std::to_string(action_index)});
                return std::unexpected(std::move(error));
            }
            PreparedAction prepared_action{
                .action = &action,
                .canonical_field = {},
                .exact_native_field = {},
                .canonical_source_field = {},
                .exact_native_source_field = {},
                .display_field = {},
                .program = std::nullopt,
                .capture_program = std::move(*capture_program),
                .capture_fields = {},
                .filter_fields = {},
            };
            prepared_action.capture_fields.reserve(
                prepared_action.capture_program->named_capture_count);
            for (const auto& token : prepared_action.capture_program->tokens) {
                if (token.kind != CapturePatternTokenKind::field) {
                    continue;
                }
                const auto identity = resolve_text_property_identity(token.text);
                const auto match_mode = identity.conventional
                                            ? MetadataFieldMatchMode::logical
                                            : MetadataFieldMatchMode::exact_native;
                const auto canonical = identity.conventional ? identity.canonical_name
                                                             : canonicalize_field_name(token.text);
                const auto exact_native = identity.conventional
                                              ? std::string{}
                                              : canonicalize_native_field_name(token.text);
                if (token.text.size() > limits.field_name_bytes || canonical.empty()) {
                    return std::unexpected(transformation_error(
                        core::ErrorCode::invalid_argument,
                        "metadata capture target field is empty or exceeds its byte limit",
                        action_index));
                }
                prepared_action.capture_fields.push_back(PreparedCaptureField{
                    .canonical_field = canonical,
                    .exact_native_field = exact_native,
                    .native_canonical_field = identity.canonical_name,
                    .display_field = token.text,
                    .match_mode = match_mode,
                });
                if (auto added =
                        add_target(canonical, exact_native, token.text, match_mode, action_index);
                    !added) {
                    return std::unexpected(std::move(added.error()));
                }
            }
            if (capture->source_kind == MetadataCaptureSourceKind::field) {
                const auto identity = resolve_text_property_identity(capture->source);
                prepared_action.canonical_source_field = canonicalize_field_name(capture->source);
                if (!identity.conventional) {
                    prepared_action.exact_native_source_field =
                        canonicalize_native_field_name(capture->source);
                }
                if (prepared_action.canonical_source_field.empty() ||
                    capture->source.size() > limits.field_name_bytes) {
                    return std::unexpected(transformation_error(
                        core::ErrorCode::invalid_argument,
                        "metadata capture source field is empty or exceeds its byte limit",
                        action_index));
                }
            } else if (capture->source_kind == MetadataCaptureSourceKind::formatted) {
                titleformat::CompileOptions options;
                options.context = titleformat::FormatContextKind::metadata_transformation;
                auto compiled = titleformat::compile(capture->source, options);
                if (!compiled.isValid()) {
                    const auto message = !compiled.parse_diagnostics.empty()
                                             ? compiled.parse_diagnostics.front().message
                                             : compiled.diagnostics.front().message;
                    return std::unexpected(transformation_error(
                        core::ErrorCode::invalid_argument,
                        "metadata capture source expression is invalid: " + message, action_index));
                }
                prepared_action.program = std::move(*compiled.program);
            }
            result.actions.push_back(std::move(prepared_action));
            continue;
        }
        const auto& target = target_field(action);
        const auto canonical = canonicalize_field_name(target);
        const auto match_mode = field_match_mode(action);
        if (match_mode != MetadataFieldMatchMode::logical &&
            match_mode != MetadataFieldMatchMode::exact_native) {
            return std::unexpected(transformation_error(
                core::ErrorCode::invalid_argument,
                "metadata transformation uses an unknown field-match mode", action_index));
        }
        const auto exact_native = match_mode == MetadataFieldMatchMode::exact_native
                                      ? canonicalize_native_field_name(target)
                                      : std::string{};
        if (target.empty() || target.size() > limits.field_name_bytes || canonical.empty()) {
            return std::unexpected(transformation_error(
                core::ErrorCode::invalid_argument,
                "metadata transformation target field is empty or exceeds its byte limit",
                action_index));
        }
        if (auto valid =
                validate_utf8(target, "metadata transformation target field", action_index);
            !valid) {
            return std::unexpected(std::move(valid.error()));
        }
        PreparedAction prepared_action{
            .action = &action,
            .canonical_field = canonical,
            .exact_native_field = exact_native,
            .canonical_source_field = {},
            .exact_native_source_field = {},
            .display_field = target,
            .program = std::nullopt,
            .capture_program = std::nullopt,
            .capture_fields = {},
            .filter_fields = {},
        };
        const auto validate_values = [&](const std::vector<std::string>& values,
                                         const std::string_view description) -> core::Result<void> {
            if (values.empty() || values.size() > limits.values_per_cell) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::invalid_argument,
                    std::string{description} + " requires a bounded non-empty value list",
                    action_index));
            }
            for (const auto& value : values) {
                if (value.size() > limits.action_text_bytes) {
                    return std::unexpected(transformation_error(
                        core::ErrorCode::limit_exceeded,
                        "metadata transformation literal value exceeds its byte limit",
                        action_index));
                }
                if (auto valid =
                        validate_utf8(value, "metadata transformation literal value", action_index);
                    !valid) {
                    return std::unexpected(std::move(valid.error()));
                }
            }
            return {};
        };
        if (const auto* set = std::get_if<MetadataSetValuesAction>(&action)) {
            if (auto valid = validate_values(set->values, "metadata set-values transformation");
                !valid) {
                return std::unexpected(std::move(valid.error()));
            }
        } else if (const auto* add = std::get_if<MetadataAddValuesAction>(&action)) {
            if (auto valid = validate_values(add->values, "metadata add-values transformation");
                !valid) {
                return std::unexpected(std::move(valid.error()));
            }
        } else if (const auto* copy = std::get_if<MetadataCopyFieldAction>(&action)) {
            prepared_action.canonical_source_field = canonicalize_field_name(copy->source_field);
            if (copy->source_field.empty() || copy->source_field.size() > limits.field_name_bytes ||
                prepared_action.canonical_source_field.empty()) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::invalid_argument,
                    "metadata copy transformation source field is empty or exceeds its byte "
                    "limit",
                    action_index));
            }
            if (auto valid = validate_utf8(copy->source_field,
                                           "metadata transformation source field", action_index);
                !valid) {
                return std::unexpected(std::move(valid.error()));
            }
        } else if (const auto* split = std::get_if<MetadataSplitValuesAction>(&action)) {
            if (split->separator.empty() || split->separator.size() > limits.action_text_bytes) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::invalid_argument,
                    "metadata split transformation requires a bounded non-empty separator",
                    action_index));
            }
            if (auto valid = validate_utf8(split->separator,
                                           "metadata transformation split separator", action_index);
                !valid) {
                return std::unexpected(std::move(valid.error()));
            }
        } else if (const auto* join = std::get_if<MetadataJoinValuesAction>(&action)) {
            if (join->separator.size() > limits.action_text_bytes) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::limit_exceeded,
                    "metadata join transformation separator exceeds its byte limit", action_index));
            }
            if (auto valid = validate_utf8(join->separator,
                                           "metadata transformation join separator", action_index);
                !valid) {
                return std::unexpected(std::move(valid.error()));
            }
        } else if (const auto* remove = std::get_if<MetadataRemoveMatchingValuesAction>(&action)) {
            if (remove->match.size() > limits.action_text_bytes) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::limit_exceeded,
                    "metadata remove-matching value exceeds its byte limit", action_index));
            }
            if (auto valid = validate_utf8(remove->match, "metadata remove-matching exact value",
                                           action_index);
                !valid) {
                return std::unexpected(std::move(valid.error()));
            }
        } else if (const auto* replace =
                       std::get_if<MetadataReplaceMatchingValuesAction>(&action)) {
            if (replace->match.size() > limits.action_text_bytes) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::limit_exceeded,
                    "metadata replace-matching value exceeds its byte limit", action_index));
            }
            if (auto valid = validate_utf8(replace->match, "metadata replace-matching exact value",
                                           action_index);
                !valid) {
                return std::unexpected(std::move(valid.error()));
            }
            if (auto valid = validate_values(replace->replacement_values,
                                             "metadata replace-matching transformation");
                !valid) {
                return std::unexpected(std::move(valid.error()));
            }
        } else if (const auto* grouped = std::get_if<MetadataNumberGroupedItemsAction>(&action)) {
            constexpr auto maximum_start = std::uint32_t{1'000'000'000U};
            constexpr auto maximum_padding = std::uint32_t{32U};
            if (grouped->start == 0U || grouped->start > maximum_start ||
                grouped->padding > maximum_padding) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::invalid_argument,
                    "metadata numbering requires a start from 1 to 1000000000 and padding from "
                    "0 to 32",
                    action_index));
            }
            if (grouped->dialect != titleformat::DialectVersion{}) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::unsupported,
                    "grouped numbering understands only tkfmt-1 expressions", action_index));
            }
            if (grouped->group_expression.empty() ||
                grouped->group_expression.size() > limits.action_text_bytes) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::invalid_argument,
                    "grouped numbering needs a nonempty bounded group expression", action_index));
            }
            titleformat::CompileOptions options;
            options.context = titleformat::FormatContextKind::metadata_transformation;
            options.dialect = grouped->dialect;
            auto compiled = titleformat::compile(grouped->group_expression, options);
            if (!compiled.isValid()) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::invalid_argument,
                    "grouped numbering group expression does not compile as tkfmt-1",
                    action_index));
            }
            prepared_action.program = std::move(*compiled.program);
        } else if (const auto* number = std::get_if<MetadataNumberSelectedItemsAction>(&action)) {
            constexpr auto maximum_start = std::uint32_t{1'000'000'000U};
            constexpr auto maximum_padding = std::uint32_t{32U};
            if (number->start == 0U || number->start > maximum_start ||
                number->padding > maximum_padding) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::invalid_argument,
                    "metadata numbering requires a start from 1 to 1000000000 and padding from "
                    "0 to 32",
                    action_index));
            }
        } else if (const auto* keep = std::get_if<MetadataKeepFirstCharactersAction>(&action)) {
            if (keep->character_count == 0U ||
                keep->character_count > limits.maximum_character_count) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::invalid_argument,
                    "metadata keep-first transformation requires a character count from 1 to " +
                        std::to_string(limits.maximum_character_count),
                    action_index));
            }
        } else if (const auto* conditional = std::get_if<MetadataRemoveFieldIfAction>(&action)) {
            const titleformat::DialectVersion current_dialect;
            if (conditional->dialect != current_dialect) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::invalid_argument,
                    "metadata conditional-remove expression uses an unsupported dialect or "
                    "compiler schema",
                    action_index));
            }
            if (conditional->condition.empty() ||
                conditional->condition.size() > limits.action_text_bytes) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::invalid_argument,
                    "metadata conditional remove requires a bounded non-empty condition",
                    action_index));
            }
            if (auto valid =
                    validate_utf8(conditional->condition,
                                  "metadata conditional-remove expression source", action_index);
                !valid) {
                return std::unexpected(std::move(valid.error()));
            }
            titleformat::CompileOptions options;
            options.context = titleformat::FormatContextKind::metadata_transformation;
            options.dialect = conditional->dialect;
            auto compiled = titleformat::compile(conditional->condition, options);
            if (!compiled.isValid()) {
                const auto message = !compiled.parse_diagnostics.empty()
                                         ? compiled.parse_diagnostics.front().message
                                         : compiled.diagnostics.front().message;
                return std::unexpected(transformation_error(
                    core::ErrorCode::invalid_argument,
                    "metadata conditional-remove expression is invalid: " + message, action_index));
            }
            prepared_action.program = std::move(*compiled.program);
        } else if (const auto* format = std::get_if<MetadataFormatValueAction>(&action)) {
            const titleformat::DialectVersion current_dialect;
            if (format->dialect != current_dialect) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::invalid_argument,
                    "metadata transformation expression uses an unsupported dialect or compiler "
                    "schema",
                    action_index));
            }
            if (format->source.size() > limits.action_text_bytes) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::limit_exceeded,
                    "metadata transformation expression exceeds its byte limit", action_index));
            }
            if (auto valid = validate_utf8(
                    format->source, "metadata transformation expression source", action_index);
                !valid) {
                return std::unexpected(std::move(valid.error()));
            }
            titleformat::CompileOptions options;
            options.context = titleformat::FormatContextKind::metadata_transformation;
            options.dialect = format->dialect;
            auto compiled = titleformat::compile(format->source, options);
            if (!compiled.isValid()) {
                const auto message = !compiled.parse_diagnostics.empty()
                                         ? compiled.parse_diagnostics.front().message
                                         : compiled.diagnostics.front().message;
                return std::unexpected(transformation_error(
                    core::ErrorCode::invalid_argument,
                    "metadata transformation expression is invalid: " + message, action_index));
            }
            prepared_action.program = std::move(*compiled.program);
        }
        result.actions.push_back(std::move(prepared_action));
        if (auto added = add_target(canonical, exact_native, target, match_mode, action_index);
            !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return result;
}

} // namespace

core::Result<void>
validate_metadata_transformation_chain(const MetadataTransformationChain& chain,
                                       const MetadataTransformationLimits& limits) {
    auto prepared = prepare_chain(chain, limits);
    if (!prepared) {
        return std::unexpected(std::move(prepared.error()));
    }
    return {};
}

core::Result<MetadataTransformationPreview> plan_metadata_transformation(
    const StagedMetadataSelection& selection, const StagedMetadataPatchSet& current_draft,
    const std::span<const std::size_t> item_indexes, MetadataTransformationChain chain,
    const core::CancellationToken& cancellation, const MetadataTransformationLimits& limits) {
    auto prepared_chain = prepare_chain(chain, limits);
    if (!prepared_chain) {
        return std::unexpected(std::move(prepared_chain.error()));
    }
    if (item_indexes.empty() || item_indexes.size() > limits.items) {
        return std::unexpected(transformation_error(
            core::ErrorCode::invalid_argument,
            "metadata transformation requires a bounded non-empty item selection"));
    }
    for (std::size_t position = 0U; position < item_indexes.size(); ++position) {
        if (item_indexes[position] >= selection.item_count() ||
            (position > 0U && item_indexes[position - 1U] >= item_indexes[position])) {
            return std::unexpected(transformation_error(
                core::ErrorCode::invalid_argument,
                "metadata transformation items must be in-range, sorted, and unique"));
        }
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(transformation_error(core::ErrorCode::cancelled,
                                                    "metadata transformation was cancelled"));
    }

    // Field filters address the complete selected field inventory. Register
    // those targets here because validation intentionally has no selection.
    for (std::size_t action_index = 0U; action_index < prepared_chain->actions.size();
         ++action_index) {
        const auto& prepared_action = prepared_chain->actions[action_index];
        const auto blocklist =
            std::holds_alternative<MetadataBlocklistFieldsAction>(*prepared_action.action);
        const auto allowlist =
            std::holds_alternative<MetadataAllowlistFieldsAction>(*prepared_action.action);
        if (!blocklist && !allowlist) {
            continue;
        }
        for (std::size_t field_index = 0U; field_index < selection.field_count(); ++field_index) {
            const auto& field = selection.field(field_index);
            const auto listed =
                std::ranges::find(prepared_action.filter_fields, field.canonical_name) !=
                prepared_action.filter_fields.end();
            if ((blocklist && !listed) || (allowlist && listed)) {
                continue;
            }
            const auto match_mode = field.exact_native_name ? MetadataFieldMatchMode::exact_native
                                                            : MetadataFieldMatchMode::logical;
            const auto exact = field.exact_native_name
                                   ? canonicalize_native_field_name(*field.exact_native_name)
                                   : std::string{};
            const auto existing = std::ranges::find_if(prepared_chain->targets, [&](const auto& t) {
                return t.canonical_field == field.canonical_name && t.exact_native_field == exact &&
                       t.match_mode == match_mode;
            });
            if (existing == prepared_chain->targets.end()) {
                prepared_chain->targets.push_back(Target{.canonical_field = field.canonical_name,
                                                         .exact_native_field = exact,
                                                         .display_field = field.display_name,
                                                         .match_mode = match_mode,
                                                         .last_action_index = action_index});
            } else {
                existing->last_action_index = action_index;
            }
        }
    }

    const auto& prepared = prepared_chain->actions;
    const auto& targets = prepared_chain->targets;
    if (!targets.empty() && item_indexes.size() > limits.addressed_cells / targets.size()) {
        return std::unexpected(
            transformation_error(core::ErrorCode::limit_exceeded,
                                 "metadata transformation addressed-cell limit was exceeded"));
    }

    std::vector<MetadataTransformationCellPreview> cells;
    cells.reserve(item_indexes.size() * targets.size());
    const auto draft_patches = current_draft.patches();
    std::size_t draft_position = 0U;
    std::size_t changed_item_count = 0U;
    std::size_t unchanged_present_cell_count = 0U;
    std::size_t unchanged_missing_cell_count = 0U;
    std::size_t preview_text_bytes = 0U;
    std::vector<std::map<std::string, std::uint64_t>> group_counters(prepared.size());
    for (std::size_t selection_position = 0U; selection_position < item_indexes.size();
         ++selection_position) {
        const auto item_index = item_indexes[selection_position];
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(transformation_error(core::ErrorCode::cancelled,
                                                        "metadata transformation was cancelled"));
        }
        while (draft_position < draft_patches.size() &&
               draft_patches[draft_position].item_index < item_index) {
            ++draft_position;
        }
        const auto item_draft_begin = draft_position;
        while (draft_position < draft_patches.size() &&
               draft_patches[draft_position].item_index == item_index) {
            ++draft_position;
        }
        const auto present_fields = selection.present_field_indexes(item_index);
        WorkingDocument document;
        document.reserve(present_fields.size() + (draft_position - item_draft_begin) +
                         targets.size());
        WorkingNativeDocument native_document;
        const auto native_fields = selection.source(item_index).baseline.effective_native_fields();
        native_document.reserve(native_fields.size() + (draft_position - item_draft_begin));
        for (std::size_t native_index = 0U; native_index < native_fields.size(); ++native_index) {
            const auto& field = native_fields[native_index];
            native_document.emplace(canonicalize_native_field_name(field.native_name),
                                    WorkingNativeField{.canonical_field = field.canonical_name,
                                                       .values = field.values,
                                                       .provenance = field.provenance,
                                                       .order = native_index});
        }
        std::size_t present_position = 0U;
        auto item_draft_position = item_draft_begin;
        std::vector<std::string> exact_draft_logical_fields;
        while (present_position < present_fields.size() || item_draft_position < draft_position) {
            const auto baseline_field = present_position < present_fields.size()
                                            ? present_fields[present_position]
                                            : std::numeric_limits<std::size_t>::max();
            const auto patch_field = item_draft_position < draft_position
                                         ? draft_patches[item_draft_position].field_index
                                         : std::numeric_limits<std::size_t>::max();
            if (patch_field <= baseline_field) {
                const auto& patch = draft_patches[item_draft_position++];
                const auto& staged_field = selection.field(patch.field_index);
                apply_draft_to_native(native_document, staged_field, patch);
                if (staged_field.exact_native_name) {
                    exact_draft_logical_fields.push_back(staged_field.canonical_name);
                }
                if (patch.kind == StagedMetadataPatchKind::replace_values) {
                    document.emplace(staged_field.canonical_name, patch.values);
                }
                if (patch_field == baseline_field) {
                    ++present_position;
                }
                continue;
            }
            const auto field_index = present_fields[present_position++];
            if (const auto* cell = selection.cell(item_index, field_index)) {
                const auto& staged_field = selection.field(field_index);
                if (!staged_field.exact_native_name) {
                    document.emplace(staged_field.canonical_name, cell->values);
                }
            }
        }
        std::ranges::sort(exact_draft_logical_fields);
        exact_draft_logical_fields.erase(
            std::unique(exact_draft_logical_fields.begin(), exact_draft_logical_fields.end()),
            exact_draft_logical_fields.end());
        for (const auto& canonical_field : exact_draft_logical_fields) {
            rebuild_logical_field(document, native_document, canonical_field);
        }
        std::vector<OptionalValues> before;
        before.reserve(targets.size());
        for (const auto& target : targets) {
            before.push_back(values_for(document, native_document, target));
        }
        for (std::size_t action_index = 0U; action_index < prepared.size(); ++action_index) {
            if (cancellation.is_cancellation_requested()) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::cancelled, "metadata transformation was cancelled"));
            }
            if (auto applied =
                    apply_action(prepared[action_index], document, native_document, action_index,
                                 item_index, selection_position, group_counters[action_index],
                                 selection.source(item_index).raw_path, cancellation, limits);
                !applied) {
                return std::unexpected(std::move(applied.error()));
            }
        }

        auto item_changed = false;
        for (std::size_t target_index = 0U; target_index < targets.size(); ++target_index) {
            auto after = values_for(document, native_document, targets[target_index]);
            if (before[target_index] == after) {
                if (before[target_index]) {
                    ++unchanged_present_cell_count;
                } else {
                    ++unchanged_missing_cell_count;
                }
                continue;
            }
            const auto account = [&preview_text_bytes, &limits](const OptionalValues& values) {
                if (!values) {
                    return true;
                }
                for (const auto& value : *values) {
                    if (value.size() >
                        limits.total_preview_text_bytes -
                            std::min(preview_text_bytes, limits.total_preview_text_bytes)) {
                        return false;
                    }
                    preview_text_bytes += value.size();
                }
                return true;
            };
            if (!account(before[target_index]) || !account(after)) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::limit_exceeded,
                    "metadata transformation preview text limit was exceeded"));
            }
            cells.push_back(MetadataTransformationCellPreview{
                .item_index = item_index,
                .last_action_index = targets[target_index].last_action_index,
                .canonical_field = targets[target_index].canonical_field,
                .display_field = targets[target_index].display_field,
                .match_mode = targets[target_index].match_mode,
                .before = std::move(before[target_index]),
                .after = std::move(after),
            });
            item_changed = true;
        }
        changed_item_count += item_changed ? 1U : 0U;
    }
    return MetadataTransformationPreview{
        .chain = std::move(chain),
        .item_indexes = {item_indexes.begin(), item_indexes.end()},
        .cells = std::move(cells),
        .changed_item_count = changed_item_count,
        .unchanged_present_cell_count = unchanged_present_cell_count,
        .unchanged_missing_cell_count = unchanged_missing_cell_count,
    };
}

} // namespace trackknife::metadata
