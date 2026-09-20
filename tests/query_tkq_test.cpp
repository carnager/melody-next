// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/query/tkq.hpp"
#include "trackknife/query/tkq_melody.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

using trackknife::query::compile_tkq;
using trackknife::query::TkqComparison;
using trackknife::query::TkqNodeKind;
using trackknife::query::TkqOperandKind;
using trackknife::query::TkqSortDirection;

void simpleWordsBecomeAnAllWordSearch() {
    const auto compiled = compile_tkq("miles DAVIS Kind\tof blue");
    CHECK(compiled.has_value());
    if (!compiled) {
        return;
    }
    CHECK(!compiled->match_all);
    CHECK(compiled->predicates.size() == 1U);
    const auto& predicate = compiled->predicates.front();
    CHECK(predicate.operand == TkqOperandKind::any_field);
    CHECK(predicate.comparison == TkqComparison::has);
    CHECK(predicate.words == (std::vector<std::string>{"miles", "davis", "kind", "of", "blue"}));
    CHECK(compiled->source == "miles DAVIS Kind\tof blue");
    CHECK(compiled->dialect == "tkq");
    CHECK(compiled->dialect_version == 1);

    // Lowercase keyword spellings are ordinary words, not structure.
    const auto lowercase = compile_tkq("black and white");
    CHECK(lowercase.has_value());
    CHECK(lowercase && lowercase->predicates.size() == 1U &&
          lowercase->predicates.front().words ==
              (std::vector<std::string>{"black", "and", "white"}));
}

void structuredQueriesParseWithPrecedence() {
    const auto compiled =
        compile_tkq("genre HAS jazz AND date GREATER 1990 OR NOT artist IS \"Miles Davis\"");
    CHECK(compiled.has_value());
    if (!compiled) {
        return;
    }
    const auto& root = compiled->nodes[compiled->root];
    CHECK(root.kind == TkqNodeKind::or_node);
    CHECK(root.children.size() == 2U);
    CHECK(compiled->nodes[root.children.front()].kind == TkqNodeKind::and_node);
    CHECK(compiled->nodes[root.children.back()].kind == TkqNodeKind::not_node);
    CHECK(compiled->predicates.size() == 3U);
    CHECK(compiled->predicates[0].field == "genre");
    CHECK(compiled->predicates[0].comparison == TkqComparison::has);
    CHECK(compiled->predicates[1].field == "date");
    CHECK(compiled->predicates[1].comparison == TkqComparison::greater);
    CHECK(compiled->predicates[1].number == 1990);
    CHECK(compiled->predicates[2].comparison == TkqComparison::is);
    CHECK(compiled->predicates[2].text == "Miles Davis");
    CHECK(compiled->predicates[2].normalized == "miles davis");
    CHECK(compiled->field_dependencies() == (std::vector<std::string>{"genre", "date", "artist"}));

    // Parentheses override precedence.
    const auto grouped = compile_tkq("genre HAS jazz AND (date GREATER 1990 OR date MISSING)");
    CHECK(grouped.has_value());
    CHECK(grouped && grouped->nodes[grouped->root].kind == TkqNodeKind::and_node);
}

void quotingEscapesAndKeywordsInsideStrings() {
    const auto compiled = compile_tkq("title IS \"AND \"\"quoted\"\" OR\"");
    CHECK(compiled.has_value());
    CHECK(compiled && compiled->predicates.front().text == "AND \"quoted\" OR");

    const auto unterminated = compile_tkq("title IS \"open");
    CHECK(!unterminated.has_value());

    // A quoted operand is a field name even if it carries spaces.
    const auto spaced_field = compile_tkq("\"my field\" PRESENT");
    CHECK(spaced_field.has_value());
    CHECK(spaced_field && spaced_field->predicates.front().field == "my field");
}

void expressionOperandsCompileAsFormatPredicates() {
    const auto compiled = compile_tkq("\"%artist% - %title%\" HAS blue");
    CHECK(compiled.has_value());
    if (compiled) {
        CHECK(compiled->predicates.front().operand == TkqOperandKind::expression);
        CHECK(compiled->programs.size() == 1U);
    }
    const auto invalid = compile_tkq("\"$if(%a%\" HAS x");
    CHECK(!invalid.has_value());
}

