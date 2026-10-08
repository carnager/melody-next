// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/native_rule_script.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace trackknife::metadata {
namespace {

// The statement layer is read the way tkfmt-1 reads a call: `$name(` opens,
// `)` closes, a comma at the top separates arguments, `%...%` is one unit and
// a backslash keeps the next character from meaning anything. A bare `(` is
// text, as it is in tkfmt-1. The same walk splits statements, decodes their
// arguments and decides what an export has to escape, so the three agree.

[[nodiscard]] bool ascii_space(const char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f' ||
           value == '\v';
}

[[nodiscard]] bool name_character(const char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_';
}

[[nodiscard]] std::string trim_ascii(const std::string_view value) {
    const auto first = std::ranges::find_if_not(value, ascii_space);
    if (first == value.end()) {
        return {};
    }
    const auto last = std::ranges::find_if_not(value | std::views::reverse, ascii_space).base();
    return std::string{first, last};
}

[[nodiscard]] std::string ascii_lower(std::string value) {
    for (auto& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

// Where a `$name(` at `position` opens its argument list, or nothing.
[[nodiscard]] std::optional<std::size_t> call_opening(const std::string_view text,
                                                      const std::size_t position) {
    if (position >= text.size() || text[position] != '$') {
        return std::nullopt;
    }
    auto cursor = position + 1U;
    while (cursor < text.size() && name_character(text[cursor])) {
        ++cursor;
    }
    if (cursor == position + 1U || cursor >= text.size() || text[cursor] != '(') {
        return std::nullopt;
    }
    return cursor;
}

// One step of the walk over `text` from `position`: how far the unit goes and
// what it is.
enum class Unit : std::uint8_t { escape, field, open, close, comma, text };

struct Step {
    Unit unit{Unit::text};
    std::size_t end{0U};
};

[[nodiscard]] Step next_unit(const std::string_view text, const std::size_t position) {
    const auto current = text[position];
    if (current == '\\') {
        return {Unit::escape, std::min(position + 2U, text.size())};
    }
    if (current == '%') {
        const auto closing = text.find('%', position + 1U);
        return {Unit::field, closing == std::string_view::npos ? text.size() : closing + 1U};
    }
    if (const auto opening = call_opening(text, position)) {
        return {Unit::open, *opening + 1U};
    }
    if (current == ')') {
        return {Unit::close, position + 1U};
    }
    if (current == ',') {
        return {Unit::comma, position + 1U};
    }
    return {Unit::text, position + 1U};
}

struct Span {
    std::size_t begin{0U};
    std::size_t end{0U};
};

struct Call {
    std::string name;
    std::size_t offset{0U};
    std::vector<Span> arguments;
};

class Reader final {
  public:
    Reader(const std::string_view source, const MetadataRuleScriptImportLimits& limits,
           MetadataRuleScriptImportResult& result)
        : source_(source), limits_(limits), result_(result) {}

    void read() {
        if (source_.size() > limits_.source_bytes) {
            error(0U, "The script is larger than 1 MiB");
            return;
        }
        const auto calls = statements({0U, source_.size()});
        if (!calls) {
            return;
        }
        for (const auto& call : *calls) {
            translate(call);
        }
        if (result_.has_errors()) {
            return;
        }
        if (result_.actions.empty()) {
            error(0U, "The script has no steps");
        } else if (result_.actions.size() > limits_.actions) {
            error(0U, "The script has more than 256 steps");
        }
    }

  private:
    // The statements in `range`: calls, with nothing but layout between them.
    [[nodiscard]] std::optional<std::vector<Call>> statements(const Span range) {
        std::vector<Call> calls;
        auto position = range.begin;
        while (position < range.end) {
            if (ascii_space(source_[position])) {
                ++position;
                continue;
            }
            const auto opening = call_opening(source_.substr(0U, range.end), position);
            if (!opening) {
                error(position, "Expected a step such as $set(FIELD,value)");
                return std::nullopt;
            }
            Call call{
                .name = ascii_lower(
                    std::string{source_.substr(position + 1U, *opening - position - 1U)}),
                .offset = position,
                .arguments = {},
            };
            auto cursor = *opening + 1U;
            auto argument_start = cursor;
            std::size_t depth = 0U;
            bool closed = false;
            while (cursor < range.end) {
                const auto step = next_unit(source_.substr(0U, range.end), cursor);
                if (step.unit == Unit::escape && step.end - cursor < 2U) {
                    error(cursor, "A backslash at the end quotes nothing");
                    return std::nullopt;
                }
                if (step.unit == Unit::field && source_[step.end - 1U] != '%') {
                    error(cursor, "A % is not closed; write \\% for a literal percent sign");
                    return std::nullopt;
                }
                if (step.unit == Unit::open) {
                    ++depth;
                    if (depth > limits_.nesting_depth) {
                        error(cursor, "Steps and expressions are nested too deeply");
                        return std::nullopt;
                    }
                } else if (step.unit == Unit::close) {
                    if (depth == 0U) {
                        call.arguments.push_back({argument_start, cursor});
                        cursor = step.end;
                        closed = true;
                        break;
                    }
                    --depth;
                } else if (step.unit == Unit::comma && depth == 0U) {
                    call.arguments.push_back({argument_start, cursor});
                    argument_start = step.end;
                }
                cursor = step.end;
            }
            if (!closed) {
                error(position, "$" + call.name + "( is not closed");
                return std::nullopt;
            }
            // `$name()` has no arguments rather than one empty one.
            if (call.arguments.size() == 1U &&
                call.arguments.front().begin == call.arguments.front().end) {
                call.arguments.clear();
            }
            calls.push_back(std::move(call));
            position = cursor;
        }
        return calls;
    }

    [[nodiscard]] std::string_view raw(const Span span) const {
        return source_.substr(span.begin, span.end - span.begin);
    }

    // Text as written, every backslash removed; a % or $ that would mean
    // something is refused rather than taken as text.
    [[nodiscard]] std::optional<std::string> literal(const Span span) {
        std::string value;
        const auto text = raw(span);
        for (std::size_t position = 0U; position < text.size(); ++position) {
            const auto current = text[position];
            if (current == '\\' && position + 1U < text.size()) {
                value.push_back(text[++position]);
                continue;
            }
            if (current == '%' || call_opening(text, position)) {
                error(span.begin + position,
                      "Plain text is expected here; write \\% or \\$ for a literal % or $");
                return std::nullopt;
            }
            value.push_back(current);
        }
        return value;
    }

    [[nodiscard]] std::optional<std::string> field(const Span span) {
        auto value = literal(span);
        if (!value) {
            return std::nullopt;
        }
        auto trimmed = trim_ascii(*value);
        if (trimmed.empty()) {
            error(span.begin, "A field name is expected here");
            return std::nullopt;
        }
        return trimmed;
    }

    [[nodiscard]] std::optional<std::uint32_t> number(const Span span) {
        const auto value = literal(span);
        if (!value) {
            return std::nullopt;
        }
        const auto text = trim_ascii(*value);
        std::uint32_t parsed = 0U;
        const auto [end, failure] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (text.empty() || failure != std::errc{} || end != text.data() + text.size()) {
            error(span.begin, "A whole number is expected here");
            return std::nullopt;
        }
        return parsed;
    }

    // An embedded expression or pattern: as written, but for the characters
    // `unquote` names, whose backslash only kept them from ending the
    // argument.
    [[nodiscard]] std::string embedded(const Span span, const std::string_view unquote) const {
        std::string value;
        const auto text = raw(span);
        std::size_t depth = 0U;
        for (std::size_t position = 0U; position < text.size();) {
            const auto step = next_unit(text, position);
            if (step.unit == Unit::escape && depth == 0U && step.end - position == 2U &&
                unquote.contains(text[position + 1U])) {
                value.push_back(text[position + 1U]);
            } else {
                if (step.unit == Unit::open) {
                    ++depth;
                } else if (step.unit == Unit::close && depth > 0U) {
                    --depth;
                }
                value.append(text.substr(position, step.end - position));
            }
            position = step.end;
        }
        return value;
    }

    [[nodiscard]] std::string expression(const Span span) const { return embedded(span, ","); }
    [[nodiscard]] std::string pattern(const Span span) const { return embedded(span, ",)$"); }

    [[nodiscard]] bool arity(const Call& call, const std::size_t least, const std::size_t most,
                             const std::string_view shape) {
        if (call.arguments.size() < least || call.arguments.size() > most) {
            error(call.offset, "Write it as " + std::string{shape});
            return false;
        }
        return true;
    }

    [[nodiscard]] std::optional<std::vector<std::string>> literals(const Call& call,
                                                                   const std::size_t from) {
        std::vector<std::string> values;
        for (auto index = from; index < call.arguments.size(); ++index) {
            auto value = literal(call.arguments[index]);
            if (!value) {
                return std::nullopt;
            }
            values.push_back(std::move(*value));
        }
        return values;
    }

    [[nodiscard]] std::optional<std::vector<std::string>> fields(const Call& call) {
        std::vector<std::string> names;
        for (const auto& argument : call.arguments) {
            auto name = field(argument);
            if (!name) {
                return std::nullopt;
            }
            names.push_back(std::move(*name));
        }
        return names;
    }

    [[nodiscard]] std::optional<MetadataFieldMatchMode> match_mode(const Call& call) {
        if (call.arguments.size() < 2U) {
            return MetadataFieldMatchMode::logical;
        }
        const auto word = literal(call.arguments[1]);
        if (!word) {
            return std::nullopt;
        }
        const auto mode = ascii_lower(trim_ascii(*word));
        if (mode == "exact") {
            return MetadataFieldMatchMode::exact_native;
        }
        if (mode == "logical") {
            return MetadataFieldMatchMode::logical;
        }
        error(call.arguments[1].begin, "Write exact, or leave it out to match any spelling");
        return std::nullopt;
    }

    void add(const Call& call, MetadataTransformationAction action) {
        const MetadataTransformationChain chain{
            .schema_version = 1U,
            .name = "Raw script",
            .actions = {action},
        };
        if (const auto valid = validate_metadata_transformation_chain(chain); !valid) {
            error(call.offset, valid.error().message);
            return;
        }
        result_.actions.push_back(std::move(action));
    }

    void translate(const Call& call) {
        const auto& name = call.name;
        const auto& arguments = call.arguments;
        if (name == "set") {
            if (!arity(call, 2U, 2U, "$set(FIELD,tkfmt-1 value)")) {
                return;
            }
            if (auto target = field(arguments[0])) {
                add(call, MetadataFormatValueAction{
                              .target_field = std::move(*target),
                              .dialect = {},
                              .source = expression(arguments[1]),
                          });
            }
        } else if (name == "setvalues" || name == "addvalues") {
            if (!arity(call, 1U, std::numeric_limits<std::size_t>::max(),
                       "$" + name + "(FIELD,value,...)")) {
                return;
            }
            auto target = field(arguments[0]);
            auto values = target ? literals(call, 1U) : std::nullopt;
            if (!values) {
                return;
            }
            if (name == "setvalues") {
                add(call, MetadataSetValuesAction{.target_field = std::move(*target),
                                                  .values = std::move(*values)});
            } else {
                add(call, MetadataAddValuesAction{.target_field = std::move(*target),
                                                  .values = std::move(*values)});
            }
        } else if (name == "delete") {
            if (!arity(call, 1U, 2U, "$delete(FIELD) or $delete(FIELD,exact)")) {
                return;
            }
            auto target = field(arguments[0]);
            const auto mode = target ? match_mode(call) : std::nullopt;
            if (mode) {
                add(call, MetadataRemoveFieldAction{.target_field = std::move(*target),
                                                    .match_mode = *mode});
            }
        } else if (name == "if") {
            translate_conditional(call);
        } else if (name == "transform") {
            if (!arity(call, 2U, 2U, "$transform(FIELD,trim|lower|upper|capitalize)")) {
                return;
            }
            auto target = field(arguments[0]);
            const auto word = target ? literal(arguments[1]) : std::nullopt;
            if (!word) {
                return;
            }
            const auto kind = ascii_lower(trim_ascii(*word));
            std::optional<MetadataValueTransformKind> transform;
            if (kind == "trim") {
                transform = MetadataValueTransformKind::trim_ascii;
            } else if (kind == "lower") {
                transform = MetadataValueTransformKind::lowercase;
            } else if (kind == "upper") {
                transform = MetadataValueTransformKind::uppercase;
            } else if (kind == "capitalize") {
                transform = MetadataValueTransformKind::capitalize_first;
            }
            if (!transform) {
                error(arguments[1].begin, "Write trim, lower, upper or capitalize");
                return;
            }
            add(call, MetadataTransformValuesAction{.target_field = std::move(*target),
                                                    .transform = *transform});
        } else if (name == "copy") {
            if (!arity(call, 2U, 2U, "$copy(FIELD,FROM FIELD)")) {
                return;
            }
            auto target = field(arguments[0]);
            auto from = target ? field(arguments[1]) : std::nullopt;
            if (from) {
                add(call, MetadataCopyFieldAction{.target_field = std::move(*target),
                                                  .source_field = std::move(*from)});
            }
        } else if (name == "splitvalues" || name == "joinvalues") {
            if (!arity(call, 2U, 2U, "$" + name + "(FIELD,separator)")) {
                return;
            }
            auto target = field(arguments[0]);
            auto separator = target ? literal(arguments[1]) : std::nullopt;
            if (!separator) {
                return;
            }
            if (name == "splitvalues") {
                add(call, MetadataSplitValuesAction{.target_field = std::move(*target),
                                                    .separator = std::move(*separator)});
            } else {
                add(call, MetadataJoinValuesAction{.target_field = std::move(*target),
                                                   .separator = std::move(*separator)});
            }
        } else if (name == "removevalue") {
            if (!arity(call, 2U, 2U, "$removevalue(FIELD,exact value)")) {
                return;
            }
            auto target = field(arguments[0]);
            auto match = target ? literal(arguments[1]) : std::nullopt;
            if (match) {
                add(call, MetadataRemoveMatchingValuesAction{.target_field = std::move(*target),
                                                             .match = std::move(*match)});
            }
        } else if (name == "replacevalue") {
            if (!arity(call, 2U, std::numeric_limits<std::size_t>::max(),
                       "$replacevalue(FIELD,exact value,replacement,...)")) {
                return;
            }
            auto target = field(arguments[0]);
            auto match = target ? literal(arguments[1]) : std::nullopt;
            auto replacements = match ? literals(call, 2U) : std::nullopt;
            if (replacements) {
                add(call, MetadataReplaceMatchingValuesAction{
                              .target_field = std::move(*target),
                              .match = std::move(*match),
                              .replacement_values = std::move(*replacements),
                          });
            }
        } else if (name == "number") {
            if (!arity(call, 3U, 3U, "$number(FIELD,start,padding)")) {
                return;
            }
            auto target = field(arguments[0]);
            const auto start = target ? number(arguments[1]) : std::nullopt;
            const auto padding = start ? number(arguments[2]) : std::nullopt;
            if (padding) {
                add(call, MetadataNumberSelectedItemsAction{.target_field = std::move(*target),
                                                            .start = *start,
                                                            .padding = *padding});
            }
        } else if (name == "numberby") {
            if (!arity(call, 4U, 4U, "$numberby(FIELD,tkfmt-1 group,start,padding)")) {
                return;
            }
            auto target = field(arguments[0]);
            const auto start = target ? number(arguments[2]) : std::nullopt;
            const auto padding = start ? number(arguments[3]) : std::nullopt;
            if (padding) {
                add(call, MetadataNumberGroupedItemsAction{
                              .target_field = std::move(*target),
                              .dialect = {},
                              .group_expression = expression(arguments[1]),
                              .start = *start,
                              .padding = *padding,
                          });
            }
        } else if (name == "keepfirst") {
            if (!arity(call, 2U, 2U, "$keepfirst(FIELD,count)")) {
                return;
            }
            auto target = field(arguments[0]);
            const auto count = target ? number(arguments[1]) : std::nullopt;
            if (count) {
                add(call, MetadataKeepFirstCharactersAction{.target_field = std::move(*target),
                                                            .character_count = *count});
            }
        } else if (name == "rating") {
            if (!arity(call, 2U, 2U, "$rating(FROM FIELD,5|10|100)")) {
                return;
            }
            auto from = field(arguments[0]);
            const auto word = from ? literal(arguments[1]) : std::nullopt;
            if (!word) {
                return;
            }
            const auto scale = plain_rating_scale_named(trim_ascii(*word));
            if (!scale || *scale == PlainRatingScale::off) {
                error(arguments[1].begin, "Write the scale the rating is kept on: 5, 10 or 100");
                return;
            }
            add(call, MetadataConvertRatingAction{.target_field = std::string{fmps_rating_field},
                                                  .source_field = std::move(*from),
                                                  .scale = *scale});
        } else if (name == "capture") {
            translate_capture(call);
        } else if (name == "deletefields" || name == "keepfields") {
            if (!arity(call, 1U, std::numeric_limits<std::size_t>::max(),
                       "$" + name + "(FIELD,...)")) {
                return;
            }
            auto names = fields(call);
            if (!names) {
                return;
            }
            if (name == "deletefields") {
                add(call, MetadataBlocklistFieldsAction{.fields = std::move(*names)});
            } else {
                add(call, MetadataAllowlistFieldsAction{.fields = std::move(*names)});
            }
        } else {
            error(call.offset, "$" + name + " is not a step; see the steps a script can have");
        }
    }

    // $if(condition,$delete(A)$delete(B)...): each field removed when the
    // condition is not empty -- one typed step per field.
    void translate_conditional(const Call& call) {
        if (!arity(call, 2U, 2U, "$if(tkfmt-1 condition,$delete(FIELD)...)")) {
            return;
        }
        const auto condition = expression(call.arguments[0]);
        const auto body = statements(call.arguments[1]);
        if (!body) {
            return;
        }
        if (body->empty()) {
            error(call.offset, "$if needs a $delete(FIELD) to do when the condition holds");
            return;
        }
        for (const auto& inner : *body) {
            if (inner.name != "delete") {
                error(inner.offset, "Only $delete(FIELD) can be conditional; for a value, "
                                    "write $set(FIELD,$if(...))");
                return;
            }
            if (!arity(inner, 1U, 2U, "$delete(FIELD) or $delete(FIELD,exact)")) {
                return;
            }
            auto target = field(inner.arguments[0]);
            const auto mode = target ? match_mode(inner) : std::nullopt;
            if (!mode) {
                return;
            }
            add(inner, MetadataRemoveFieldIfAction{
                           .target_field = std::move(*target),
                           .dialect = {},
                           .condition = condition,
                           .match_mode = *mode,
                       });
        }
    }

    void translate_capture(const Call& call) {
        if (call.arguments.empty()) {
            error(call.offset, "Write it as $capture(filename|path,pattern), "
                               "$capture(formatted,tkfmt-1 text,pattern) or "
                               "$capture(field,FIELD,pattern)");
            return;
        }
        const auto word = literal(call.arguments[0]);
        if (!word) {
            return;
        }
        const auto kind = ascii_lower(trim_ascii(*word));
        if (kind == "filename" || kind == "path") {
            if (!arity(call, 2U, 2U, "$capture(" + kind + ",pattern)")) {
                return;
            }
            add(call, MetadataCaptureValuesAction{
                          .dialect = {},
                          .source_kind = kind == "filename" ? MetadataCaptureSourceKind::filename
                                                            : MetadataCaptureSourceKind::full_path,
                          .source = {},
                          .pattern = pattern(call.arguments[1]),
                      });
        } else if (kind == "formatted") {
            if (!arity(call, 3U, 3U, "$capture(formatted,tkfmt-1 text,pattern)")) {
                return;
            }
            add(call, MetadataCaptureValuesAction{
                          .dialect = {},
                          .source_kind = MetadataCaptureSourceKind::formatted,
                          .source = expression(call.arguments[1]),
                          .pattern = pattern(call.arguments[2]),
                      });
        } else if (kind == "field") {
            if (!arity(call, 3U, 3U, "$capture(field,FIELD,pattern)")) {
                return;
            }
            if (auto from = field(call.arguments[1])) {
                add(call, MetadataCaptureValuesAction{
                              .dialect = {},
                              .source_kind = MetadataCaptureSourceKind::field,
                              .source = std::move(*from),
                              .pattern = pattern(call.arguments[2]),
                          });
            }
        } else {
            error(call.arguments[0].begin, "Write filename, path, formatted or field");
        }
    }

    void error(const std::size_t offset, std::string message) {
        std::size_t line = 1U;
        std::size_t line_start = 0U;
        for (std::size_t position = 0U; position < offset && position < source_.size();
             ++position) {
            if (source_[position] == '\n') {
                ++line;
                line_start = position + 1U;
            }
        }
        result_.diagnostics.push_back(MetadataRuleScriptDiagnostic{
            .severity = MetadataRuleScriptDiagnosticSeverity::error,
            .byte_offset = offset,
            .line = line,
            .column = offset - line_start + 1U,
            .message = std::move(message),
        });
    }

    std::string_view source_;
    const MetadataRuleScriptImportLimits& limits_;
    MetadataRuleScriptImportResult& result_;
};

// Plain text in an argument: everything that could mean something quoted.
[[nodiscard]] std::string quote_literal(const std::string_view value) {
    std::string quoted;
    quoted.reserve(value.size());
    for (const auto character : value) {
        if (character == '\\' || character == '$' || character == '%' || character == ',' ||
            character == '(' || character == ')') {
            quoted.push_back('\\');
        }
        quoted.push_back(character);
    }
    return quoted;
}

// An expression or pattern as an argument: as written, but for the
// characters in `quote` where they would end the argument.
[[nodiscard]] std::string quote_embedded(const std::string_view text,
                                         const std::string_view quote) {
    std::string quoted;
    std::size_t depth = 0U;
    for (std::size_t position = 0U; position < text.size();) {
        auto step = next_unit(text, position);
        // A pattern has no calls: its `$` is text, quoted with the rest.
        if (step.unit == Unit::open && quote.contains('$')) {
            step = {Unit::text, position + 1U};
        }
        if (depth == 0U && step.unit != Unit::escape && step.unit != Unit::field &&
            quote.contains(text[position])) {
            quoted.push_back('\\');
            quoted.push_back(text[position]);
            position = step.end;
            continue;
        }
        if (step.unit == Unit::open) {
            ++depth;
        } else if (step.unit == Unit::close && depth > 0U) {
            --depth;
        }
        quoted.append(text.substr(position, step.end - position));
        position = step.end;
    }
    return quoted;
}

// Each value after a comma: `,a,b`.
[[nodiscard]] std::string joined(const std::vector<std::string>& values) {
    std::string text;
    for (const auto& value : values) {
        text.push_back(',');
        text += quote_literal(value);
    }
    return text;
}

[[nodiscard]] std::string_view mode_suffix(const MetadataFieldMatchMode mode) {
    return mode == MetadataFieldMatchMode::exact_native ? ",exact" : "";
}

[[nodiscard]] core::Error unsupported(std::string message, const std::size_t index) {
    return core::Error{
        .code = core::ErrorCode::unsupported,
        .message = std::move(message),
        .context = {{.key = "action", .value = std::to_string(index)}},
    };
}

} // namespace

MetadataRuleScriptImportResult
import_native_rule_script(const std::string_view source,
                          const MetadataRuleScriptImportLimits& limits) {
    MetadataRuleScriptImportResult result;
    Reader reader{source, limits, result};
    reader.read();
    if (result.has_errors()) {
        result.actions.clear();
    }
    return result;
}

core::Result<std::string>
export_native_rule_script(const std::span<const MetadataTransformationAction> actions) {
    std::string source;
    for (std::size_t index = 0U; index < actions.size(); ++index) {
        auto line = std::visit(
            [index](const auto& action) -> core::Result<std::string> {
                using Action = std::decay_t<decltype(action)>;
                const titleformat::DialectVersion tkfmt{};
                const auto other_dialect = [index] {
                    return std::unexpected(unsupported(
                        "This step uses an expression dialect other than tkfmt-1", index));
                };
                if constexpr (std::is_same_v<Action, MetadataSetValuesAction>) {
                    return "$setvalues(" + quote_literal(action.target_field) +
                           joined(action.values) + ')';
                } else if constexpr (std::is_same_v<Action, MetadataAddValuesAction>) {
                    return "$addvalues(" + quote_literal(action.target_field) +
                           joined(action.values) + ')';
                } else if constexpr (std::is_same_v<Action, MetadataRemoveFieldAction>) {
                    return "$delete(" + quote_literal(action.target_field) +
                           std::string{mode_suffix(action.match_mode)} + ')';
                } else if constexpr (std::is_same_v<Action, MetadataRemoveFieldIfAction>) {
                    if (action.dialect != tkfmt) {
                        return other_dialect();
                    }
                    return "$if(" + quote_embedded(action.condition, ",") + ",$delete(" +
                           quote_literal(action.target_field) +
                           std::string{mode_suffix(action.match_mode)} + "))";
                } else if constexpr (std::is_same_v<Action, MetadataTransformValuesAction>) {
                    const auto* kind =
                        action.transform == MetadataValueTransformKind::trim_ascii  ? "trim"
                        : action.transform == MetadataValueTransformKind::lowercase ? "lower"
                        : action.transform == MetadataValueTransformKind::uppercase ? "upper"
                                                                                    : "capitalize";
                    return "$transform(" + quote_literal(action.target_field) + ',' + kind + ')';
                } else if constexpr (std::is_same_v<Action, MetadataFormatValueAction>) {
                    if (action.dialect != tkfmt) {
                        return other_dialect();
                    }
                    return "$set(" + quote_literal(action.target_field) + ',' +
                           quote_embedded(action.source, ",") + ')';
                } else if constexpr (std::is_same_v<Action, MetadataCopyFieldAction>) {
                    return "$copy(" + quote_literal(action.target_field) + ',' +
                           quote_literal(action.source_field) + ')';
                } else if constexpr (std::is_same_v<Action, MetadataSplitValuesAction>) {
                    return "$splitvalues(" + quote_literal(action.target_field) + ',' +
                           quote_literal(action.separator) + ')';
                } else if constexpr (std::is_same_v<Action, MetadataJoinValuesAction>) {
                    return "$joinvalues(" + quote_literal(action.target_field) + ',' +
                           quote_literal(action.separator) + ')';
                } else if constexpr (std::is_same_v<Action, MetadataRemoveMatchingValuesAction>) {
                    return "$removevalue(" + quote_literal(action.target_field) + ',' +
                           quote_literal(action.match) + ')';
                } else if constexpr (std::is_same_v<Action, MetadataReplaceMatchingValuesAction>) {
                    return "$replacevalue(" + quote_literal(action.target_field) + ',' +
                           quote_literal(action.match) + joined(action.replacement_values) + ')';
                } else if constexpr (std::is_same_v<Action, MetadataNumberSelectedItemsAction>) {
                    return "$number(" + quote_literal(action.target_field) + ',' +
                           std::to_string(action.start) + ',' + std::to_string(action.padding) +
                           ')';
                } else if constexpr (std::is_same_v<Action, MetadataNumberGroupedItemsAction>) {
                    if (action.dialect != tkfmt) {
                        return other_dialect();
                    }
                    return "$numberby(" + quote_literal(action.target_field) + ',' +
                           quote_embedded(action.group_expression, ",") + ',' +
                           std::to_string(action.start) + ',' + std::to_string(action.padding) +
                           ')';
                } else if constexpr (std::is_same_v<Action, MetadataKeepFirstCharactersAction>) {
                    return "$keepfirst(" + quote_literal(action.target_field) + ',' +
                           std::to_string(action.character_count) + ')';
                } else if constexpr (std::is_same_v<Action, MetadataCaptureValuesAction>) {
                    if (action.dialect != CapturePatternDialectVersion{}) {
                        return std::unexpected(unsupported(
                            "This step uses a capture dialect other than tkcapture-1", index));
                    }
                    const auto pattern = quote_embedded(action.pattern, ",)$");
                    switch (action.source_kind) {
                    case MetadataCaptureSourceKind::filename:
                        return "$capture(filename," + pattern + ')';
                    case MetadataCaptureSourceKind::full_path:
                        return "$capture(path," + pattern + ')';
                    case MetadataCaptureSourceKind::formatted:
                        return "$capture(formatted," + quote_embedded(action.source, ",") + ',' +
                               pattern + ')';
                    case MetadataCaptureSourceKind::field:
                        return "$capture(field," + quote_literal(action.source) + ',' + pattern +
                               ')';
                    }
                    return std::unexpected(unsupported("Unknown capture source", index));
                } else if constexpr (std::is_same_v<Action, MetadataBlocklistFieldsAction>) {
                    auto text = joined(action.fields);
                    return "$deletefields(" + text.substr(text.empty() ? 0U : 1U) + ')';
                } else if constexpr (std::is_same_v<Action, MetadataAllowlistFieldsAction>) {
                    auto text = joined(action.fields);
                    return "$keepfields(" + text.substr(text.empty() ? 0U : 1U) + ')';
                } else if constexpr (std::is_same_v<Action, MetadataConvertRatingAction>) {
                    if (canonicalize_field_name(action.target_field) !=
                        canonicalize_field_name(fmps_rating_field)) {
                        return std::unexpected(
                            unsupported("This rating conversion writes a field other than "
                                        "FMPS_RATING",
                                        index));
                    }
                    return "$rating(" + quote_literal(action.source_field) + ',' +
                           std::string{plain_rating_scale_name(action.scale)} + ')';
                } else {
                    static_assert(!sizeof(Action), "every typed step has a statement");
                }
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

    // Exact or nothing: what is shown must be what is kept.
    const auto round_trip = import_native_rule_script(source);
    if (round_trip.has_errors() || !std::ranges::equal(round_trip.actions, actions)) {
        std::size_t differs = 0U;
        while (differs < actions.size() && differs < round_trip.actions.size() &&
               round_trip.actions[differs] == actions[differs]) {
            ++differs;
        }
        return std::unexpected(
            unsupported("This step's text would not read back as the same step",
                        std::min(differs, actions.empty() ? 0U : actions.size() - 1U)));
    }
    return source;
}

} // namespace trackknife::metadata
