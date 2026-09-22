// SPDX-License-Identifier: GPL-3.0-only

#include "tkq_internal.hpp"
#include "trackknife/query/tkq.hpp"

#include "trackknife/core/error.hpp"
#include "trackknife/core/unicode.hpp"

#include <algorithm>
#include <charconv>
#include <utility>

namespace trackknife::query {
namespace {

using internal::Token;
using internal::TokenKind;

[[nodiscard]] core::Error parse_error(std::string message, const std::size_t begin,
                                      const std::size_t end) {
    return core::Error{
        .code = core::ErrorCode::invalid_argument,
        .message = std::move(message),
        .context = {{.key = "span_begin", .value = std::to_string(begin)},
                    {.key = "span_end", .value = std::to_string(end)}},
    };
}

[[nodiscard]] std::string lower(const std::string& value) {
    const auto result = core::unicodeSimpleLower(value);
    return result ? *result : value;
}

[[nodiscard]] std::vector<std::string> split_words(const std::string& normalized) {
    std::vector<std::string> words;
    std::string current;
    for (const auto character : normalized) {
        if (character == ' ' || character == '\t' || character == '\n' || character == '\r') {
            if (!current.empty()) {
                words.push_back(std::move(current));
                current.clear();
            }
            continue;
        }
        current.push_back(character);
    }
    if (!current.empty()) {
        words.push_back(std::move(current));
    }
    return words;
}

// An operand that carries tkfmt syntax markers is an expression predicate.
[[nodiscard]] bool looks_like_expression(const std::string& text) {
    return text.find_first_of("%$#") != std::string::npos;
}

class Parser {
  public:
    Parser(std::string_view source, std::vector<Token> tokens, const TkqLimits& limits,
           CompiledTkq& output)
        : source_(source), tokens_(std::move(tokens)), limits_(limits), output_(output) {}

    [[nodiscard]] core::Result<void> run() {
        // A trailing SORT clause is split off before expression parsing:
        // everything after BY is tkfmt-1 source, taken verbatim.
        auto expression_end = tokens_.size();
        for (std::size_t index = 0U; index < tokens_.size(); ++index) {
            if (tokens_[index].kind == TokenKind::word && tokens_[index].keyword &&
                tokens_[index].text == "SORT") {
                if (auto sort = parse_sort(index); !sort) {
                    return std::unexpected(std::move(sort.error()));
                }
                expression_end = index;
                break;
            }
        }
        if (expression_end == 0U) {
            return std::unexpected(parse_error("the query is empty", 0U, source_.size()));
        }
        // ALL is the everything query; nothing may follow it.
        if (tokens_.front().keyword && tokens_.front().text == "ALL") {
            if (expression_end != 1U) {
                return std::unexpected(parse_error("ALL stands alone", tokens_[1].begin,
                                                   tokens_[expression_end - 1U].end));
            }
            output_.match_all = true;
            return {};
        }
        // Bare words without any reserved token are an all-word search.
        const auto structured = std::any_of(
            tokens_.begin(), tokens_.begin() + static_cast<std::ptrdiff_t>(expression_end),
            [](const Token& token) { return token.kind != TokenKind::word || token.keyword; });
        if (!structured) {
            std::string text;
            for (std::size_t index = 0U; index < expression_end; ++index) {
                if (!text.empty()) {
                    text += ' ';
                }
                text += tokens_[index].text;
            }
            auto predicate = make_words_predicate(TkqOperandKind::any_field, {}, text);
            if (!predicate) {
                return std::unexpected(std::move(predicate.error()));
            }
            output_.root = add_predicate_node(*predicate);
            return {};
        }
        position_ = 0U;
        end_ = expression_end;
        auto root = parse_or();
        if (!root) {
            return std::unexpected(std::move(root.error()));
        }
        if (position_ != end_) {
            return std::unexpected(parse_error("unexpected trailing input",
                                               tokens_[position_].begin, tokens_[end_ - 1U].end));
        }
        output_.root = *root;
        return {};
    }