void sortClauseSplitsFromTheExpression() {
    const auto compiled = compile_tkq("genre IS jazz SORT DESCENDING BY %album% - %title%");
    CHECK(compiled.has_value());
    if (!compiled) {
        return;
    }
    CHECK(compiled->sort.has_value());
    CHECK(compiled->sort && compiled->sort->direction == TkqSortDirection::descending);
    CHECK(compiled->sort && compiled->sort->source == "%album% - %title%");
    CHECK(compiled->predicates.size() == 1U);

    const auto plain = compile_tkq("ALL SORT BY %album%");
    CHECK(plain.has_value());
    CHECK(plain && plain->match_all && plain->sort.has_value() &&
          plain->sort->direction == TkqSortDirection::ascending);

    CHECK(!compile_tkq("genre IS jazz SORT BY").has_value());
    CHECK(!compile_tkq("genre IS jazz SORT %album%").has_value());
}

void strictErrorsNeverDegradeToWordSearch() {
    // A reserved keyword makes the query structured; malformed structure
    // is a hard error rather than a silent word search.
    CHECK(!compile_tkq("jazz AND").has_value());
    CHECK(!compile_tkq("genre HAS").has_value());
    CHECK(!compile_tkq("genre GREATER abc").has_value());
    CHECK(!compile_tkq("(genre IS jazz").has_value());
    CHECK(!compile_tkq("AND genre IS jazz").has_value());
    CHECK(!compile_tkq("* IS jazz").has_value());
    CHECK(!compile_tkq("* PRESENT").has_value());
    CHECK(!compile_tkq("ALL genre IS jazz").has_value());
    CHECK(!compile_tkq("").has_value());
    CHECK(!compile_tkq("   ").has_value());
    CHECK(!compile_tkq("genre IS jazz extra").has_value());

    // Diagnostics carry the offending span.
    const auto failed = compile_tkq("genre GREATER abc");
    CHECK(!failed.has_value());
    if (!failed) {
        bool has_span = false;
        for (const auto& entry : failed.error().context) {
            has_span = has_span || entry.key == "span_begin";
        }
        CHECK(has_span);
    }
}

void boundsFailClosed() {
    trackknife::query::TkqLimits limits;
    limits.maximum_source_bytes = 16U;
    CHECK(!compile_tkq("genre IS jazz AND artist IS someone", limits).has_value());

    limits = {};
    limits.maximum_nodes = 2U;
    CHECK(!compile_tkq("a IS b AND c IS d AND e IS f", limits).has_value());

    limits = {};
    limits.maximum_words = 2U;
    CHECK(!compile_tkq("one two three", limits).has_value());
}

} // namespace

void melodyTranslationCoversTheSupportedSubset() {
    using trackknife::query::translate_tkq_to_melody;
    const auto translate = [](const std::string_view source) {
        const auto compiled = compile_tkq(source);
        CHECK(compiled.has_value());
        return translate_tkq_to_melody(*compiled);
    };

    // Word searches, tag terms, and pseudo-fields join with AND.
    auto translated = translate("miles davis");
    CHECK(translated.has_value() && translated->filter_expression ==
                                        "((any contains \"miles\") AND (any contains \"davis\"))");
    translated = translate("genre HAS jazz AND rating GREATER 7");
    CHECK(translated.has_value() &&
          translated->filter_expression == "((genre contains \"jazz\") AND (rating > 7))");
    translated = translate("albumartist IS \"Bohren & der Club of Gore\"");
    CHECK(translated.has_value() &&
          translated->filter_expression == "(albumartist == \"Bohren & der Club of Gore\")");
    translated = translate("samplerate GREATER 48000 AND bitspersample EQUAL 24");
    CHECK(translated.has_value() &&
          translated->filter_expression == "((samplerate > 48000) AND (bitspersample == 24))");
    translated = translate("length_ms GREATER 600000");
    CHECK(translated.has_value() && translated->filter_expression == "(length > 600)");
    translated = translate("rating PRESENT");
    CHECK(translated.has_value() && translated->filter_expression == "(rating >= 1)");
    translated = translate("musicbrainz_albumid IS abc");
    CHECK(translated.has_value() &&
          translated->filter_expression == "(musicbrainz_albumid == \"abc\")");
    translated = translate("ALL");
    CHECK(translated.has_value() && translated->filter_expression == "(base \"\")");
    translated = translate("albumrating GREATER 7 SORT DESCENDING BY %date%");
    CHECK(translated.has_value() && translated->filter_expression == "(albumrating > 7)" &&
          translated->sort == "-date");
    translated = translate("artist HAS nick SORT BY %tracknumber%");
    CHECK(translated.has_value() && translated->sort == "track");

    // Untranslatable constructs are typed errors, never broadened queries.
    for (const auto* source : {"genre HAS jazz OR genre HAS blues", "NOT genre HAS jazz",
                               "rating MISSING", "date GREATER 1990", "genre GREATER 5",
                               "\"%artist% x\" HAS y", "genre HAS jazz SORT BY $lower(%artist%)"}) {
        const auto rejected = translate(source);
        CHECK(!rejected.has_value());
        if (!rejected.has_value()) {
            CHECK(rejected.error().code == trackknife::core::ErrorCode::unsupported);
        }
    }
}

