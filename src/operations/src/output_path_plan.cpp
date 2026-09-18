// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/operations/output_path_plan.hpp"

#include "trackknife/core/unicode.hpp"
#include "trackknife/titleformat/evaluator.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace trackknife::operations {
namespace {

[[nodiscard]] core::Error plan_error(const core::ErrorCode code, std::string message) {
    return core::Error{.code = code, .message = std::move(message), .context = {}};
}

[[nodiscard]] core::Result<void> validate_utf8(const std::string_view text,
                                               const std::string_view description) {
    if (const auto valid = core::unicodeCodePointCount(text); !valid) {
        return std::unexpected(plan_error(core::ErrorCode::invalid_argument,
                                          std::string{description} + " must be valid UTF-8"));
    }
    return {};
}

[[nodiscard]] std::string ascii_lower(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const auto character : text) {
        result.push_back(character >= 'A' && character <= 'Z'
                             ? static_cast<char>(character - 'A' + 'a')
                             : character);
    }
    return result;
}

[[nodiscard]] bool contains_nul(const std::string_view value) {
    return value.find('\0') != std::string_view::npos;
}

[[nodiscard]] bool is_normal_absolute_path(const std::string_view raw_path) {
    if (raw_path.empty() || contains_nul(raw_path)) {
        return false;
    }
    const std::filesystem::path path{raw_path};
    return path.is_absolute() && !path.filename().empty() && path == path.lexically_normal();
}

[[nodiscard]] bool is_normal_absolute_observation(const std::string_view raw_path) {
    if (raw_path.empty() || contains_nul(raw_path)) {
        return false;
    }
    const std::filesystem::path path{raw_path};
    return path.is_absolute() && path == path.lexically_normal();
}

[[nodiscard]] bool is_normal_absolute_root(const std::string_view raw_path) {
    if (raw_path.empty() || contains_nul(raw_path)) {
        return false;
    }
    const std::filesystem::path path{raw_path};
    return path.is_absolute() && path != path.root_path() && path == path.lexically_normal();
}

[[nodiscard]] std::filesystem::path operation_root_path(std::filesystem::path path) {
    if (path != path.root_path() && path.filename().empty()) {
        path = path.parent_path();
    }
    return path;
}

[[nodiscard]] core::Result<titleformat::Program>
compile_expression(const OutputLayoutProfile& profile, const std::string& source,
                   const std::string_view description) {
    titleformat::CompileOptions options;
    options.context = titleformat::FormatContextKind::path_generation;
    options.dialect = profile.dialect;
    auto compiled = titleformat::compile(source, options);
    if (!compiled.isValid()) {
        const auto message = !compiled.parse_diagnostics.empty()
                                 ? compiled.parse_diagnostics.front().message
                                 : compiled.diagnostics.front().message;
        return std::unexpected(plan_error(core::ErrorCode::invalid_argument,
                                          std::string{description} + " is invalid: " + message));
    }
    return std::move(*compiled.program);
}

class PathEvaluationContext final : public titleformat::EvaluationContext {
  public:
    PathEvaluationContext(const metadata::MetadataDocument& document, std::string_view source_path,
                          const std::optional<std::string>& target_extension)
        : document_(document), source_path_(source_path), target_extension_(target_extension) {}

    [[nodiscard]] titleformat::FormatContextKind kind() const noexcept override {
        return titleformat::FormatContextKind::path_generation;
    }

    [[nodiscard]] std::optional<std::string>
    resolveField(const std::string_view name) const override {
        const auto values = resolveMetadata(name);
        if (!values) {
            return std::nullopt;
        }
        std::string result;
        for (std::size_t index = 0U; index < values->size(); ++index) {
            if (index > 0U) {
                result += "; ";
            }
            result += values->at(index);
        }
        return result;
    }

    [[nodiscard]] std::optional<MetadataValues>
    resolveMetadata(const std::string_view name) const override {
        auto values = document_.effective_values(name);
        return values.empty() ? std::nullopt : std::optional{std::move(values)};
    }

