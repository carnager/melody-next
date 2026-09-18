// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/rule_script_import.hpp"

#include "trackknife/core/unicode.hpp"
#include "trackknife/metadata/document.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trackknife::metadata {
namespace {

enum class NodeKind : std::uint8_t {
    literal,
    field,
    call,
};

struct Node;
using Sequence = std::vector<Node>;

struct Node {
    NodeKind kind{NodeKind::literal};
    std::size_t offset{0U};
    std::string text;
    std::vector<Sequence> arguments;
};

[[nodiscard]] bool ascii_space(const char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f' ||
           value == '\v';
}

[[nodiscard]] bool ascii_identifier_character(const char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_';
}

[[nodiscard]] std::string trim_ascii(std::string value) {
    const auto first = std::ranges::find_if_not(value, ascii_space);
    if (first == value.end()) {
        return {};
    }
    const auto last = std::ranges::find_if_not(value | std::views::reverse, ascii_space).base();
    return std::string{first, last};
}

[[nodiscard]] std::string escape_argument_literal(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        if (character == '$' || character == '%' || character == ',' || character == '(' ||
            character == ')' || character == '\\') {
            result.push_back('\\');
        }
        result.push_back(character);
    }
    return result;
}

[[nodiscard]] core::Error export_error(const core::ErrorCode code, std::string message,
                                       const std::size_t action_index) {
    return core::Error{
        .code = code,
        .message = std::move(message),
        .context = {{.key = "action", .value = std::to_string(action_index)}},
    };
}

[[nodiscard]] bool layout_only(const std::string_view value) {
    return std::ranges::all_of(value, ascii_space);
}

