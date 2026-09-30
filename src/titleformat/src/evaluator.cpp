// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/titleformat/evaluator.hpp"

#include "trackknife/core/unicode.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace trackknife::titleformat {
namespace {

using Integer = std::int64_t;
using ExpansionBindings = std::vector<std::pair<std::string, std::string>>;

[[nodiscard]] constexpr bool isAsciiWhitespace(const char character) noexcept {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
           character == '\f' || character == '\v';
}

[[nodiscard]] std::string asciiLower(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        result.push_back(character >= 'A' && character <= 'Z'
                             ? static_cast<char>(character - 'A' + 'a')
                             : character);
    }
    return result;
}

[[nodiscard]] core::Error makeError(const core::ErrorCode code, std::string message,
                                    const Program& program, const NodeId id) {
    const auto span = program.syntax().node(id).span;
    return core::Error{code, std::move(message), {}}
        .with_context("source_begin", std::to_string(span.begin))
        .with_context("source_end", std::to_string(span.end));
}

} // namespace

class Evaluator final {
  public:
    Evaluator(const Program& program, const EvaluationContext& context, EvaluationOptions options,
              const ExpansionBindings* expansion_bindings = nullptr)
        : program_(program), context_(context), options_(std::move(options)),
          expansion_bindings_(expansion_bindings) {}

    [[nodiscard]] core::Result<EvalValue> run() {
        if (context_.kind() != program_.context()) {
            return std::unexpected(core::Error{core::ErrorCode::invalid_argument,
                                               "evaluation context does not match compiled host",
                                               {}});
        }
        return evaluateNode(program_.syntax().root());
    }

  private:
    [[nodiscard]] core::Error error(const core::ErrorCode code, std::string message,
                                    const NodeId id) const {
        return makeError(code, std::move(message), program_, id);
    }

    [[nodiscard]] core::Result<EvalValue> checked(EvalValue value, const NodeId id) const {
        if (value.text.size() > options_.maximum_output_bytes) {
            return std::unexpected(error(core::ErrorCode::limit_exceeded,
                                         "format-expression output byte limit exceeded", id));
        }
        return value;
    }

    [[nodiscard]] core::Result<EvalValue> evaluateNode(const NodeId id) {
        if (options_.cancellation.is_cancellation_requested()) {
            return std::unexpected(
                error(core::ErrorCode::cancelled, "format-expression evaluation cancelled", id));
        }
        if (++steps_ > options_.maximum_steps) {
            return std::unexpected(error(core::ErrorCode::limit_exceeded,
                                         "format-expression step limit exceeded", id));
        }

        const auto& node = program_.syntax().node(id);
        return std::visit(
            [this, id](const auto& data) -> core::Result<EvalValue> {
                using Data = std::decay_t<decltype(data)>;
                if constexpr (std::is_same_v<Data, SequenceNode>) {
                    return evaluateSequence(data, id);
                } else if constexpr (std::is_same_v<Data, LiteralNode>) {
                    return checked({data.value}, id);
                } else if constexpr (std::is_same_v<Data, FieldNode>) {
                    return evaluateField(data, id);
                } else if constexpr (std::is_same_v<Data, CallNode>) {
                    return evaluateCall(data, id);
                } else {
                    return std::unexpected(error(core::ErrorCode::invalid_argument,
                                                 "recovered syntax cannot be evaluated", id));
                }
            },
            node.data);
    }

    [[nodiscard]] core::Result<EvalValue> evaluateSequence(const SequenceNode& sequence,
                                                           const NodeId id) {
        std::string result;
        for (const auto child : sequence.children) {
            auto value = evaluateNode(child);
            if (!value) {
                return std::unexpected(std::move(value.error()));
            }
            if (value->text.size() > options_.maximum_output_bytes - result.size()) {
                return std::unexpected(error(core::ErrorCode::limit_exceeded,
                                             "format-expression output byte limit exceeded", id));
            }
            result += value->text;
        }
        return EvalValue{std::move(result)};
    }