    [[nodiscard]] std::optional<std::string>
    resolveTechnicalInfo(const std::string_view name) const override {
        const std::filesystem::path source{source_path_};
        const auto lowered = ascii_lower(name);
        std::string value;
        if (lowered == "path") {
            value = source.native();
        } else if (lowered == "directory") {
            value = source.parent_path().native();
        } else if (lowered == "filename") {
            value = source.stem().native();
        } else if (lowered == "filename_ext") {
            value = source.filename().native();
        } else if (lowered == "extension") {
            if (target_extension_) {
                value = *target_extension_;
            } else {
                value = source.extension().native();
                if (value.starts_with('.')) {
                    value.erase(value.begin());
                }
            }
        } else {
            return std::nullopt;
        }
        return core::unicodeCodePointCount(value) ? std::optional{std::move(value)} : std::nullopt;
    }

  private:
    const metadata::MetadataDocument& document_;
    std::string_view source_path_;
    const std::optional<std::string>& target_extension_;
};

struct SanitizedComponent {
    std::string value;
    bool changed{false};
};

[[nodiscard]] bool is_portable_reserved_stem(const std::string_view value) {
    auto stem = value.substr(0U, value.find('.'));
    const auto lowered = ascii_lower(stem);
    if (lowered == "con" || lowered == "prn" || lowered == "aux" || lowered == "nul") {
        return true;
    }
    return lowered.size() == 4U && (lowered.starts_with("com") || lowered.starts_with("lpt")) &&
           lowered.back() >= '1' && lowered.back() <= '9';
}

[[nodiscard]] SanitizedComponent sanitize_component(std::string value, const bool replace_separator,
                                                    const bool portable) {
    auto changed = false;
    for (auto& character : value) {
        const auto byte = static_cast<unsigned char>(character);
        const auto portable_forbidden =
            portable && (byte < 0x20U || character == '<' || character == '>' || character == ':' ||
                         character == '"' || character == '\\' || character == '|' ||
                         character == '?' || character == '*');
        if (character == '\0' || (replace_separator && character == '/') || portable_forbidden) {
            character = '_';
            changed = true;
        }
    }
    if (portable) {
        auto trailing = value.size();
        while (trailing > 0U && (value[trailing - 1U] == ' ' || value[trailing - 1U] == '.')) {
            value[trailing - 1U] = '_';
            --trailing;
            changed = true;
        }
    }
    if (value.empty()) {
        value = "_";
        changed = true;
    } else if (value == ".") {
        value = "_";
        changed = true;
    } else if (value == "..") {
        value = "__";
        changed = true;
    }
    if (portable && is_portable_reserved_stem(value)) {
        value.insert(value.begin(), '_');
        changed = true;
    }
    return {.value = std::move(value), .changed = changed};
}

struct SanitizedDirectory {
    std::string value;
    std::vector<std::string> components;
    bool changed{false};
};

[[nodiscard]] SanitizedDirectory sanitize_directory(const std::string_view raw,
                                                    const bool portable) {
    if (raw.empty()) {
        return {};
    }
    SanitizedDirectory result;
    std::size_t start = 0U;
    while (true) {
        const auto separator = raw.find('/', start);
        const auto component = separator == std::string_view::npos
                                   ? raw.substr(start)
                                   : raw.substr(start, separator - start);
        auto sanitized = sanitize_component(std::string{component}, false, portable);
        result.changed = result.changed || sanitized.changed;
        result.components.push_back(std::move(sanitized.value));
        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1U;
    }
    for (std::size_t index = 0U; index < result.components.size(); ++index) {
        if (index > 0U) {
            result.value.push_back('/');
        }
        result.value += result.components[index];
    }
    return result;
}

[[nodiscard]] bool contained_by(const std::filesystem::path& root,
                                const std::filesystem::path& target) {
    auto root_component = root.begin();
    auto target_component = target.begin();
    for (; root_component != root.end(); ++root_component, ++target_component) {
        if (target_component == target.end() || *root_component != *target_component) {
            return false;
        }
    }
    return target_component != target.end();
}

[[nodiscard]] std::string comparison_key(const std::string_view raw_path,
                                         const bool ascii_case_insensitive) {
    return ascii_case_insensitive ? ascii_lower(raw_path) : std::string{raw_path};
}

struct CalculatedItem {
    PlannedOutputPathSource source;
    std::vector<OutputPathPlanIssue> issues;
};