[[nodiscard]] std::string ascii_lower(std::string value) {
    for (auto& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

class Parser final {
  public:
    Parser(const std::string_view source, const MetadataRuleScriptImportLimits& limits,
           MetadataRuleScriptImportResult& result)
        : source_(source), limits_(limits), result_(result) {}

    [[nodiscard]] Sequence parse() {
        if (source_.size() > limits_.source_bytes) {
            diagnostic(MetadataRuleScriptDiagnosticSeverity::error, 0U,
                       "The pasted script exceeds the 1 MiB import limit");
            return {};
        }
        if (!core::unicodeCodePointCount(source_)) {
            diagnostic(MetadataRuleScriptDiagnosticSeverity::error, 0U,
                       "The pasted script must be valid UTF-8");
            return {};
        }
        return sequence(false, 0U);
    }

  private:
    [[nodiscard]] Sequence sequence(const bool nested, const std::size_t depth) {
        Sequence nodes;
        while (position_ < source_.size()) {
            const auto current = source_[position_];
            if (nested && (current == ',' || current == ')')) {
                break;
            }
            if (!nested && (current == ',' || current == ')')) {
                diagnostic(MetadataRuleScriptDiagnosticSeverity::warning, position_,
                           current == ','
                               ? "Ignored a top-level comma between complete rules"
                               : "Ignored an unmatched closing parenthesis after a complete rule");
                ++position_;
                continue;
            }
            if (current == '$') {
                nodes.push_back(call(depth));
            } else if (current == '%') {
                nodes.push_back(field());
            } else {
                nodes.push_back(literal(nested));
            }
            if (++node_count_ > limits_.syntax_nodes) {
                diagnostic(MetadataRuleScriptDiagnosticSeverity::error, position_,
                           "The pasted script exceeds the syntax-node limit");
                position_ = source_.size();
                break;
            }
        }
        return nodes;
    }

    [[nodiscard]] Node call(const std::size_t depth) {
        const auto start = position_++;
        Node node{.kind = NodeKind::call, .offset = start, .text = {}, .arguments = {}};
        const auto name_start = position_;
        while (position_ < source_.size()) {
            if (!ascii_identifier_character(source_[position_])) {
                break;
            }
            ++position_;
        }
        node.text = ascii_lower(std::string{source_.substr(name_start, position_ - name_start)});
        if (node.text.empty() || position_ >= source_.size() || source_[position_] != '(') {
            diagnostic(MetadataRuleScriptDiagnosticSeverity::error, start,
                       "Expected a function name followed by '('");
            if (position_ == start + 1U) {
                node.kind = NodeKind::literal;
                node.text = "$";
            }
            return node;
        }
        if (depth >= limits_.nesting_depth) {
            diagnostic(MetadataRuleScriptDiagnosticSeverity::error, start,
                       "The pasted script exceeds the nesting-depth limit");
            position_ = source_.size();
            return node;
        }
        ++position_;
        if (position_ < source_.size() && source_[position_] == ')') {
            ++position_;
            return node;
        }
        while (position_ < source_.size()) {
            node.arguments.push_back(sequence(true, depth + 1U));
            if (position_ >= source_.size()) {
                diagnostic(MetadataRuleScriptDiagnosticSeverity::error, start,
                           "Function call is missing its closing parenthesis");
                return node;
            }
            if (source_[position_] == ',') {
                ++position_;
                continue;
            }
            ++position_;
            return node;
        }
        diagnostic(MetadataRuleScriptDiagnosticSeverity::error, start,
                   "Function call is missing its closing parenthesis");
        return node;
    }

    [[nodiscard]] Node field() {
        const auto start = position_++;
        const auto name_start = position_;
        const auto end = source_.find('%', position_);
        if (end == std::string_view::npos) {
            diagnostic(MetadataRuleScriptDiagnosticSeverity::error, start,
                       "Field reference is missing its closing '%'");
            position_ = source_.size();
            return Node{.kind = NodeKind::field,
                        .offset = start,
                        .text = std::string{source_.substr(name_start)},
                        .arguments = {}};
        }
        position_ = end + 1U;
        return Node{.kind = NodeKind::field,
                    .offset = start,
                    .text = std::string{source_.substr(name_start, end - name_start)},
                    .arguments = {}};
    }

    [[nodiscard]] Node literal(const bool nested) {
        const auto start = position_;
        std::string value;
        while (position_ < source_.size()) {
            const auto current = source_[position_];
            if (current == '$' || current == '%' ||
                (nested && (current == ',' || current == ')')) ||
                (!nested && (current == ',' || current == ')'))) {
                break;
            }
            if (current == '\\' && position_ + 1U < source_.size()) {
                value.push_back(source_[position_ + 1U]);
                position_ += 2U;
            } else {
                value.push_back(current);
                ++position_;
            }
        }
        return Node{
            .kind = NodeKind::literal, .offset = start, .text = std::move(value), .arguments = {}};
    }

    void diagnostic(const MetadataRuleScriptDiagnosticSeverity severity, const std::size_t offset,
                    std::string message) {
        std::size_t line = 1U;
        std::size_t column = 1U;
        for (std::size_t index = 0U; index < std::min(offset, source_.size()); ++index) {
            if (source_[index] == '\n') {
                ++line;
                column = 1U;
            } else {
                ++column;
            }
        }
        result_.diagnostics.push_back(MetadataRuleScriptDiagnostic{
            .severity = severity,
            .byte_offset = offset,
            .line = line,
            .column = column,
            .message = std::move(message),
        });
    }

    std::string_view source_;
    const MetadataRuleScriptImportLimits& limits_;
    MetadataRuleScriptImportResult& result_;
    std::size_t position_{0U};
    std::size_t node_count_{0U};
};

class Translator final {
  public:
    Translator(MetadataRuleScriptImportResult& result, const MetadataRuleScriptImportLimits& limits,
               const std::string_view source)
        : result_(result), limits_(limits), source_(source) {}

    void translate(const Sequence& script) {
        translate_sequence(script, std::nullopt);
        if (result_.actions.size() > limits_.actions) {
            error(0U, "The generated rule list exceeds the 256-action limit");
        }
        if (result_.actions.empty() && !result_.has_errors()) {
            error(0U, "The pasted script did not generate any supported rules");
        }
        if (!result_.actions.empty() && !result_.has_errors()) {
            const MetadataTransformationChain chain{
                .schema_version = 1U,
                .name = "Imported rules",
                .actions = result_.actions,
            };
            if (const auto valid = validate_metadata_transformation_chain(chain); !valid) {
                error(0U, "Generated rules failed validation: " + valid.error().message);
            }
        }
    }

  private:
    [[nodiscard]] static std::vector<const Node*> meaningful(const Sequence& sequence) {
        std::vector<const Node*> result;
        for (const auto& node : sequence) {
            if (node.kind != NodeKind::literal || !layout_only(node.text)) {
                result.push_back(&node);
            }
        }
        return result;
    }

    [[nodiscard]] std::optional<std::string> literal_name(const Sequence& sequence,
                                                          const std::size_t offset) {
        std::string value;
        for (const auto& node : sequence) {
            if (node.kind != NodeKind::literal) {
                error(offset, "Rule target names must be literal field names");
                return std::nullopt;
            }
            value += node.text;
        }
        value = trim_ascii(std::move(value));
        if (value.empty() || value.contains('*')) {
            error(offset, "Rule target names must be non-empty and cannot contain wildcards");
            return std::nullopt;
        }
        if (ascii_lower(value) == "comment:") {
            if (!warned_default_comment_) {
                warning(offset,
                        "Translated Picard's default comment target 'comment:' to Trackknife's "
                        "COMMENT field");
                warned_default_comment_ = true;
            }
            value = "comment";
        }
        return value;
    }

    [[nodiscard]] static std::string escape_literal(const std::string_view value) {
        std::string result;
        result.reserve(value.size());
        for (const auto character : value) {
            if (character == '$' || character == '%' || character == ',' || character == '(' ||
                character == ')' || character == '\\') {
                result.push_back('\\');
            }
            result.push_back(character);
        }
        return result;
    }

    [[nodiscard]] std::optional<std::string> expression(const Sequence& sequence,
                                                        const std::size_t offset) {
        std::string result;
        for (const auto& node : sequence) {
            if (node.kind == NodeKind::literal) {
                if (!layout_only(node.text)) {
                    result += escape_literal(node.text);
                }
                continue;
            }
            if (node.kind == NodeKind::field) {
                const auto name = trim_ascii(node.text);
                if (name.empty()) {
                    error(node.offset, "Field references cannot be empty");
                    return std::nullopt;
                }
                result += '%' + name + '%';
                continue;
            }
            constexpr std::array allowed{
                std::string_view{"if"}, std::string_view{"if2"},  std::string_view{"and"},
                std::string_view{"or"}, std::string_view{"not"},  std::string_view{"eq"},
                std::string_view{"ne"}, std::string_view{"left"},
            };
            if (!std::ranges::contains(allowed, std::string_view{node.text})) {
                error(node.offset, "Unsupported expression function $" + node.text);
                return std::nullopt;
            }
            result += '$' + node.text + '(';
            for (std::size_t index = 0U; index < node.arguments.size(); ++index) {
                if (index > 0U) {
                    result.push_back(',');
                }
                auto argument = expression(node.arguments[index], node.offset);
                if (!argument) {
                    return std::nullopt;
                }
                result += *argument;
            }
            result.push_back(')');
        }
        result = trim_ascii(std::move(result));
        if (result.empty()) {
            error(offset, "Expression cannot be empty");
            return std::nullopt;
        }
        return result;
    }

    [[nodiscard]] std::optional<std::uint32_t> keep_first_count(const std::string& target,
                                                                const Sequence& value) const {
        const auto nodes = meaningful(value);
        if (nodes.size() != 1U || nodes.front()->kind != NodeKind::call ||
            nodes.front()->text != "left" || nodes.front()->arguments.size() != 2U) {
            return std::nullopt;
        }
        const auto source = meaningful(nodes.front()->arguments[0]);
        if (source.size() != 1U || source.front()->kind != NodeKind::field ||
            canonicalize_field_name(source.front()->text) != canonicalize_field_name(target)) {
            return std::nullopt;
        }
        std::string count_text;
        for (const auto& node : nodes.front()->arguments[1]) {
            if (node.kind != NodeKind::literal) {
                return std::nullopt;
            }
            count_text += node.text;
        }
        count_text = trim_ascii(std::move(count_text));
        std::uint32_t count = 0U;
        const auto converted =
            std::from_chars(count_text.data(), count_text.data() + count_text.size(), count);
        if (converted.ec != std::errc{} || converted.ptr != count_text.data() + count_text.size() ||
            count == 0U || count > 1'000'000U) {
            return std::nullopt;
        }
        return count;
    }

    [[nodiscard]] static std::string combine_condition(const std::optional<std::string>& outer,
                                                       const std::string& inner) {
        return outer ? "$and(" + *outer + ',' + inner + ')' : inner;
    }

    [[nodiscard]] static bool is_call(const Node& node, const std::string_view name) {
        return node.kind == NodeKind::call && node.text == name;
    }

    [[nodiscard]] static std::optional<std::string>
    single_field_reference(const Sequence& sequence) {
        const auto nodes = meaningful(sequence);
        if (nodes.size() != 1U || nodes.front()->kind != NodeKind::field) {
            return std::nullopt;
        }
        auto name = trim_ascii(nodes.front()->text);
        return name.empty() ? std::nullopt : std::optional{std::move(name)};
    }

    struct SetMutation {
        const Node* call{nullptr};
        std::string target;
        std::string canonical_target;
    };

    [[nodiscard]] std::optional<std::vector<SetMutation>>
    set_mutations(const Sequence& sequence, const std::size_t condition_offset) {
        const auto nodes = meaningful(sequence);
        if (!std::ranges::all_of(nodes, [](const Node* node) { return is_call(*node, "set"); })) {
            return std::nullopt;
        }
        std::vector<SetMutation> mutations;
        mutations.reserve(nodes.size());
        for (const auto* node : nodes) {
            if (node->arguments.size() != 2U) {
                error(node->offset, "$set requires exactly a field name and value expression");
                return std::vector<SetMutation>{};
            }
            auto target = literal_name(node->arguments[0], node->offset);
            if (!target) {
                return std::vector<SetMutation>{};
            }
            const auto canonical_target = canonicalize_field_name(*target);
            if (std::ranges::any_of(mutations, [&canonical_target](const auto& mutation) {
                    return mutation.canonical_target == canonical_target;
                })) {
                error(condition_offset,
                      "A conditional branch cannot set the same field more than once during "
                      "script translation");
                return std::vector<SetMutation>{};
            }
            mutations.push_back(SetMutation{
                .call = node,
                .target = std::move(*target),
                .canonical_target = canonical_target,
            });
        }
        return mutations;
    }

    [[nodiscard]] bool translate_set_branches(const Node& node, const std::string& tested,
                                              const std::optional<std::string>& outer_condition) {
        if (outer_condition) {
            return false;
        }
        auto true_sets = set_mutations(node.arguments[1], node.offset);
        auto false_sets = node.arguments.size() == 3U
                              ? set_mutations(node.arguments[2], node.offset)
                              : std::optional{std::vector<SetMutation>{}};
        if (!true_sets || !false_sets) {
            return false;
        }
        if (true_sets->empty() && false_sets->empty()) {
            return false;
        }
        if (result_.has_errors()) {
            return true;
        }

        const auto tested_field = single_field_reference(node.arguments[0]);
        std::vector<bool> consumed_false(false_sets->size(), false);
        std::vector<MetadataTransformationAction> translated;
        translated.reserve(true_sets->size());
        for (auto& true_set : *true_sets) {
            const auto matching_false =
                std::ranges::find_if(*false_sets, [&true_set](const auto& false_set) {
                    return false_set.canonical_target == true_set.canonical_target;
                });
            if (matching_false != false_sets->end()) {
                auto true_value = expression(true_set.call->arguments[1], true_set.call->offset);
                auto false_value =
                    expression(matching_false->call->arguments[1], matching_false->call->offset);
                if (!true_value || !false_value) {
                    return true;
                }
                consumed_false[static_cast<std::size_t>(matching_false - false_sets->begin())] =
                    true;
                translated.push_back(MetadataFormatValueAction{
                    .target_field = std::move(true_set.target),
                    .dialect = {},
                    .source = "$if(" + tested + ',' + *true_value + ',' + *false_value + ')',
                });
                continue;
            }

            const auto count = keep_first_count(true_set.target, true_set.call->arguments[1]);
            if (!count || !tested_field ||
                canonicalize_field_name(*tested_field) != true_set.canonical_target) {
                error(node.offset,
                      "A field set in only one $if branch cannot yet be translated safely; "
                      "self-prefix cleanup guarded by that same field is supported");
                return true;
            }
            translated.push_back(MetadataKeepFirstCharactersAction{
                .target_field = std::move(true_set.target),
                .character_count = *count,
            });
        }
        if (std::ranges::contains(consumed_false, false)) {
            error(node.offset,
                  "A field set only in the false $if branch cannot yet be translated safely");
            return true;
        }
        result_.actions.insert(result_.actions.end(), std::make_move_iterator(translated.begin()),
                               std::make_move_iterator(translated.end()));
        return true;
    }

    void translate_sequence(const Sequence& sequence, const std::optional<std::string>& condition) {
        for (const auto& node : sequence) {
            if (node.kind == NodeKind::literal && layout_only(node.text)) {
                continue;
            }
            if (node.kind != NodeKind::call) {
                error(node.offset, "Only mutation function calls can appear as complete rules");
                continue;
            }
            translate_call(node, condition);
        }
    }

    void translate_call(const Node& node, const std::optional<std::string>& condition) {
        if (node.text == "unset" || node.text == "delete") {
            translate_remove(node, condition);
            return;
        }
        if (node.text == "set") {
            translate_set(node, condition);
            return;
        }
        if (node.text == "if") {
            translate_if(node, condition);
            return;
        }
        error(node.offset, "Unsupported mutation function $" + node.text);
    }

    void translate_remove(const Node& node, const std::optional<std::string>& condition) {
        if (node.arguments.size() != 1U) {
            error(node.offset, '$' + node.text + " requires exactly one field name");
            return;
        }
        auto target = literal_name(node.arguments.front(), node.offset);
        if (!target) {
            return;
        }
        if (node.text == "unset" && !warned_unset_) {
            warning(node.offset,
                    "Trackknife translates $unset as an actual exact-native Remove field rule; "
                    "it does not reproduce Picard's separate unset-versus-delete save behavior");
            warned_unset_ = true;
        }
        if (condition) {
            MetadataRemoveFieldIfAction action;
            action.target_field = std::move(*target);
            action.condition = *condition;
            action.match_mode = MetadataFieldMatchMode::exact_native;
            result_.actions.push_back(std::move(action));
        } else {
            result_.actions.push_back(MetadataRemoveFieldAction{
                .target_field = std::move(*target),
                .match_mode = MetadataFieldMatchMode::exact_native,
            });
        }
    }

    void translate_set(const Node& node, const std::optional<std::string>& condition) {
        if (node.arguments.size() != 2U) {
            error(node.offset, "$set requires exactly a field name and value expression");
            return;
        }
        if (condition) {
            error(node.offset,
                  "Conditional $set is supported only when both $if branches set the same field");
            return;
        }
        auto target = literal_name(node.arguments[0], node.offset);
        if (!target) {
            return;
        }
        if (const auto count = keep_first_count(*target, node.arguments[1])) {
            result_.actions.push_back(MetadataKeepFirstCharactersAction{
                .target_field = std::move(*target),
                .character_count = *count,
            });
            return;
        }
        auto source = expression(node.arguments[1], node.offset);
        if (!source) {
            return;
        }
        result_.actions.push_back(MetadataFormatValueAction{
            .target_field = std::move(*target),
            .dialect = {},
            .source = std::move(*source),
        });
    }

    void translate_if(const Node& node, const std::optional<std::string>& outer_condition) {
        if (node.arguments.size() < 2U || node.arguments.size() > 3U) {
            error(node.offset,
                  "$if accepts 2 or 3 arguments (condition, true branch, optional false "
                  "branch); this call has " +
                      std::to_string(node.arguments.size()));
            return;
        }
        auto tested = expression(node.arguments[0], node.offset);
        if (!tested) {
            return;
        }

        if (translate_set_branches(node, *tested, outer_condition)) {
            return;
        }

        const auto true_condition = combine_condition(outer_condition, *tested);
        translate_sequence(node.arguments[1], true_condition);
        if (node.arguments.size() == 3U) {
            const auto false_test = "$not(" + *tested + ')';
            const auto false_condition = combine_condition(outer_condition, false_test);
            translate_sequence(node.arguments[2], false_condition);
        }
    }

    void diagnostic(const MetadataRuleScriptDiagnosticSeverity severity, const std::size_t offset,
                    std::string message) {
        std::size_t line = 1U;
        std::size_t column = 1U;
        for (std::size_t index = 0U; index < std::min(offset, source_.size()); ++index) {
            if (source_[index] == '\n') {
                ++line;
                column = 1U;
            } else {
                ++column;
            }
        }
        result_.diagnostics.push_back(MetadataRuleScriptDiagnostic{
            .severity = severity,
            .byte_offset = offset,
            .line = line,
            .column = column,
            .message = std::move(message),
        });
    }

    void warning(const std::size_t offset, std::string message) {
        diagnostic(MetadataRuleScriptDiagnosticSeverity::warning, offset, std::move(message));
    }

    void error(const std::size_t offset, std::string message) {
        diagnostic(MetadataRuleScriptDiagnosticSeverity::error, offset, std::move(message));
    }

    MetadataRuleScriptImportResult& result_;
    const MetadataRuleScriptImportLimits& limits_;
    std::string_view source_;
    bool warned_unset_{false};
    bool warned_default_comment_{false};
};

} // namespace

bool MetadataRuleScriptImportResult::has_errors() const noexcept {
    return std::ranges::any_of(diagnostics, [](const auto& diagnostic) {
        return diagnostic.severity == MetadataRuleScriptDiagnosticSeverity::error;
    });
}

MetadataRuleScriptImportResult
import_metadata_rule_script(const std::string_view source,
                            const MetadataRuleScriptImportLimits& limits) {
    MetadataRuleScriptImportResult result;
    Parser parser{source, limits, result};
    const auto syntax = parser.parse();
    if (!result.has_errors()) {
        Translator translator{result, limits, source};
        translator.translate(syntax);
    }
    return result;
}

core::Result<std::string>
export_metadata_rule_script(const std::span<const MetadataTransformationAction> actions) {
    if (actions.empty()) {
        return std::unexpected(export_error(core::ErrorCode::invalid_argument,
                                            "Raw cleanup source requires at least one action", 0U));
    }

    std::string source;
    for (std::size_t index = 0U; index < actions.size(); ++index) {
        auto line = std::visit(
            [index](const auto& action) -> core::Result<std::string> {
                using Action = std::decay_t<decltype(action)>;
                const auto target = [&] {
                    if constexpr (std::is_same_v<Action, MetadataCaptureValuesAction> ||
                                  std::is_same_v<Action, MetadataBlocklistFieldsAction> ||
                                  std::is_same_v<Action, MetadataAllowlistFieldsAction>) {
                        return std::string{};
                    } else {
                        return escape_argument_literal(action.target_field);
                    }
                }();
                if constexpr (std::is_same_v<Action, MetadataRemoveFieldAction>) {
                    if (action.match_mode == MetadataFieldMatchMode::exact_native) {
                        return "$delete(" + target + ')';
                    }
                } else if constexpr (std::is_same_v<Action, MetadataRemoveFieldIfAction>) {
                    if (action.match_mode == MetadataFieldMatchMode::exact_native &&
                        action.dialect == titleformat::DialectVersion{}) {
                        return "$if(" + action.condition + ",$delete(" + target + "))";
                    }
                } else if constexpr (std::is_same_v<Action, MetadataFormatValueAction>) {
                    if (action.dialect == titleformat::DialectVersion{}) {
                        return "$set(" + target + ',' + action.source + ')';
                    }
                } else if constexpr (std::is_same_v<Action, MetadataKeepFirstCharactersAction>) {
                    if (!action.target_field.contains('%')) {
                        return "$set(" + target + ",$left(%" + action.target_field + "%," +
                               std::to_string(action.character_count) + "))";
                    }
                }
                return std::unexpected(export_error(
                    core::ErrorCode::unsupported,
                    "This typed transformation step is not representable by the raw cleanup "
                    "script subset",
                    index));
            },
            actions[index]);
        if (!line) {
            return std::unexpected(std::move(line.error()));
        }
        if (!source.empty()) {
            source.push_back('\n');
        }
        source += *line;
    }

    const auto round_trip = import_metadata_rule_script(source);
    if (round_trip.has_errors() || !std::ranges::equal(round_trip.actions, actions)) {
        return std::unexpected(export_error(
            core::ErrorCode::unsupported,
            "The typed transformation cannot round-trip through raw cleanup source", 0U));
    }
    return source;
}

} // namespace trackknife::metadata