    [[nodiscard]] core::Result<std::string>
    joinValues(const EvaluationContext::MetadataValues& values, const std::string_view separator,
               const NodeId id) const {
        std::string result;
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (index != 0U) {
                if (separator.size() > options_.maximum_output_bytes - result.size()) {
                    return std::unexpected(error(core::ErrorCode::limit_exceeded,
                                                 "format-expression output byte limit exceeded",
                                                 id));
                }
                result += separator;
            }
            if (values.at(index).size() > options_.maximum_output_bytes - result.size()) {
                return std::unexpected(error(core::ErrorCode::limit_exceeded,
                                             "format-expression output byte limit exceeded", id));
            }
            result += values.at(index);
        }
        return result;
    }

    [[nodiscard]] core::Result<EvalValue> evaluateField(const FieldNode& field,
                                                        const NodeId id) const {
        const auto values = context_.resolveMetadata(field.name);
        if (!values) {
            return EvalValue{""};
        }
        auto joined = joinValues(*values, "; ", id);
        if (!joined) {
            return std::unexpected(std::move(joined.error()));
        }
        return EvalValue{std::move(*joined)};
    }

    [[nodiscard]] core::Result<EvalValue> evaluateCall(const CallNode& call, const NodeId id) {
        const auto function = program_.functionAt(id);
        if (!function) {
            return std::unexpected(
                error(core::ErrorCode::invariant, "compiled call has no function binding", id));
        }

        switch (*function) {
        case FunctionId::if_:
            return evaluateIf(call, id);
        case FunctionId::if2:
            return evaluateIf2(call);
        case FunctionId::and_:
            return evaluateAnd(call);
        case FunctionId::or_:
            return evaluateOr(call);
        case FunctionId::not_:
            return evaluateNot(call, id);
        case FunctionId::eq:
        case FunctionId::ne:
        case FunctionId::eqi:
            return evaluateEquality(*function, call, id);
        case FunctionId::gt:
        case FunctionId::gte:
        case FunctionId::lt:
        case FunctionId::lte:
            return evaluateIntegerComparison(*function, call, id);
        case FunctionId::add:
        case FunctionId::sub:
        case FunctionId::mul:
        case FunctionId::div:
        case FunctionId::mod:
        case FunctionId::min:
        case FunctionId::max:
            return evaluateIntegerFunction(*function, call, id);
        case FunctionId::num:
            return evaluateNum(call, id);
        case FunctionId::decimal:
            return evaluateDecimal(call, id);
        case FunctionId::lower:
            return evaluateCase(call, id, false);
        case FunctionId::upper:
            return evaluateCase(call, id, true);
        case FunctionId::trim:
            return evaluateTrim(call, id);
        case FunctionId::len:
            return evaluateLength(call, id);
        case FunctionId::left:
        case FunctionId::right:
            return evaluateSlice(*function, call, id);
        case FunctionId::longest:
            return evaluateLongest(call, id);
        case FunctionId::repeat:
            return evaluateRepeat(call, id);
        case FunctionId::replace:
            return evaluateReplace(call, id);
        case FunctionId::pad:
            return evaluatePad(call, id);
        case FunctionId::get:
            return evaluateGet(call, id, "; ");
        case FunctionId::getmulti:
            return evaluateGetMulti(call, id);
        case FunctionId::join:
            return evaluateJoin(call, id);
        case FunctionId::lenmulti:
            return evaluateMultiLength(call, id);
        case FunctionId::info:
            return evaluateInfo(call, id);
        case FunctionId::each:
            return evaluateEach(call, id);
        }
        return std::unexpected(
            error(core::ErrorCode::invariant, "unhandled compiled format function", id));
    }

    [[nodiscard]] core::Result<std::vector<EvalValue>> evaluateArguments(const CallNode& call) {
        std::vector<EvalValue> arguments;
        arguments.reserve(call.arguments.size());
        for (const auto argument : call.arguments) {
            auto value = evaluateNode(argument);
            if (!value) {
                return std::unexpected(std::move(value.error()));
            }
            arguments.push_back(std::move(*value));
        }
        return arguments;
    }

    [[nodiscard]] core::Result<EvalValue> evaluateIf(const CallNode& call, const NodeId id) {
        auto condition = evaluateNode(call.arguments.at(0));
        if (!condition) {
            return condition;
        }
        if (condition->truthy()) {
            return evaluateNode(call.arguments.at(1));
        }
        if (call.arguments.size() == 3U) {
            return evaluateNode(call.arguments.at(2));
        }
        return checked({""}, id);
    }

    [[nodiscard]] core::Result<EvalValue> evaluateIf2(const CallNode& call) {
        for (const auto argument : call.arguments) {
            auto value = evaluateNode(argument);
            if (!value || value->truthy()) {
                return value;
            }
        }
        return EvalValue{""};
    }

    [[nodiscard]] core::Result<EvalValue> evaluateAnd(const CallNode& call) {
        for (const auto argument : call.arguments) {
            auto value = evaluateNode(argument);
            if (!value) {
                return value;
            }
            if (!value->truthy()) {
                return EvalValue{""};
            }
        }
        return EvalValue{"1"};
    }

    [[nodiscard]] core::Result<EvalValue> evaluateOr(const CallNode& call) {
        for (const auto argument : call.arguments) {
            auto value = evaluateNode(argument);
            if (!value) {
                return value;
            }
            if (value->truthy()) {
                return EvalValue{"1"};
            }
        }
        return EvalValue{""};
    }

    [[nodiscard]] core::Result<EvalValue> evaluateNot(const CallNode& call, const NodeId id) {
        auto value = evaluateNode(call.arguments.at(0));
        if (!value) {
            return value;
        }
        return checked({value->truthy() ? "" : "1"}, id);
    }

    [[nodiscard]] core::Result<EvalValue> evaluateEquality(const FunctionId function,
                                                           const CallNode& call, const NodeId id) {
        auto arguments = evaluateArguments(call);
        if (!arguments) {
            return std::unexpected(std::move(arguments.error()));
        }
        bool equal = arguments->at(0).text == arguments->at(1).text;
        if (function == FunctionId::eqi) {
            auto comparison =
                core::unicodeSimpleCaseEqual(arguments->at(0).text, arguments->at(1).text);
            if (!comparison) {
                return std::unexpected(
                    error(comparison.error().code, comparison.error().message, id));
            }
            equal = *comparison;
        }
        if (function == FunctionId::ne) {
            equal = !equal;
        }
        return checked({equal ? "1" : ""}, id);
    }

    [[nodiscard]] core::Result<Integer> coerceInteger(const std::string_view text,
                                                      const NodeId id) const {
        std::size_t begin = 0;
        while (begin < text.size() && isAsciiWhitespace(text.at(begin))) {
            ++begin;
        }
        std::size_t end = text.size();
        while (end > begin && isAsciiWhitespace(text.at(end - 1U))) {
            --end;
        }
        if (begin == end) {
            return Integer{0};
        }

        bool negative = false;
        if (text.at(begin) == '+' || text.at(begin) == '-') {
            negative = text.at(begin) == '-';
            ++begin;
        }
        if (begin == end) {
            return Integer{0};
        }

        constexpr auto positive_limit =
            static_cast<std::uint64_t>(std::numeric_limits<Integer>::max());
        constexpr auto negative_limit = positive_limit + std::uint64_t{1};
        const auto limit = negative ? negative_limit : positive_limit;
        std::uint64_t magnitude = 0;
        for (auto index = begin; index < end; ++index) {
            const auto character = text.at(index);
            if (character < '0' || character > '9') {
                return Integer{0};
            }
            const auto digit = static_cast<std::uint64_t>(character - '0');
            if (magnitude > (limit - digit) / std::uint64_t{10}) {
                return std::unexpected(
                    error(core::ErrorCode::invalid_argument, "integer is out of range", id));
            }
            magnitude = magnitude * std::uint64_t{10} + digit;
        }

        if (!negative) {
            return static_cast<Integer>(magnitude);
        }
        if (magnitude == negative_limit) {
            return std::numeric_limits<Integer>::min();
        }
        return -static_cast<Integer>(magnitude);
    }

    [[nodiscard]] core::Result<Integer> checkedAdd(const Integer left, const Integer right,
                                                   const NodeId id) const {
        if ((right > 0 && left > std::numeric_limits<Integer>::max() - right) ||
            (right < 0 && left < std::numeric_limits<Integer>::min() - right)) {
            return std::unexpected(
                error(core::ErrorCode::invalid_argument, "integer result is out of range", id));
        }
        return left + right;
    }

    [[nodiscard]] core::Result<Integer> checkedSubtract(const Integer left, const Integer right,
                                                        const NodeId id) const {
        if ((right < 0 && left > std::numeric_limits<Integer>::max() + right) ||
            (right > 0 && left < std::numeric_limits<Integer>::min() + right)) {
            return std::unexpected(
                error(core::ErrorCode::invalid_argument, "integer result is out of range", id));
        }
        return left - right;
    }

    [[nodiscard]] core::Result<Integer> checkedMultiply(const Integer left, const Integer right,
                                                        const NodeId id) const {
        if (left == 0 || right == 0) {
            return Integer{0};
        }
        if ((left == -1 && right == std::numeric_limits<Integer>::min()) ||
            (right == -1 && left == std::numeric_limits<Integer>::min())) {
            return std::unexpected(
                error(core::ErrorCode::invalid_argument, "integer result is out of range", id));
        }
        if ((left > 0 && right > 0 && left > std::numeric_limits<Integer>::max() / right) ||
            (left > 0 && right < 0 && right < std::numeric_limits<Integer>::min() / left) ||
            (left < 0 && right > 0 && left < std::numeric_limits<Integer>::min() / right) ||
            (left < 0 && right < 0 && left < std::numeric_limits<Integer>::max() / right)) {
            return std::unexpected(
                error(core::ErrorCode::invalid_argument, "integer result is out of range", id));
        }
        return left * right;
    }

    [[nodiscard]] core::Result<Integer> checkedDivide(const Integer left, const Integer right,
                                                      const NodeId id) const {
        if (right == 0) {
            return std::unexpected(
                error(core::ErrorCode::invalid_argument, "division by zero", id));
        }
        if (left == std::numeric_limits<Integer>::min() && right == -1) {
            return std::unexpected(
                error(core::ErrorCode::invalid_argument, "integer result is out of range", id));
        }
        return left / right;
    }

    [[nodiscard]] core::Result<EvalValue>
    evaluateIntegerComparison(const FunctionId function, const CallNode& call, const NodeId id) {
        auto arguments = evaluateArguments(call);
        if (!arguments) {
            return std::unexpected(std::move(arguments.error()));
        }
        auto left = coerceInteger(arguments->at(0).text, id);
        auto right = coerceInteger(arguments->at(1).text, id);
        if (!left) {
            return std::unexpected(std::move(left.error()));
        }
        if (!right) {
            return std::unexpected(std::move(right.error()));
        }
        bool result = false;
        switch (function) {
        case FunctionId::gt:
            result = *left > *right;
            break;
        case FunctionId::gte:
            result = *left >= *right;
            break;
        case FunctionId::lt:
            result = *left < *right;
            break;
        case FunctionId::lte:
            result = *left <= *right;
            break;
        default:
            return std::unexpected(
                error(core::ErrorCode::invariant, "invalid integer comparison function", id));
        }
        return checked({result ? "1" : ""}, id);
    }

    [[nodiscard]] core::Result<EvalValue>
    evaluateIntegerFunction(const FunctionId function, const CallNode& call, const NodeId id) {
        auto arguments = evaluateArguments(call);
        if (!arguments) {
            return std::unexpected(std::move(arguments.error()));
        }
        std::vector<Integer> values;
        values.reserve(arguments->size());
        for (const auto& argument : *arguments) {
            auto value = coerceInteger(argument.text, id);
            if (!value) {
                return std::unexpected(std::move(value.error()));
            }
            values.push_back(*value);
        }

        if (function == FunctionId::add && values.empty()) {
            return EvalValue{"0"};
        }
        if (function == FunctionId::mul && values.empty()) {
            return EvalValue{"1"};
        }
        if (values.empty()) {
            return std::unexpected(
                error(core::ErrorCode::invariant, "integer function received no arguments", id));
        }

        auto result = values.front();
        for (std::size_t index = 1; index < values.size(); ++index) {
            core::Result<Integer> next = result;
            switch (function) {
            case FunctionId::add:
                next = checkedAdd(result, values.at(index), id);
                break;
            case FunctionId::sub:
                next = checkedSubtract(result, values.at(index), id);
                break;
            case FunctionId::mul:
                next = checkedMultiply(result, values.at(index), id);
                break;
            case FunctionId::div:
                next = checkedDivide(result, values.at(index), id);
                break;
            case FunctionId::min:
                next = std::min(result, values.at(index));
                break;
            case FunctionId::max:
                next = std::max(result, values.at(index));
                break;
            default:
                break;
            }
            if (!next) {
                return std::unexpected(std::move(next.error()));
            }
            result = *next;
        }

        if (function == FunctionId::mod) {
            if (values.at(1) == 0) {
                return std::unexpected(
                    error(core::ErrorCode::invalid_argument, "division by zero", id));
            }
            if (values.at(0) == std::numeric_limits<Integer>::min() && values.at(1) == -1) {
                result = 0;
            } else {
                result = values.at(0) % values.at(1);
            }
        }
        return checked({std::to_string(result)}, id);
    }

    // $decimal(value,divisor,places): the quotient as decimal text, rounded
    // half away from zero to exactly `places` digits after the point -- the
    // one way to a fraction in a language of whole numbers, as a rating on a
    // 0.0-1.0 scale needs.
    [[nodiscard]] core::Result<EvalValue> evaluateDecimal(const CallNode& call, const NodeId id) {
        auto arguments = evaluateArguments(call);
        if (!arguments) {
            return std::unexpected(std::move(arguments.error()));
        }
        auto value = coerceInteger(arguments->at(0).text, id);
        auto divisor = coerceInteger(arguments->at(1).text, id);
        auto places = coerceInteger(arguments->at(2).text, id);
        for (const auto* each : {&value, &divisor, &places}) {
            if (!*each) {
                return std::unexpected(std::move(each->error()));
            }
        }
        if (*divisor == 0) {
            return std::unexpected(error(core::ErrorCode::invalid_argument, "division by zero", id));
        }
        if (*places < 0 || *places > 9) {
            return std::unexpected(error(core::ErrorCode::invalid_argument,
                                         "$decimal places must be from 0 to 9", id));
        }
        // Scaled by 10^places, overflow an error as it is everywhere in
        // tkfmt-1; the rounding is decided on unsigned magnitudes, which
        // hold even the most negative integer.
        Integer scaled = *value;
        for (Integer place = 0; place < *places; ++place) {
            auto next = checkedMultiply(scaled, 10, id);
            if (!next) {
                return std::unexpected(std::move(next.error()));
            }
            scaled = *next;
        }
        const auto magnitude = [](const Integer number) {
            return number < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(number)
                              : static_cast<std::uint64_t>(number);
        };
        const auto top = magnitude(scaled);
        const auto bottom = magnitude(*divisor);
        auto quotient = top / bottom;
        const auto remainder = top % bottom;
        if (remainder >= bottom - remainder) {
            ++quotient;
        }
        const bool negative = quotient != 0U && ((scaled < 0) != (*divisor < 0));
        auto digits_left = quotient;
        std::string digits;
        do {
            digits.insert(digits.begin(), static_cast<char>('0' + digits_left % 10U));
            digits_left /= 10;
        } while (digits_left != 0);
        const auto fraction = static_cast<std::size_t>(*places);
        if (digits.size() <= fraction) {
            digits.insert(0U, fraction + 1U - digits.size(), '0');
        }
        if (fraction > 0U) {
            digits.insert(digits.size() - fraction, 1U, '.');
        }
        return checked({negative ? "-" + digits : digits}, id);
    }

    [[nodiscard]] core::Result<EvalValue> evaluateNum(const CallNode& call, const NodeId id) {
        auto arguments = evaluateArguments(call);
        if (!arguments) {
            return std::unexpected(std::move(arguments.error()));
        }
        auto value = coerceInteger(arguments->at(0).text, id);
        auto width = coerceInteger(arguments->at(1).text, id);
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        if (!width) {
            return std::unexpected(std::move(width.error()));
        }
        if (*width < 0) {
            return std::unexpected(
                error(core::ErrorCode::invalid_argument, "$num width cannot be negative", id));
        }
        auto result = std::to_string(*value);
        const auto requested = static_cast<std::uint64_t>(*width);
        if (requested > options_.maximum_output_bytes) {
            return std::unexpected(error(core::ErrorCode::limit_exceeded,
                                         "format-expression output byte limit exceeded", id));
        }
        const auto requested_size = static_cast<std::size_t>(requested);
        if (result.size() < requested_size) {
            const auto zeroes = requested_size - result.size();
            if (!result.empty() && result.front() == '-') {
                result.insert(1U, zeroes, '0');
            } else {
                result.insert(0U, zeroes, '0');
            }
        }
        return checked({std::move(result)}, id);
    }

    [[nodiscard]] core::Result<EvalValue> evaluateCase(const CallNode& call, const NodeId id,
                                                       const bool uppercase) {
        auto value = evaluateNode(call.arguments.at(0));
        if (!value) {
            return value;
        }
        auto result = uppercase ? core::unicodeSimpleUpper(value->text)
                                : core::unicodeSimpleLower(value->text);
        if (!result) {
            return std::unexpected(error(result.error().code, result.error().message, id));
        }
        return checked({std::move(*result)}, id);
    }

    [[nodiscard]] core::Result<EvalValue> evaluateTrim(const CallNode& call, const NodeId id) {
        auto value = evaluateNode(call.arguments.at(0));
        if (!value) {
            return value;
        }
        std::size_t begin = 0;
        while (begin < value->text.size() && isAsciiWhitespace(value->text.at(begin))) {
            ++begin;
        }
        std::size_t end = value->text.size();
        while (end > begin && isAsciiWhitespace(value->text.at(end - 1U))) {
            --end;
        }
        return checked({value->text.substr(begin, end - begin)}, id);
    }

    [[nodiscard]] core::Result<EvalValue> evaluateLength(const CallNode& call, const NodeId id) {
        auto value = evaluateNode(call.arguments.at(0));
        if (!value) {
            return value;
        }
        auto length = core::unicodeCodePointCount(value->text);
        if (!length) {
            return std::unexpected(error(length.error().code, length.error().message, id));
        }
        return checked({std::to_string(*length)}, id);
    }

    [[nodiscard]] core::Result<EvalValue> evaluateSlice(const FunctionId function,
                                                        const CallNode& call, const NodeId id) {
        auto arguments = evaluateArguments(call);
        if (!arguments) {
            return std::unexpected(std::move(arguments.error()));
        }
        auto count = coerceInteger(arguments->at(1).text, id);
        if (!count) {
            return std::unexpected(std::move(count.error()));
        }
        if (*count < 0) {
            return std::unexpected(
                error(core::ErrorCode::invalid_argument, "slice count cannot be negative", id));
        }
        auto length = core::unicodeCodePointCount(arguments->at(0).text);
        if (!length) {
            return std::unexpected(error(length.error().code, length.error().message, id));
        }
        const auto requested = static_cast<std::uint64_t>(*count);
        if (requested >= *length) {
            return checked({arguments->at(0).text}, id);
        }
        const auto requested_size = static_cast<std::size_t>(requested);
        const auto codepoint_offset = function == FunctionId::right ? *length - requested_size : 0U;
        const auto codepoint_end = function == FunctionId::right ? *length : requested_size;
        auto begin = core::unicodeByteOffset(arguments->at(0).text, codepoint_offset);
        auto end = core::unicodeByteOffset(arguments->at(0).text, codepoint_end);
        if (!begin) {
            return std::unexpected(error(begin.error().code, begin.error().message, id));
        }
        if (!end) {
            return std::unexpected(error(end.error().code, end.error().message, id));
        }
        return checked({arguments->at(0).text.substr(*begin, *end - *begin)}, id);
    }

    [[nodiscard]] core::Result<EvalValue> evaluateLongest(const CallNode& call, const NodeId id) {
        auto arguments = evaluateArguments(call);
        if (!arguments) {
            return std::unexpected(std::move(arguments.error()));
        }
        std::size_t selected = 0;
        std::size_t selected_length = 0;
        for (std::size_t index = 0; index < arguments->size(); ++index) {
            auto length = core::unicodeCodePointCount(arguments->at(index).text);
            if (!length) {
                return std::unexpected(error(length.error().code, length.error().message, id));
            }
            if (index == 0U || *length > selected_length) {
                selected = index;
                selected_length = *length;
            }
        }
        return checked({arguments->at(selected).text}, id);
    }

    [[nodiscard]] core::Result<EvalValue> evaluateRepeat(const CallNode& call, const NodeId id) {
        auto arguments = evaluateArguments(call);
        if (!arguments) {
            return std::unexpected(std::move(arguments.error()));
        }
        auto count = coerceInteger(arguments->at(1).text, id);
        if (!count) {
            return std::unexpected(std::move(count.error()));
        }
        if (*count < 0) {
            return std::unexpected(
                error(core::ErrorCode::invalid_argument, "$repeat count cannot be negative", id));
        }
        const auto unsigned_count = static_cast<std::uint64_t>(*count);
        const auto& source = arguments->at(0).text;
        if (!source.empty() && unsigned_count > options_.maximum_output_bytes / source.size()) {
            return std::unexpected(error(core::ErrorCode::limit_exceeded,
                                         "format-expression output byte limit exceeded", id));
        }
        std::string result;
        result.reserve(source.size() * static_cast<std::size_t>(unsigned_count));
        for (std::uint64_t index = 0; index < unsigned_count; ++index) {
            result += source;
        }
        return checked({std::move(result)}, id);
    }

    [[nodiscard]] core::Result<EvalValue> evaluateReplace(const CallNode& call, const NodeId id) {
        auto arguments = evaluateArguments(call);
        if (!arguments) {
            return std::unexpected(std::move(arguments.error()));
        }
        std::string result = arguments->at(0).text;
        for (std::size_t pair = 1; pair + 1U < arguments->size(); pair += 2U) {
            const auto& search = arguments->at(pair).text;
            const auto& replacement = arguments->at(pair + 1U).text;
            if (search.empty()) {
                return std::unexpected(error(core::ErrorCode::invalid_argument,
                                             "$replace search text cannot be empty", id));
            }
            std::size_t position = 0;
            while ((position = result.find(search, position)) != std::string::npos) {
                if (replacement.size() > search.size() &&
                    replacement.size() - search.size() >
                        options_.maximum_output_bytes - result.size()) {
                    return std::unexpected(error(core::ErrorCode::limit_exceeded,
                                                 "format-expression output byte limit exceeded",
                                                 id));
                }
                result.replace(position, search.size(), replacement);
                position += replacement.size();
            }
        }
        return checked({std::move(result)}, id);
    }

    [[nodiscard]] core::Result<EvalValue> evaluatePad(const CallNode& call, const NodeId id) {
        auto arguments = evaluateArguments(call);
        if (!arguments) {
            return std::unexpected(std::move(arguments.error()));
        }
        auto width = coerceInteger(arguments->at(1).text, id);
        if (!width) {
            return std::unexpected(std::move(width.error()));
        }
        if (*width < 0) {
            return std::unexpected(
                error(core::ErrorCode::invalid_argument, "$pad width cannot be negative", id));
        }
        const std::string_view fill = arguments->size() == 3U
                                          ? std::string_view{arguments->at(2).text}
                                          : std::string_view{" "};
        auto fill_length = core::unicodeCodePointCount(fill);
        if (!fill_length) {
            return std::unexpected(
                error(fill_length.error().code, fill_length.error().message, id));
        }
        if (*fill_length != 1U) {
            return std::unexpected(error(core::ErrorCode::invalid_argument,
                                         "$pad fill must be one Unicode scalar value", id));
        }
        auto length = core::unicodeCodePointCount(arguments->at(0).text);
        if (!length) {
            return std::unexpected(error(length.error().code, length.error().message, id));
        }
        const auto requested = static_cast<std::uint64_t>(*width);
        if (requested <= *length) {
            return checked({arguments->at(0).text}, id);
        }
        const auto count = requested - *length;
        if (count > (options_.maximum_output_bytes - arguments->at(0).text.size()) / fill.size()) {
            return std::unexpected(error(core::ErrorCode::limit_exceeded,
                                         "format-expression output byte limit exceeded", id));
        }
        std::string result = arguments->at(0).text;
        result.reserve(result.size() + static_cast<std::size_t>(count) * fill.size());
        for (std::uint64_t index = 0; index < count; ++index) {
            result += fill;
        }
        return checked({std::move(result)}, id);
    }

    [[nodiscard]] core::Result<EvalValue> evaluateGet(const CallNode& call, const NodeId id,
                                                      const std::string_view separator) {
        auto name = evaluateNode(call.arguments.at(0));
        if (!name) {
            return name;
        }
        const auto values = context_.resolveMetadata(name->text);
        if (!values) {
            return EvalValue{""};
        }
        auto joined = joinValues(*values, separator, id);
        if (!joined) {
            return std::unexpected(std::move(joined.error()));
        }
        return checked({std::move(*joined)}, id);
    }

    [[nodiscard]] core::Result<EvalValue> evaluateGetMulti(const CallNode& call, const NodeId id) {
        auto arguments = evaluateArguments(call);
        if (!arguments) {
            return std::unexpected(std::move(arguments.error()));
        }
        auto index = coerceInteger(arguments->at(1).text, id);
        if (!index) {
            return std::unexpected(std::move(index.error()));
        }
        if (*index < 0) {
            return EvalValue{""};
        }
        const auto values = context_.resolveMetadata(arguments->at(0).text);
        const auto unsigned_index = static_cast<std::uint64_t>(*index);
        if (!values || unsigned_index >= values->size()) {
            return EvalValue{""};
        }
        return checked({values->at(static_cast<std::size_t>(unsigned_index))}, id);
    }

    [[nodiscard]] core::Result<EvalValue> evaluateJoin(const CallNode& call, const NodeId id) {
        auto arguments = evaluateArguments(call);
        if (!arguments) {
            return std::unexpected(std::move(arguments.error()));
        }
        const auto values = context_.resolveMetadata(arguments->at(0).text);
        if (!values) {
            return EvalValue{""};
        }
        auto joined = joinValues(*values, arguments->at(1).text, id);
        if (!joined) {
            return std::unexpected(std::move(joined.error()));
        }
        return checked({std::move(*joined)}, id);
    }

    [[nodiscard]] core::Result<EvalValue> evaluateMultiLength(const CallNode& call,
                                                              const NodeId id) {
        auto name = evaluateNode(call.arguments.at(0));
        if (!name) {
            return name;
        }
        const auto values = context_.resolveMetadata(name->text);
        return checked({std::to_string(values ? values->size() : 0U)}, id);
    }

    [[nodiscard]] core::Result<EvalValue> evaluateInfo(const CallNode& call, const NodeId id) {
        auto name = evaluateNode(call.arguments.at(0));
        if (!name) {
            return name;
        }
        const auto information = context_.resolveTechnicalInfo(name->text);
        return checked({information.value_or("")}, id);
    }

    [[nodiscard]] core::Result<EvalValue> evaluateEach(const CallNode& call, const NodeId id) {
        if (expansion_bindings_ == nullptr) {
            return std::unexpected(error(core::ErrorCode::invalid_argument,
                                         "$each requires expanded tree evaluation", id));
        }
        auto name = evaluateNode(call.arguments.at(0));
        if (!name) {
            return name;
        }
        const auto normalized = asciiLower(name->text);
        const auto match = std::ranges::find(*expansion_bindings_, normalized,
                                             &ExpansionBindings::value_type::first);
        if (match == expansion_bindings_->end()) {
            return std::unexpected(
                error(core::ErrorCode::invariant, "expanded field has no evaluation binding", id));
        }
        return checked({match->second}, id);
    }

    const Program& program_;
    const EvaluationContext& context_;
    EvaluationOptions options_;
    std::size_t steps_{0};
    const ExpansionBindings* expansion_bindings_{nullptr};
};