  private:
    [[nodiscard]] core::Result<void> parse_sort(const std::size_t sort_index) {
        auto index = sort_index + 1U;
        TkqSort sort;
        if (index < tokens_.size() && tokens_[index].keyword &&
            (tokens_[index].text == "ASCENDING" || tokens_[index].text == "DESCENDING")) {
            sort.direction = tokens_[index].text == "DESCENDING" ? TkqSortDirection::descending
                                                                 : TkqSortDirection::ascending;
            ++index;
        }
        if (index < tokens_.size() && tokens_[index].kind == TokenKind::word &&
            tokens_[index].text == "HISTORY") {
            if (index + 4U != tokens_.size() || tokens_[index + 1U].kind != TokenKind::open_paren ||
                tokens_[index + 2U].kind != TokenKind::word ||
                tokens_[index + 3U].kind != TokenKind::close_paren)
                return std::unexpected(parse_error("SORT HISTORY needs one statistic name",
                                                   tokens_[index].begin, source_.size()));
            sort.history = lower(tokens_[index + 2U].text);
            if (sort.history != "playcount" && sort.history != "lastplayed" &&
                sort.history != "dayssinceplayed" && sort.history != "albumplaycount" &&
                sort.history != "albumlastplayed" && sort.history != "albumdayssinceplayed")
                return std::unexpected(
                    parse_error("Unknown HISTORY statistic", tokens_[index].begin, source_.size()));
            sort.source = std::string{source_.substr(tokens_[index].begin)};
            output_.sort = std::move(sort);
            return {};
        }
        if (index >= tokens_.size() || !tokens_[index].keyword || tokens_[index].text != "BY") {
            return std::unexpected(parse_error("SORT needs BY and a format expression",
                                               tokens_[sort_index].begin, tokens_[sort_index].end));
        }
        const auto source_begin = tokens_[index].end;
        auto text = std::string{source_.substr(source_begin)};
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return std::unexpected(parse_error("SORT BY needs a format expression",
                                               tokens_[index].begin, source_.size()));
        }
        text.erase(0U, first);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' ||
                                 text.back() == '\n')) {
            text.pop_back();
        }
        auto program = compile_format(text, titleformat::FormatContextKind::sort,
                                      tokens_[index].end, source_.size());
        if (!program) {
            return std::unexpected(std::move(program.error()));
        }
        sort.source = std::move(text);
        sort.program = std::move(*program);
        output_.sort = std::move(sort);
        return {};
    }

    [[nodiscard]] core::Result<titleformat::Program>
    compile_format(const std::string& text, const titleformat::FormatContextKind context,
                   const std::size_t begin, const std::size_t end) {
        const titleformat::CompileOptions options{
            .context = context, .dialect = {}, .parse_options = {}};
        auto compiled = titleformat::compile(text, options);
        if (!compiled.program) {
            auto message = std::string{"the format expression does not compile"};
            if (!compiled.diagnostics.empty()) {
                message += ": " + compiled.diagnostics.front().message;
            } else if (!compiled.parse_diagnostics.empty()) {
                message += ": " + compiled.parse_diagnostics.front().message;
            }
            return std::unexpected(parse_error(std::move(message), begin, end));
        }
        return std::move(*compiled.program);
    }

    [[nodiscard]] core::Result<std::size_t> parse_or() {
        auto left = parse_and();
        if (!left) {
            return left;
        }
        while (accept_keyword("OR")) {
            auto right = parse_and();
            if (!right) {
                return right;
            }
            left = add_node(TkqNodeKind::or_node, {*left, *right});
            if (!left) {
                return left;
            }
        }
        return left;
    }

    [[nodiscard]] core::Result<std::size_t> parse_and() {
        auto left = parse_unary();
        if (!left) {
            return left;
        }
        while (accept_keyword("AND")) {
            auto right = parse_unary();
            if (!right) {
                return right;
            }
            left = add_node(TkqNodeKind::and_node, {*left, *right});
            if (!left) {
                return left;
            }
        }
        return left;
    }

    [[nodiscard]] core::Result<std::size_t> parse_unary() {
        if (accept_keyword("NOT")) {
            auto operand = parse_unary();
            if (!operand) {
                return operand;
            }
            return add_node(TkqNodeKind::not_node, {*operand});
        }
        if (accept(TokenKind::open_paren)) {
            auto inner = parse_or();
            if (!inner) {
                return inner;
            }
            if (!accept(TokenKind::close_paren)) {
                return std::unexpected(current_error("a closing parenthesis is missing"));
            }
            return inner;
        }
        return parse_predicate();
    }

    [[nodiscard]] core::Result<std::size_t> parse_predicate() {
        if (position_ >= end_) {
            return std::unexpected(parse_error("the query ends where a predicate should start",
                                               source_.size(), source_.size()));
        }
        const auto operand_token = tokens_[position_];
        TkqOperandKind operand = TkqOperandKind::field;
        std::string field;
        if (operand_token.kind == TokenKind::star) {
            operand = TkqOperandKind::any_field;
            ++position_;
        } else if (operand_token.kind == TokenKind::word ||
                   operand_token.kind == TokenKind::quoted) {
            if (operand_token.kind == TokenKind::word && operand_token.keyword) {
                return std::unexpected(
                    parse_error("\"" + operand_token.text + "\" is a keyword, not a field",
                                operand_token.begin, operand_token.end));
            }
            if (looks_like_expression(operand_token.text)) {
                operand = TkqOperandKind::expression;
            } else {
                field = lower(operand_token.text);
            }
            ++position_;
        } else {
            return std::unexpected(current_error("a predicate should start here"));
        }

        // Explicit accessor: previously invalid syntax, so ordinary file fields
        // (including a tag named HISTORY or playcount) retain tkq-1 semantics.
        if (operand_token.kind == TokenKind::word && operand_token.text == "HISTORY" &&
            accept(TokenKind::open_paren)) {
            if (position_ >= end_ || tokens_[position_].kind != TokenKind::word)
                return std::unexpected(current_error("HISTORY needs a statistic name"));
            field = lower(tokens_[position_++].text);
            if (field != "playcount" && field != "lastplayed" && field != "dayssinceplayed" &&
                field != "albumplaycount" && field != "albumlastplayed" &&
                field != "albumdayssinceplayed")
                return std::unexpected(current_error("Unknown HISTORY statistic"));
            if (!accept(TokenKind::close_paren))
                return std::unexpected(current_error("HISTORY needs a closing parenthesis"));
            operand = TkqOperandKind::history;
        }

        if (position_ >= end_ || tokens_[position_].kind != TokenKind::word ||
            !tokens_[position_].keyword) {
            return std::unexpected(parse_error(
                "the field needs an operator (HAS, IS, GREATER, LESS, EQUAL, PRESENT, MISSING)",
                operand_token.begin,
                position_ < end_ ? tokens_[position_].end : operand_token.end));
        }
        const auto operator_token = tokens_[position_];
        ++position_;

        TkqPredicate predicate;
        predicate.operand = operand;
        predicate.field = std::move(field);
        if (operand == TkqOperandKind::expression) {
            auto program =
                compile_format(operand_token.text, titleformat::FormatContextKind::grouping,
                               operand_token.begin, operand_token.end);
            if (!program) {
                return std::unexpected(std::move(program.error()));
            }
            predicate.program_index = output_.programs.size();
            output_.programs.push_back(std::move(*program));
        }

        const auto& name = operator_token.text;
        if (operand == TkqOperandKind::history && (name == "HAS" || name == "IS"))
            return std::unexpected(
                current_error("HISTORY requires numeric or presence comparisons"));
        if (name == "PRESENT" || name == "MISSING") {
            if (operand == TkqOperandKind::any_field) {
                return std::unexpected(parse_error("* cannot be tested for presence",
                                                   operand_token.begin, operator_token.end));
            }
            predicate.comparison =
                name == "PRESENT" ? TkqComparison::present : TkqComparison::missing;
            return add_predicate(std::move(predicate));
        }
        if (name == "HAS" || name == "IS") {
            if (operand == TkqOperandKind::any_field && name == "IS") {
                return std::unexpected(
                    parse_error("* supports HAS only", operand_token.begin, operator_token.end));
            }
            auto value = take_text_value(operator_token);
            if (!value) {
                return std::unexpected(std::move(value.error()));
            }
            predicate.comparison = name == "HAS" ? TkqComparison::has : TkqComparison::is;
            predicate.text = std::move(*value);
            predicate.normalized = lower(predicate.text);
            if (predicate.comparison == TkqComparison::has) {
                predicate.words = split_words(predicate.normalized);
                if (predicate.words.empty()) {
                    return std::unexpected(parse_error("HAS needs at least one word",
                                                       operator_token.begin, operator_token.end));
                }
                if (predicate.words.size() > limits_.maximum_words) {
                    return std::unexpected(parse_error("the comparison carries more than " +
                                                           std::to_string(limits_.maximum_words) +
                                                           " words",
                                                       operator_token.begin, operator_token.end));
                }
            }
            return add_predicate(std::move(predicate));
        }
        if (name == "GREATER" || name == "LESS" || name == "EQUAL") {
            if (operand == TkqOperandKind::any_field) {
                return std::unexpected(
                    parse_error("* supports HAS only", operand_token.begin, operator_token.end));
            }
            auto value = take_text_value(operator_token);
            if (!value) {
                return std::unexpected(std::move(value.error()));
            }
            std::int64_t number = 0;
            const auto* begin = value->data();
            const auto* end = value->data() + value->size();
            const auto parsed = std::from_chars(begin, end, number);
            if (parsed.ec != std::errc{} || parsed.ptr != end) {
                return std::unexpected(parse_error("\"" + *value + "\" is not an integer",
                                                   operator_token.begin, operator_token.end));
            }
            predicate.comparison = name == "GREATER" ? TkqComparison::greater
                                   : name == "LESS"  ? TkqComparison::less
                                                     : TkqComparison::equal;
            predicate.number = number;
            return add_predicate(std::move(predicate));
        }
        return std::unexpected(parse_error("\"" + name + "\" is not an operator",
                                           operator_token.begin, operator_token.end));
    }

    [[nodiscard]] core::Result<std::string> take_text_value(const Token& operator_token) {
        // The comparison text is one token: a bare word or a quoted string.
        // Multi-word comparisons are quoted.
        if (position_ >= end_) {
            return std::unexpected(parse_error("the operator needs a value", operator_token.begin,
                                               operator_token.end));
        }
        const auto& token = tokens_[position_];
        if (token.kind == TokenKind::quoted) {
            ++position_;
            return token.text;
        }
        if (token.kind == TokenKind::word && !token.keyword) {
            ++position_;
            return token.text;
        }
        return std::unexpected(current_error("the operator needs a value"));
    }

    [[nodiscard]] core::Result<TkqPredicate>
    make_words_predicate(const TkqOperandKind operand, std::string field, std::string text) {
        TkqPredicate predicate;
        predicate.operand = operand;
        predicate.field = std::move(field);
        predicate.comparison = TkqComparison::has;
        predicate.text = std::move(text);
        predicate.normalized = lower(predicate.text);
        predicate.words = split_words(predicate.normalized);
        if (predicate.words.size() > limits_.maximum_words) {
            return std::unexpected(parse_error("the query carries more than " +
                                                   std::to_string(limits_.maximum_words) + " words",
                                               0U, source_.size()));
        }
        return predicate;
    }

    [[nodiscard]] core::Result<std::size_t> add_predicate(TkqPredicate predicate) {
        return add_predicate_node_checked(std::move(predicate));
    }

    [[nodiscard]] std::size_t add_predicate_node(TkqPredicate predicate) {
        output_.predicates.push_back(std::move(predicate));
        output_.nodes.push_back({TkqNodeKind::predicate, output_.predicates.size() - 1U, {}});
        return output_.nodes.size() - 1U;
    }

    [[nodiscard]] core::Result<std::size_t> add_predicate_node_checked(TkqPredicate predicate) {
        if (output_.nodes.size() >= limits_.maximum_nodes) {
            return std::unexpected(node_limit_error());
        }
        return add_predicate_node(std::move(predicate));
    }

    [[nodiscard]] core::Result<std::size_t> add_node(const TkqNodeKind kind,
                                                     std::vector<std::size_t> children) {
        if (output_.nodes.size() >= limits_.maximum_nodes) {
            return std::unexpected(node_limit_error());
        }
        output_.nodes.push_back({kind, 0U, std::move(children)});
        return output_.nodes.size() - 1U;
    }

    [[nodiscard]] core::Error node_limit_error() const {
        return parse_error("the query has more than " + std::to_string(limits_.maximum_nodes) +
                               " nodes",
                           0U, source_.size());
    }

    [[nodiscard]] bool accept(const TokenKind kind) {
        if (position_ < end_ && tokens_[position_].kind == kind) {
            ++position_;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool accept_keyword(const std::string_view name) {
        if (position_ < end_ && tokens_[position_].kind == TokenKind::word &&
            tokens_[position_].keyword && tokens_[position_].text == name) {
            ++position_;
            return true;
        }
        return false;
    }

    [[nodiscard]] core::Error current_error(std::string message) const {
        if (position_ < end_) {
            return parse_error(std::move(message), tokens_[position_].begin,
                               tokens_[position_].end);
        }
        return parse_error(std::move(message), source_.size(), source_.size());
    }

    std::string_view source_;
    std::vector<Token> tokens_;
    const TkqLimits& limits_;
    CompiledTkq& output_;
    std::size_t position_{0U};
    std::size_t end_{0U};
};

} // namespace

