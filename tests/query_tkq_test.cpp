// SPDX-License-Identifier: GPL-3.0-only

#include "query/history_corpus.hpp"
#include "query/search_preset_corpus.hpp"
#include "trackknife/query/search_presets.hpp"
#include "trackknife/query/tkq.hpp"

#include <algorithm>
#include <iostream>
#include <set>
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

int main() {
    std::set<std::string_view> ids;
    std::set<std::string_view> topics;
    for (const auto& preset : trackknife::query::search_presets()) {
        CHECK(ids.insert(preset.id).second);
        topics.insert(preset.topic);
        const auto source = trackknife::query::preset_query(preset, preset.example);
        CHECK(source.has_value());
        if (!source)
            continue;
        const auto compiled = compile_tkq(*source);
        CHECK(compiled.has_value());
        if (preset.input == trackknife::query::PresetInput::integer) {
            CHECK(!trackknife::query::preset_query(preset, ""));
            CHECK(!trackknife::query::preset_query(preset, "1 OR ALL"));
            CHECK(!trackknife::query::preset_query(preset, std::to_string(preset.minimum - 1)));
            CHECK(!trackknife::query::preset_query(preset, std::to_string(preset.maximum + 1)));
        }
        if (preset.input == trackknife::query::PresetInput::text) {
            CHECK(!trackknife::query::preset_query(preset, ""));
            CHECK(!trackknife::query::preset_query(preset, std::string(1025, 'x')));
            const auto escaped =
                trackknife::query::preset_query(preset, "a\" OR artist PRESENT OR \"b");
            CHECK(escaped && compile_tkq(*escaped)->predicates.size() == 1U);
        }
        if (preset.id == "decade")
            CHECK(!trackknife::query::preset_query(preset, "1991"));
    }
    CHECK(topics.size() == 5U);
    for (const auto& test : search_preset_corpus::cases) {
        const auto presets = trackknife::query::search_presets();
        const auto found =
            std::ranges::find(presets, test.input_context, &trackknife::query::SearchPreset::id);
        CHECK(found != presets.end());
        if (found == presets.end())
            continue;
        const auto result = trackknife::query::preset_query(*found, test.source);
        CHECK(result && *result == test.expected);
    }
    // The corpus's expected outputs are the retired Melody translation; what
    // stays true is that every source still compiles.
    for (const auto& test : history_corpus::sort_cases) {
        CHECK(compile_tkq(test.source).has_value());
    }
    const auto literal_sort = compile_tkq("ALL SORT BY HISTORY");
    CHECK(literal_sort && literal_sort->sort->history.empty());
    const auto tag_sort = compile_tkq("ALL SORT BY %playcount%");
    CHECK(tag_sort && tag_sort->sort->history.empty());
    CHECK(!compile_tkq("ALL SORT HISTORY(unknown)"));
    CHECK(!compile_tkq("ALL SORT HISTORY(playcount) trailing"));
    CHECK(!compile_tkq("ALL SORT HISTORY(playcount"));
    for (const auto& test : history_corpus::cases) {
        CHECK(compile_tkq(test.source).has_value());
    }
    const auto history =
        compile_tkq("HISTORY(albumplaycount) EQUAL 0 OR HISTORY(albumdayssinceplayed) GREATER 180");
    CHECK(history.has_value());
    CHECK(history->predicates.front().operand == trackknife::query::TkqOperandKind::history);
    CHECK(!compile_tkq("HISTORY(unknown) EQUAL 0"));
    CHECK(!compile_tkq("HISTORY(playcount) HAS 1"));
    CHECK(compile_tkq("HISTORY IS tag")->predicates.front().operand ==
          trackknife::query::TkqOperandKind::field);
    simpleWordsBecomeAnAllWordSearch();
    structuredQueriesParseWithPrecedence();
    quotingEscapesAndKeywordsInsideStrings();
    expressionOperandsCompileAsFormatPredicates();
    sortClauseSplitsFromTheExpression();
    strictErrorsNeverDegradeToWordSearch();
    boundsFailClosed();
    return failures == 0 ? 0 : 1;
}