void melodyFullGrammarTranslatesStructuredQueries() {
    using trackknife::query::translate_tkq_to_melody;
    const auto translate = [](const std::string_view source) {
        const auto compiled = compile_tkq(source);
        CHECK(compiled.has_value());
        return translate_tkq_to_melody(*compiled, true);
    };

    // OR, NOT, and nesting render the server's parenthesized grammar.
    auto translated = translate("genre HAS jazz OR genre HAS blues");
    CHECK(translated.has_value() &&
          translated->filter_expression ==
              "((genre contains \"jazz\") OR (genre contains \"blues\"))");
    translated = translate("NOT genre HAS jazz");
    CHECK(translated.has_value() &&
          translated->filter_expression == "(!(genre contains \"jazz\"))");
    translated = translate("(artist IS a AND genre HAS jazz) OR title IS b");
    CHECK(translated.has_value() &&
          translated->filter_expression ==
              "(((artist == \"a\") AND (genre contains \"jazz\")) OR (title == \"b\"))");

    // MPD's empty-value forms carry PRESENT and MISSING; ratings negate
    // their numeric form because 0 means unrated.
    translated = translate("genre MISSING");
    CHECK(translated.has_value() && translated->filter_expression == "(genre == \"\")");
    translated = translate("genre PRESENT");
    CHECK(translated.has_value() && translated->filter_expression == "(genre != \"\")");
    translated = translate("rating MISSING");
    CHECK(translated.has_value() && translated->filter_expression == "(!(rating >= 1))");

    // Numeric comparisons reach ordinary tags; EQUAL becomes the range
    // pair because the server's == is string equality there.
    translated = translate("date GREATER 1990 AND date LESS 2000");
    CHECK(translated.has_value() &&
          translated->filter_expression == "((date > 1990) AND (date < 2000))");
    translated = translate("date EQUAL 1994");
    CHECK(translated.has_value() &&
          translated->filter_expression == "((date >= 1994) AND (date <= 1994))");

    // Still impossible even at grammar level 2.
    for (const auto* source : {"\"%artist% x\" HAS y", "samplerate MISSING"}) {
        const auto rejected = translate(source);
        CHECK(!rejected.has_value());
        if (!rejected.has_value()) {
            CHECK(rejected.error().code == trackknife::core::ErrorCode::unsupported);
        }
    }
}

int main() {
    simpleWordsBecomeAnAllWordSearch();
    melodyTranslationCoversTheSupportedSubset();
    melodyFullGrammarTranslatesStructuredQueries();
    structuredQueriesParseWithPrecedence();
    quotingEscapesAndKeywordsInsideStrings();
    expressionOperandsCompileAsFormatPredicates();
    sortClauseSplitsFromTheExpression();
    strictErrorsNeverDegradeToWordSearch();
    boundsFailClosed();
    return failures == 0 ? 0 : 1;
}