std::vector<std::string> CompiledTkq::field_dependencies() const {
    std::vector<std::string> names;
    for (const auto& predicate : predicates) {
        if (predicate.operand == TkqOperandKind::field &&
            std::ranges::find(names, predicate.field) == names.end()) {
            names.push_back(predicate.field);
        }
    }
    return names;
}

core::Result<CompiledTkq> compile_tkq_word_search(const std::string_view source,
                                                  const TkqLimits& limits) {
    if (source.size() > limits.maximum_source_bytes) {
        return std::unexpected(parse_error(
            "the query is longer than " + std::to_string(limits.maximum_source_bytes) + " bytes",
            0U, source.size()));
    }
    CompiledTkq compiled;
    compiled.source = std::string{source};
    TkqPredicate predicate;
    predicate.operand = TkqOperandKind::any_field;
    predicate.comparison = TkqComparison::has;
    predicate.text = compiled.source;
    predicate.normalized = lower(predicate.text);
    predicate.words = split_words(predicate.normalized);
    if (predicate.words.empty()) {
        return std::unexpected(parse_error("the query is empty", 0U, source.size()));
    }
    if (predicate.words.size() > limits.maximum_words) {
        return std::unexpected(parse_error("the query carries more than " +
                                               std::to_string(limits.maximum_words) + " words",
                                           0U, source.size()));
    }
    compiled.predicates.push_back(std::move(predicate));
    compiled.nodes.push_back({TkqNodeKind::predicate, 0U, {}});
    compiled.root = 0U;
    return compiled;
}

core::Result<CompiledTkq> compile_tkq(const std::string_view source, const TkqLimits& limits) {
    auto tokens = internal::lex_tkq(source, limits.maximum_source_bytes);
    if (!tokens) {
        return std::unexpected(std::move(tokens.error()));
    }
    if (tokens->empty()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "the query is empty",
            .context = {},
        });
    }
    CompiledTkq compiled;
    compiled.source = std::string{source};
    Parser parser{source, std::move(*tokens), limits, compiled};
    if (auto parsed = parser.run(); !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    return compiled;
}

} // namespace trackknife::query
