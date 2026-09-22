// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/query/tkq_melody.hpp"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>
#include <vector>

namespace trackknife::query {
namespace {

[[nodiscard]] core::Error unsupported(std::string message) {
    return core::Error{
        .code = core::ErrorCode::unsupported, .message = std::move(message), .context = {}};
}

// The parser lowercases fields but keeps separators; the server's condition
// tags are separator-free except the MusicBrainz identifiers.
[[nodiscard]] std::string canonical_field(const std::string& field) {
    std::string canonical;
    canonical.reserve(field.size());
    for (const auto character : field) {
        if (character == ' ' || character == '_' || character == '-') {
            continue;
        }
        canonical.push_back(character);
    }
    return canonical;
}

[[nodiscard]] std::string melody_condition_tag(const std::string& canonical) {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 7> renames{{
        {"musicbrainzartistid", "musicbrainz_artistid"},
        {"musicbrainzalbumid", "musicbrainz_albumid"},
        {"musicbrainzalbumartistid", "musicbrainz_albumartistid"},
        {"musicbrainztrackid", "musicbrainz_trackid"},
        {"musicbrainzreleasetrackid", "musicbrainz_releasetrackid"},
        {"musicbrainzreleasegroupid", "musicbrainz_releasegroupid"},
        {"musicbrainzworkid", "musicbrainz_workid"},
    }};
    const auto found =
        std::ranges::find(renames, canonical, [](const auto& rename) { return rename.first; });
    return found == renames.end() ? canonical : std::string{found->second};
}

[[nodiscard]] bool rating_field(const std::string& canonical) {
    return canonical == "rating" || canonical == "albumrating";
}

[[nodiscard]] bool technical_field(const std::string& canonical) {
    return canonical == "samplerate" || canonical == "bitspersample" || canonical == "channels" ||
           canonical == "lengthms";
}

[[nodiscard]] bool numeric_server_field(const std::string& canonical) {
    return rating_field(canonical) || technical_field(canonical);
}

[[nodiscard]] std::string escaped(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        if (character == '"' || character == '\\') {
            result.push_back('\\');
        }
        result.push_back(character);
    }
    return result;
}

// Joins already-parenthesized expressions with a connector, adding the
// grouping parentheses the server grammar requires for more than one.
[[nodiscard]] std::string joined_group(const std::vector<std::string>& expressions,
                                       const std::string_view connector) {
    if (expressions.size() == 1U) {
        return expressions.front();
    }
    std::string joined;
    for (const auto& expression : expressions) {
        if (!joined.empty()) {
            joined += " ";
            joined += connector;
            joined += " ";
        }
        joined += expression;
    }
    return "(" + joined + ")";
}

[[nodiscard]] core::Result<std::string> translate_predicate(const TkqPredicate& predicate,
                                                            const bool full_grammar) {
    if (predicate.operand == TkqOperandKind::history) {
        const auto tag = "history-" + predicate.field;
        const auto comparison = predicate.comparison;
        if (comparison == TkqComparison::has || comparison == TkqComparison::is)
            return std::unexpected(unsupported("HISTORY requires numeric or presence comparisons"));
        const auto op = comparison == TkqComparison::greater   ? ">"
                        : comparison == TkqComparison::less    ? "<"
                        : comparison == TkqComparison::present ? ">="
                        : comparison == TkqComparison::missing ? "<"
                                                               : "==";
        const auto number =
            comparison == TkqComparison::present || comparison == TkqComparison::missing
                ? 0
                : predicate.number;
        const auto expression = "(" + tag + " " + op + " " + std::to_string(number) + ")";
        if (comparison != TkqComparison::present && comparison != TkqComparison::missing &&
            predicate.field != "playcount" && predicate.field != "albumplaycount")
            return "((" + tag + " >= 0) AND " + expression + ")";
        return expression;
    }
    if (predicate.operand == TkqOperandKind::expression) {
        return std::unexpected(unsupported(
            "tkfmt expression predicates evaluate locally and cannot run on the server"));
    }
    if (predicate.operand == TkqOperandKind::any_field) {
        if (predicate.comparison != TkqComparison::has) {
            return std::unexpected(unsupported("`*` only supports HAS on the server"));
        }
        std::vector<std::string> words;
        words.reserve(predicate.words.size());
        for (const auto& word : predicate.words) {
            words.push_back("(any contains \"" + escaped(word) + "\")");
        }
        return joined_group(words, "AND");
    }

    const auto canonical = canonical_field(predicate.field);
    const auto server_numeric = numeric_server_field(canonical);
    const auto tag =
        canonical == "lengthms" ? std::string{"length"} : melody_condition_tag(canonical);
    switch (predicate.comparison) {
    case TkqComparison::has: {
        if (server_numeric) {
            return std::unexpected(
                unsupported("`" + predicate.field + "` only supports number comparisons"));
        }
        if (canonical == "date") {
            return std::unexpected(
                unsupported("the server matches date exactly; use IS or a year comparison "
                            "locally"));
        }
        std::vector<std::string> words;
        words.reserve(predicate.words.size());
        for (const auto& word : predicate.words) {
            words.push_back("(" + tag + " contains \"" + escaped(word) + "\")");
        }
        return joined_group(words, "AND");
    }
    case TkqComparison::is:
        if (server_numeric) {
            return "(" + tag + " == " + escaped(predicate.normalized) + ")";
        }
        return "(" + tag + " == \"" + escaped(predicate.text) + "\")";
    case TkqComparison::greater:
    case TkqComparison::less:
    case TkqComparison::equal: {
        if (!server_numeric && !full_grammar) {
            return std::unexpected(
                unsupported("this server compares numbers only on rating, albumrating, samplerate, "
                            "bitspersample, channels, and length_ms"));
        }
        auto number = predicate.number;
        if (canonical == "lengthms") {
            // The server stores whole seconds.
            number /= 1'000;
        }
        const auto rendered = std::to_string(number);
        if (predicate.comparison == TkqComparison::equal && !server_numeric) {
            // The server's == is string equality on ordinary tags; the
            // range pair reproduces tkq's leading-integer EQUAL.
            return "((" + tag + " >= " + rendered + ") AND (" + tag + " <= " + rendered + "))";
        }
        const auto* comparator = predicate.comparison == TkqComparison::greater ? ">"
                                 : predicate.comparison == TkqComparison::less  ? "<"
                                                                                : "==";
        return "(" + tag + " " + comparator + " " + rendered + ")";
    }
    case TkqComparison::present:
        if (rating_field(canonical)) {
            return "(" + tag + " >= 1)";
        }
        if (full_grammar && !technical_field(canonical) && canonical != "codec") {
            return "(" + tag + " != \"\")";
        }
        return std::unexpected(unsupported(
            full_grammar ? "PRESENT cannot run on the server for probe-derived technical fields"
                         : "PRESENT only translates for rating and albumrating on this server"));
    case TkqComparison::missing:
        if (!full_grammar) {
            return std::unexpected(
                unsupported("MISSING cannot run on this server; its filter grammar has no "
                            "negation"));
        }
        if (rating_field(canonical)) {
            return "(!(" + tag + " >= 1))";
        }
        if (technical_field(canonical) || canonical == "codec") {
            return std::unexpected(
                unsupported("MISSING cannot run on the server for probe-derived technical fields"));
        }
        return "(" + tag + " == \"\")";
    }
    return std::unexpected(unsupported("unsupported comparison"));
}

[[nodiscard]] core::Result<std::string> translate_node(const CompiledTkq& compiled,
                                                       std::size_t index, const bool full_grammar) {
    const auto& node = compiled.nodes[index];
    switch (node.kind) {
    case TkqNodeKind::predicate:
        return translate_predicate(compiled.predicates[node.predicate_index], full_grammar);
    case TkqNodeKind::and_node:
    case TkqNodeKind::or_node: {
        if (node.kind == TkqNodeKind::or_node && !full_grammar) {
            return std::unexpected(unsupported(
                "OR cannot run on this server; its filter grammar joins with AND only"));
        }
        std::vector<std::string> children;
        children.reserve(node.children.size());
        for (const auto child : node.children) {
            auto translated = translate_node(compiled, child, full_grammar);
            if (!translated) {
                return translated;
            }
            children.push_back(std::move(*translated));
        }
        return joined_group(children, node.kind == TkqNodeKind::or_node ? "OR" : "AND");
    }
    case TkqNodeKind::not_node: {
        if (!full_grammar) {
            return std::unexpected(
                unsupported("NOT cannot run on this server; its filter grammar has no negation"));
        }
        auto child = translate_node(compiled, node.children.front(), full_grammar);
        if (!child) {
            return child;
        }
        return "(!" + *child + ")";
    }
    }
    return std::unexpected(unsupported("unsupported query shape"));
}

[[nodiscard]] core::Result<std::string> translate_sort(const TkqSort& sort) {
    if (!sort.history.empty())
        return (sort.direction == TkqSortDirection::descending ? "-history-" : "history-") +
               sort.history;
    auto source = sort.source;
    const auto begin = source.find_first_not_of(" \t");
    const auto end = source.find_last_not_of(" \t");
    if (begin == std::string::npos) {
        return std::unexpected(unsupported("empty sort"));
    }
    source = source.substr(begin, end - begin + 1U);
    if (source.size() < 3U || source.front() != '%' || source.back() != '%') {
        return std::unexpected(
            unsupported("the server sorts by a single %field% only (artist, albumartist, "
                        "album, title, date, track, disc)"));
    }
    auto field = canonical_field(source.substr(1U, source.size() - 2U));
    std::ranges::transform(field, field.begin(), [](const char character) {
        return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                                    : character;
    });
    if (field == "tracknumber") {
        field = "track";
    }
    if (field == "discnumber") {
        field = "disc";
    }
    constexpr std::array sortable{"artist", "albumartist", "album", "title",
                                  "date",   "track",       "disc"};
    if (std::ranges::find(sortable, field) == sortable.end()) {
        return std::unexpected(unsupported("the server cannot sort by %" + field +
                                           "%; supported: artist, "
                                           "albumartist, album, title, date, track, disc"));
    }
    return (sort.direction == TkqSortDirection::descending ? "-" : "") + field;
}

} // namespace

core::Result<MelodyTranslatedQuery> translate_tkq_to_melody(const CompiledTkq& compiled,
                                                            const bool full_grammar,
                                                            const bool history_filters,
                                                            const bool history_sort) {
    if (compiled.sort && !compiled.sort->history.empty() && !history_sort)
        return std::unexpected(unsupported("This server does not advertise history sorting"));
    if (!history_filters && std::ranges::any_of(compiled.predicates, [](const auto& p) {
            return p.operand == TkqOperandKind::history;
        }))
        return std::unexpected(unsupported("This server does not advertise history filters"));
    MelodyTranslatedQuery translated;
    if (compiled.match_all) {
        translated.filter_expression = "(base \"\")";
    } else {
        auto expression = translate_node(compiled, compiled.root, full_grammar);
        if (!expression) {
            return std::unexpected(std::move(expression.error()));
        }
        translated.filter_expression = std::move(*expression);
    }
    if (compiled.sort) {
        auto sort = translate_sort(*compiled.sort);
        if (!sort) {
            return std::unexpected(std::move(sort.error()));
        }
        translated.sort = std::move(*sort);
    }
    return translated;
}

} // namespace trackknife::query