core::Result<EvalValue> evaluate(const Program& program, const EvaluationContext& context,
                                 EvaluationOptions options) {
    if (program.hasExpansions()) {
        return std::unexpected(
            core::Error{core::ErrorCode::invalid_argument,
                        "tree expression contains $each; use expanded evaluation",
                        {}});
    }
    return Evaluator{program, context, std::move(options)}.run();
}

core::Result<std::vector<EvalValue>> evaluateExpanded(const Program& program,
                                                      const EvaluationContext& context,
                                                      EvaluationOptions options) {
    if (program.context() != FormatContextKind::tree_level ||
        context.kind() != FormatContextKind::tree_level) {
        return std::unexpected(
            core::Error{core::ErrorCode::invalid_argument,
                        "expanded evaluation requires a tree-level program and context",
                        {}});
    }
    if (!program.hasExpansions()) {
        auto scalar = evaluate(program, context, options);
        if (!scalar) {
            return std::unexpected(std::move(scalar.error()));
        }
        std::vector<EvalValue> result;
        result.push_back(std::move(*scalar));
        return result;
    }
    if (options.maximum_expanded_results == 0U) {
        return std::unexpected(core::Error{
            core::ErrorCode::limit_exceeded, "format-expression expansion limit is zero", {}});
    }

    std::vector<ExpansionBindings> combinations(1U);
    for (const auto& field : program.expansionDependencies()) {
        auto values = context.resolveMetadata(field);
        if (!values || values->empty()) {
            values = EvaluationContext::MetadataValues{""};
        }
        if (values->size() > options.maximum_expanded_results / combinations.size()) {
            return std::unexpected(core::Error{core::ErrorCode::limit_exceeded,
                                               "format-expression expanded-result limit exceeded",
                                               {}});
        }
        std::vector<ExpansionBindings> expanded;
        expanded.reserve(combinations.size() * values->size());
        for (const auto& combination : combinations) {
            for (const auto& value : *values) {
                auto next = combination;
                next.emplace_back(field, value);
                expanded.push_back(std::move(next));
            }
        }
        combinations = std::move(expanded);
    }

    std::vector<EvalValue> results;
    results.reserve(combinations.size());
    for (const auto& bindings : combinations) {
        auto value = Evaluator{program, context, options, &bindings}.run();
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        results.push_back(std::move(*value));
    }
    return results;
}

std::vector<core::Result<EvalValue>>
evaluateBatch(const Program& program, const std::span<const EvaluationContextRef> contexts,
              const EvaluationOptions options) {
    std::vector<core::Result<EvalValue>> results;
    results.reserve(contexts.size());
    for (const auto context : contexts) {
        results.push_back(evaluate(program, context.get(), options));
    }
    return results;
}

} // namespace trackknife::titleformat