struct PhysicalSourceKey {
    std::uint64_t device{0U};
    std::uint64_t inode{0U};

    friend bool operator==(const PhysicalSourceKey&, const PhysicalSourceKey&) = default;
};

struct PhysicalSourceKeyHash {
    [[nodiscard]] std::size_t operator()(const PhysicalSourceKey& key) const noexcept {
        const auto first = std::hash<std::uint64_t>{}(key.device);
        const auto second = std::hash<std::uint64_t>{}(key.inode);
        return first ^ (second + 0x9e3779b9U + (first << 6U) + (first >> 2U));
    }
};

} // namespace

std::string_view output_path_plan_issue_kind_name(const OutputPathPlanIssueKind kind) {
    switch (kind) {
    case OutputPathPlanIssueKind::invalid_source_path:
        return "invalid source path";
    case OutputPathPlanIssueKind::expression_evaluation_failed:
        return "expression evaluation failed";
    case OutputPathPlanIssueKind::invalid_expression_output:
        return "invalid expression output";
    case OutputPathPlanIssueKind::absolute_relative_directory:
        return "absolute relative directory";
    case OutputPathPlanIssueKind::component_too_long:
        return "component too long";
    case OutputPathPlanIssueKind::path_too_long:
        return "path too long";
    case OutputPathPlanIssueKind::containment_failure:
        return "containment failure";
    case OutputPathPlanIssueKind::shared_source_target_conflict:
        return "shared source target conflict";
    case OutputPathPlanIssueKind::shared_source_revision_conflict:
        return "shared source revision conflict";
    case OutputPathPlanIssueKind::physical_source_alias:
        return "physical source alias";
    case OutputPathPlanIssueKind::duplicate_target:
        return "duplicate target";
    case OutputPathPlanIssueKind::existing_target:
        return "existing target";
    case OutputPathPlanIssueKind::target_parent_not_directory:
        return "target parent is not a directory";
    case OutputPathPlanIssueKind::source_target_dependency:
        return "source target dependency";
    case OutputPathPlanIssueKind::case_only_change:
        return "case-only change";
    case OutputPathPlanIssueKind::mirror_source_outside_root:
        return "source outside the mirror root";
    }
    return "output path issue";
}

std::string common_source_directory_raw_path(const std::span<const std::string> raw_paths) {
    std::string common;
    bool first = true;
    for (const auto& raw_path : raw_paths) {
        const auto slash = raw_path.rfind('/');
        if (slash == std::string::npos || raw_path.front() != '/') {
            return {};
        }
        auto directory = slash == 0U ? std::string{"/"} : raw_path.substr(0U, slash);
        if (first) {
            common = std::move(directory);
            first = false;
            continue;
        }
        // Shorten to the longest shared prefix that ends on a component
        // boundary in both directories.
        while (!(directory == common ||
                 (directory.size() > common.size() && directory.starts_with(common) &&
                  (common == "/" || directory[common.size()] == '/')))) {
            if (common == "/") {
                return "/";
            }
            const auto parent = common.rfind('/');
            common = parent == 0U ? std::string{"/"} : common.substr(0U, parent);
        }
    }
    return common;
}

bool OutputPathPlan::ready() const noexcept {
    return !sources.empty() && std::ranges::none_of(issues, &OutputPathPlanIssue::blocking);
}

core::Result<void> validate_output_layout_profile(const OutputLayoutProfile& profile,
                                                  const OutputPathPlanningLimits& limits) {
    const titleformat::DialectVersion current_dialect;
    const auto known_sanitization = profile.sanitization_policy.version == 1U &&
                                    (profile.sanitization_policy.name == "linux" ||
                                     profile.sanitization_policy.name == "portable");
    if (profile.schema_version != 1U || profile.name.empty() ||
        profile.name.size() > limits.profile_name_bytes ||
        profile.relative_directory_expression.size() > limits.expression_bytes ||
        profile.basename_expression.empty() ||
        profile.basename_expression.size() > limits.expression_bytes ||
        profile.dialect != current_dialect || !known_sanitization) {
        return std::unexpected(plan_error(core::ErrorCode::invalid_argument,
                                          "output layout requires schema 1, tkfmt-1, linux-v1 or "
                                          "portable-v1 sanitization, a bounded name, "
                                          "and a bounded non-empty basename expression"));
    }
    if (auto valid = validate_utf8(profile.name, "output layout name"); !valid) {
        return valid;
    }
    if (auto valid = validate_utf8(profile.relative_directory_expression,
                                   "output layout directory expression");
        !valid) {
        return valid;
    }
    if (auto valid =
            validate_utf8(profile.basename_expression, "output layout basename expression");
        !valid) {
        return valid;
    }
    if (auto compiled = compile_expression(profile, profile.relative_directory_expression,
                                           "output layout directory expression");
        !compiled) {
        return std::unexpected(std::move(compiled.error()));
    }
    if (auto compiled = compile_expression(profile, profile.basename_expression,
                                           "output layout basename expression");
        !compiled) {
        return std::unexpected(std::move(compiled.error()));
    }
    return {};
}

core::Result<void> validate_destination_profile(const DestinationProfile& profile,
                                                const OutputPathPlanningLimits& limits) {
    if (profile.schema_version != 1U || profile.name.empty() ||
        profile.name.size() > limits.profile_name_bytes || profile.root_raw_path.empty() ||
        profile.root_raw_path.size() > limits.root_path_bytes ||
        profile.containment_policy.name != "lexical-beneath-root" ||
        profile.containment_policy.version != 1U ||
        !is_normal_absolute_root(profile.root_raw_path)) {
        return std::unexpected(plan_error(
            core::ErrorCode::invalid_argument,
            "destination requires schema 1, a bounded name, a normalized absolute non-root raw "
            "path, and lexical-beneath-root-v1 containment"));
    }
    return validate_utf8(profile.name, "destination name");
}

core::Result<OutputPathPlan>
plan_output_paths(const std::span<const OutputPathPlanningItem> items,
                  const OutputPathOperationSelection operations, OutputLayoutProfile layout,
                  std::optional<DestinationProfile> destination,
                  OutputPathPlanningSnapshot snapshot, const core::CancellationToken& cancellation,
                  const OutputPathPlanningLimits& limits,
                  const std::optional<ConvertedPublicationPolicy>& converted) {
    std::optional<std::string> target_extension;
    std::optional<std::string> mirror_root;
    if (converted) {
        target_extension = converted->target_extension;
        mirror_root = converted->mirror_source_root_raw_path;
    }
    if (mirror_root && *mirror_root != "/" && !is_normal_absolute_path(*mirror_root)) {
        return std::unexpected(
            plan_error(core::ErrorCode::invalid_argument,
                       "mirror planning requires a normalized absolute source root"));
    }
    if (!operations.rename_files && !operations.move_files) {
        return std::unexpected(plan_error(core::ErrorCode::invalid_argument,
                                          "output path planning requires rename or move"));
    }
    if (items.empty() || items.size() > limits.items) {
        return std::unexpected(
            plan_error(core::ErrorCode::invalid_argument,
                       "output path planning requires a bounded non-empty item selection"));
    }
    if (!mirror_root) {
        if (auto valid = validate_output_layout_profile(layout, limits); !valid) {
            return std::unexpected(std::move(valid.error()));
        }
    }
    if (operations.move_files) {
        if (!destination) {
            return std::unexpected(plan_error(core::ErrorCode::invalid_argument,
                                              "move planning requires a destination profile"));
        }
        if (auto valid = validate_destination_profile(*destination, limits); !valid) {
            return std::unexpected(std::move(valid.error()));
        }
    }
    for (std::size_t index = 0U; index < items.size(); ++index) {
        if (index > 0U && items[index - 1U].item_index >= items[index].item_index) {
            return std::unexpected(plan_error(
                core::ErrorCode::invalid_argument,
                "output path planning item indexes must be bounded, sorted, and unique"));
        }
        if (items[index].source_revision.inode == 0U) {
            return std::unexpected(plan_error(
                core::ErrorCode::invalid_argument,
                "output path planning requires an observed source revision for every item"));
        }
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(
            plan_error(core::ErrorCode::cancelled, "output path planning was cancelled"));
    }

    // Mirror mode derives paths from the sources; the layout expressions are
    // neither compiled nor evaluated.
    std::optional<titleformat::Program> directory_program;
    std::optional<titleformat::Program> basename_program;
    if (!mirror_root) {
        auto compiled_directory = compile_expression(layout, layout.relative_directory_expression,
                                                     "output layout directory expression");
        auto compiled_basename = compile_expression(layout, layout.basename_expression,
                                                    "output layout basename expression");
        if (!compiled_directory || !compiled_basename) {
            return std::unexpected(!compiled_directory ? std::move(compiled_directory.error())
                                                       : std::move(compiled_basename.error()));
        }
        directory_program = std::move(*compiled_directory);
        basename_program = std::move(*compiled_basename);
    }

    std::unordered_map<std::string, ObservedOutputPathKind> observed;
    observed.reserve(snapshot.existing_paths.size());
    for (const auto& entry : snapshot.existing_paths) {
        if (!is_normal_absolute_observation(entry.raw_path)) {
            return std::unexpected(
                plan_error(core::ErrorCode::invalid_argument,
                           "output path observation requires a normalized absolute raw path"));
        }
        const auto key = comparison_key(entry.raw_path, snapshot.ascii_case_insensitive);
        if (const auto [found, inserted] = observed.emplace(key, entry.kind);
            !inserted && found->second != entry.kind) {
            return std::unexpected(
                plan_error(core::ErrorCode::invalid_argument,
                           "output path observations disagree about one normalized path"));
        }
    }

    OutputPathPlan plan{.layout = std::move(layout),
                        .destination = std::move(destination),
                        .operations = operations,
                        .sources = {},
                        .issues = {}};
    std::vector<CalculatedItem> calculated;
    calculated.reserve(items.size());
    const auto add_item_issue = [&](CalculatedItem& item, const OutputPathPlanIssueKind kind,
                                    std::string message, const bool blocking = true) {
        item.issues.push_back(OutputPathPlanIssue{
            .kind = kind,
            .blocking = blocking,
            .message = std::move(message),
            .item_indexes = item.source.item_indexes,
            .source_raw_path = item.source.source_raw_path,
            .target_raw_path = item.source.target_raw_path.empty()
                                   ? std::nullopt
                                   : std::optional{item.source.target_raw_path},
        });
    };

    for (const auto& input : items) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(
                plan_error(core::ErrorCode::cancelled, "output path planning was cancelled"));
        }
        CalculatedItem item;
        item.source.source_raw_path = input.source_raw_path;
        item.source.source_revision = input.source_revision;
        item.source.item_indexes = {input.item_index};
        if (!is_normal_absolute_path(input.source_raw_path)) {
            add_item_issue(item, OutputPathPlanIssueKind::invalid_source_path,
                           "Source path is not a normalized absolute file path");
            calculated.push_back(std::move(item));
            continue;
        }

        if (mirror_root) {
            // Byte-exact mirroring below the source root: raw OS path bytes,
            // so the expression-output UTF-8 gate does not apply.
            const auto& source = input.source_raw_path;
            const auto slash = source.rfind('/');
            const auto directory = slash == 0U ? std::string{"/"} : source.substr(0U, slash);
            const auto inside =
                directory == *mirror_root ||
                (directory.size() > mirror_root->size() && directory.starts_with(*mirror_root) &&
                 (*mirror_root == "/" || directory[mirror_root->size()] == '/'));
            if (!inside) {
                add_item_issue(item, OutputPathPlanIssueKind::mirror_source_outside_root,
                               "Source lies outside the mirrored root directory");
            } else if (directory != *mirror_root) {
                item.source.raw_relative_directory =
                    directory.substr(*mirror_root == "/" ? 1U : mirror_root->size() + 1U);
            }
            const auto filename = source.substr(slash + 1U);
            const auto dot = filename.rfind('.');
            item.source.raw_basename =
                dot == std::string::npos || dot == 0U ? filename : filename.substr(0U, dot);
        } else {
            const PathEvaluationContext context{input.final_metadata, input.source_raw_path,
                                                target_extension};
            if (operations.move_files) {
                auto evaluated = titleformat::evaluate(
                    *directory_program, context,
                    titleformat::EvaluationOptions{.maximum_steps = 100'000U,
                                                   .maximum_output_bytes = limits.expression_bytes,
                                                   .maximum_expanded_results = 1U,
                                                   .cancellation = cancellation});
                if (!evaluated) {
                    if (evaluated.error().code == core::ErrorCode::cancelled) {
                        return std::unexpected(std::move(evaluated.error()));
                    }
                    add_item_issue(item, OutputPathPlanIssueKind::expression_evaluation_failed,
                                   "Directory expression failed: " + evaluated.error().message);
                } else {
                    item.source.raw_relative_directory = std::move(evaluated->text);
                }
            }
            if (operations.rename_files) {
                auto evaluated = titleformat::evaluate(
                    *basename_program, context,
                    titleformat::EvaluationOptions{.maximum_steps = 100'000U,
                                                   .maximum_output_bytes = limits.expression_bytes,
                                                   .maximum_expanded_results = 1U,
                                                   .cancellation = cancellation});
                if (!evaluated) {
                    if (evaluated.error().code == core::ErrorCode::cancelled) {
                        return std::unexpected(std::move(evaluated.error()));
                    }
                    add_item_issue(item, OutputPathPlanIssueKind::expression_evaluation_failed,
                                   "Basename expression failed: " + evaluated.error().message);
                } else {
                    item.source.raw_basename = std::move(evaluated->text);
                }
            }
            if (auto valid = validate_utf8(item.source.raw_relative_directory,
                                           "evaluated relative directory");
                !valid) {
                add_item_issue(item, OutputPathPlanIssueKind::invalid_expression_output,
                               valid.error().message);
            }
            if (operations.rename_files) {
                if (auto valid = validate_utf8(item.source.raw_basename, "evaluated basename");
                    !valid) {
                    add_item_issue(item, OutputPathPlanIssueKind::invalid_expression_output,
                                   valid.error().message);
                }
            }
            if (operations.move_files && item.source.raw_relative_directory.starts_with('/')) {
                add_item_issue(item, OutputPathPlanIssueKind::absolute_relative_directory,
                               "Directory expression produced an absolute path");
            }
        }

        const auto portable = plan.layout.sanitization_policy.name == "portable";
        const auto sanitized_directory =
            sanitize_directory(item.source.raw_relative_directory, portable);
        item.source.sanitized_relative_directory = sanitized_directory.value;
        SanitizedComponent sanitized_basename;
        if (operations.rename_files) {
            sanitized_basename = sanitize_component(item.source.raw_basename, true, portable);
            item.source.sanitized_basename = sanitized_basename.value;
        }
        item.source.sanitized = sanitized_directory.changed || sanitized_basename.changed;

        const std::filesystem::path source_path{input.source_raw_path};
        const auto root =
            operations.move_files
                ? operation_root_path(std::filesystem::path{plan.destination->root_raw_path})
                : source_path.parent_path();
        auto target_parent = root;
        if (operations.move_files && !sanitized_directory.value.empty()) {
            target_parent /= std::filesystem::path{sanitized_directory.value};
        }
        const auto target_suffix =
            target_extension ? "." + *target_extension : source_path.extension().native();
        const auto filename = operations.rename_files ? sanitized_basename.value + target_suffix
                                                      : source_path.filename().native();
        const auto target = (target_parent / std::filesystem::path{filename}).lexically_normal();
        item.source.target_raw_path = target.native();
        item.source.no_change = item.source.target_raw_path == item.source.source_raw_path;

        for (const auto& component : sanitized_directory.components) {
            if (component.size() > limits.component_bytes) {
                add_item_issue(item, OutputPathPlanIssueKind::component_too_long,
                               "Sanitized directory component exceeds the byte limit");
            }
        }
        if (filename.size() > limits.component_bytes) {
            add_item_issue(item, OutputPathPlanIssueKind::component_too_long,
                           "Sanitized filename exceeds the byte limit");
        }
        if (item.source.target_raw_path.size() > limits.path_bytes) {
            add_item_issue(item, OutputPathPlanIssueKind::path_too_long,
                           "Target path exceeds the byte limit");
        }
        if (!contained_by(root, target)) {
            add_item_issue(item, OutputPathPlanIssueKind::containment_failure,
                           "Target is not lexically contained below its operation root");
        }
        const auto source_key = comparison_key(item.source.source_raw_path, true);
        const auto target_key = comparison_key(item.source.target_raw_path, true);
        if (!item.source.no_change && source_key == target_key) {
            add_item_issue(item, OutputPathPlanIssueKind::case_only_change,
                           "Source and target differ only by ASCII letter case", false);
        }
        calculated.push_back(std::move(item));
    }

    std::unordered_map<std::string, std::size_t> source_positions;
    source_positions.reserve(calculated.size());
    std::vector<bool> shared_conflicts;
    std::vector<bool> shared_revision_conflicts;
    for (auto& item : calculated) {
        for (auto& issue : item.issues) {
            if (plan.issues.size() >= limits.issues) {
                return std::unexpected(plan_error(core::ErrorCode::limit_exceeded,
                                                  "output path issue limit was exceeded"));
            }
            plan.issues.push_back(std::move(issue));
        }
        // Converted publication fans logical items out to one output each,
        // so entries sharing a source file are independent, never merged.
        const auto found =
            converted ? source_positions.end() : source_positions.find(item.source.source_raw_path);
        if (found == source_positions.end()) {
            source_positions.emplace(item.source.source_raw_path, plan.sources.size());
            plan.sources.push_back(std::move(item.source));
            shared_conflicts.push_back(false);
            shared_revision_conflicts.push_back(false);
            continue;
        }
        auto& retained = plan.sources[found->second];
        retained.item_indexes.insert(retained.item_indexes.end(), item.source.item_indexes.begin(),
                                     item.source.item_indexes.end());
        if (retained.target_raw_path != item.source.target_raw_path ||
            retained.raw_relative_directory != item.source.raw_relative_directory ||
            retained.raw_basename != item.source.raw_basename) {
            shared_conflicts[found->second] = true;
        }
        if (retained.source_revision != item.source.source_revision) {
            shared_revision_conflicts[found->second] = true;
        }
    }
    for (std::size_t index = 0U; index < plan.sources.size(); ++index) {
        if (!shared_conflicts[index]) {
            continue;
        }
        if (plan.issues.size() >= limits.issues) {
            return std::unexpected(plan_error(core::ErrorCode::limit_exceeded,
                                              "output path issue limit was exceeded"));
        }
        plan.issues.push_back(OutputPathPlanIssue{
            .kind = OutputPathPlanIssueKind::shared_source_target_conflict,
            .blocking = true,
            .message = "Logical items sharing one physical source produced different targets",
            .item_indexes = plan.sources[index].item_indexes,
            .source_raw_path = plan.sources[index].source_raw_path,
            .target_raw_path = plan.sources[index].target_raw_path,
        });
    }
    for (std::size_t index = 0U; index < plan.sources.size(); ++index) {
        if (!shared_revision_conflicts[index]) {
            continue;
        }
        if (plan.issues.size() >= limits.issues) {
            return std::unexpected(plan_error(core::ErrorCode::limit_exceeded,
                                              "output path issue limit was exceeded"));
        }
        plan.issues.push_back(OutputPathPlanIssue{
            .kind = OutputPathPlanIssueKind::shared_source_revision_conflict,
            .blocking = true,
            .message = "Logical items sharing one physical source carry different revisions",
            .item_indexes = plan.sources[index].item_indexes,
            .source_raw_path = plan.sources[index].source_raw_path,
            .target_raw_path = plan.sources[index].target_raw_path,
        });
    }

    std::unordered_map<PhysicalSourceKey, std::size_t, PhysicalSourceKeyHash> physical_sources;
    physical_sources.reserve(plan.sources.size());
    for (std::size_t index = 0U; !converted && index < plan.sources.size(); ++index) {
        const auto& source = plan.sources[index];
        const PhysicalSourceKey key{.device = source.source_revision.device,
                                    .inode = source.source_revision.inode};
        if (const auto [existing, inserted] = physical_sources.emplace(key, index); !inserted) {
            auto indexes = plan.sources[existing->second].item_indexes;
            indexes.insert(indexes.end(), source.item_indexes.begin(), source.item_indexes.end());
            if (plan.issues.size() >= limits.issues) {
                return std::unexpected(plan_error(core::ErrorCode::limit_exceeded,
                                                  "output path issue limit was exceeded"));
            }
            plan.issues.push_back(OutputPathPlanIssue{
                .kind = OutputPathPlanIssueKind::physical_source_alias,
                .blocking = true,
                .message = "Different raw paths refer to the same observed physical source",
                .item_indexes = std::move(indexes),
                .source_raw_path = source.source_raw_path,
                .target_raw_path = source.target_raw_path,
            });
        }
    }

    std::unordered_map<std::string, std::size_t> source_keys;
    source_keys.reserve(plan.sources.size());
    for (std::size_t index = 0U; index < plan.sources.size(); ++index) {
        source_keys.emplace(
            comparison_key(plan.sources[index].source_raw_path, snapshot.ascii_case_insensitive),
            index);
    }
    std::unordered_map<std::string, std::size_t> target_positions;
    target_positions.reserve(plan.sources.size());
    const auto add_plan_issue = [&](OutputPathPlanIssue issue) -> core::Result<void> {
        if (plan.issues.size() >= limits.issues) {
            return std::unexpected(plan_error(core::ErrorCode::limit_exceeded,
                                              "output path issue limit was exceeded"));
        }
        plan.issues.push_back(std::move(issue));
        return {};
    };
    for (std::size_t index = 0U; index < plan.sources.size(); ++index) {
        const auto& source = plan.sources[index];
        if (source.target_raw_path.empty()) {
            continue;
        }
        const auto source_key =
            comparison_key(source.source_raw_path, snapshot.ascii_case_insensitive);
        const auto target_key =
            comparison_key(source.target_raw_path, snapshot.ascii_case_insensitive);
        if (const auto [existing, inserted] = target_positions.emplace(target_key, index);
            !inserted && existing->second != index) {
            auto indexes = plan.sources[existing->second].item_indexes;
            indexes.insert(indexes.end(), source.item_indexes.begin(), source.item_indexes.end());
            if (auto added = add_plan_issue(OutputPathPlanIssue{
                    .kind = OutputPathPlanIssueKind::duplicate_target,
                    .blocking = true,
                    .message = "Multiple physical sources produced the same target",
                    .item_indexes = std::move(indexes),
                    .source_raw_path = source.source_raw_path,
                    .target_raw_path = source.target_raw_path,
                });
                !added) {
                return std::unexpected(std::move(added.error()));
            }
        }
        if (const auto dependency = source_keys.find(target_key);
            dependency != source_keys.end() && dependency->second != index) {
            if (auto added = add_plan_issue(OutputPathPlanIssue{
                    .kind = OutputPathPlanIssueKind::source_target_dependency,
                    .blocking = true,
                    .message = "Target is another selected physical source",
                    .item_indexes = source.item_indexes,
                    .source_raw_path = source.source_raw_path,
                    .target_raw_path = source.target_raw_path,
                });
                !added) {
                return std::unexpected(std::move(added.error()));
            }
        }
        if (const auto existing = observed.find(target_key);
            existing != observed.end() && target_key != source_key) {
            if (auto added = add_plan_issue(OutputPathPlanIssue{
                    .kind = OutputPathPlanIssueKind::existing_target,
                    .blocking = true,
                    .message = "Target already exists in the filesystem snapshot",
                    .item_indexes = source.item_indexes,
                    .source_raw_path = source.source_raw_path,
                    .target_raw_path = source.target_raw_path,
                });
                !added) {
                return std::unexpected(std::move(added.error()));
            }
        }

        auto parent = std::filesystem::path{source.target_raw_path}.parent_path();
        while (!parent.empty() && parent != parent.root_path()) {
            const auto key = comparison_key(parent.native(), snapshot.ascii_case_insensitive);
            const auto existing = observed.find(key);
            if (existing != observed.end() &&
                existing->second != ObservedOutputPathKind::directory) {
                if (auto added = add_plan_issue(OutputPathPlanIssue{
                        .kind = OutputPathPlanIssueKind::target_parent_not_directory,
                        .blocking = true,
                        .message = "A target parent exists but is not a directory",
                        .item_indexes = source.item_indexes,
                        .source_raw_path = source.source_raw_path,
                        .target_raw_path = parent.native(),
                    });
                    !added) {
                    return std::unexpected(std::move(added.error()));
                }
                break;
            }
            parent = parent.parent_path();
        }
    }
    return plan;
}

} // namespace trackknife::operations
