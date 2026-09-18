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
    const auto found = std::ranges::find(renames, canonical,
                                         [](const auto& rename) { return rename.first; });
    return found == renames.end() ? canonical : std::string{found->second};
}

[[nodiscard]] bool numeric_server_field(const std::string& canonical) {
    return canonical == "rating" || canonical == "albumrating" || canonical == "samplerate" ||
           canonical == "bitspersample" || canonical == "channels" || canonical == "lengthms";
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

[[nodiscard]] core::Result<void> translate_predicate(const TkqPredicate& predicate,
                                                     std::vector<std::string>& parts) {
    if (predicate.operand == TkqOperandKind::expression) {
        return std::unexpected(unsupported(
            "tkfmt expression predicates evaluate locally and cannot run on the server"));
    }
    if (predicate.operand == TkqOperandKind::any_field) {
        if (predicate.comparison != TkqComparison::has) {
            return std::unexpected(unsupported("`*` only supports HAS on the server"));
        }
        for (const auto& word : predicate.words) {
            parts.push_back("(any contains \"" + escaped(word) + "\")");
        }
        return {};
    }

    const auto canonical = canonical_field(predicate.field);
    const auto server_numeric = numeric_server_field(canonical);
    const auto tag =
        canonical == "lengthms" ? std::string{"length"} : melody_condition_tag(canonical);
    switch (predicate.comparison) {
    case TkqComparison::has:
        if (server_numeric) {
            return std::unexpected(
                unsupported("`" + predicate.field + "` only supports number comparisons"));
        }
        if (canonical == "date") {
            return std::unexpected(
                unsupported("the server matches date exactly; use IS or a year comparison "
                            "locally"));
        }
        for (const auto& word : predicate.words) {
            parts.push_back("(" + tag + " contains \"" + escaped(word) + "\")");
        }
        return {};
    case TkqComparison::is:
        if (server_numeric) {
            parts.push_back("(" + tag + " == " + escaped(predicate.normalized) + ")");
            return {};
        }
        parts.push_back("(" + tag + " == \"" + escaped(predicate.text) + "\")");
        return {};
    case TkqComparison::greater:
    case TkqComparison::less:
    case TkqComparison::equal: {
        if (!server_numeric) {
            return std::unexpected(unsupported(
                "the server compares numbers only on rating, albumrating, samplerate, "
                "bitspersample, channels, and length_ms"));
        }
        auto number = predicate.number;
        if (canonical == "lengthms") {
            // The server stores whole seconds.
            number /= 1'000;
        }
        const auto* comparator = predicate.comparison == TkqComparison::greater ? ">"
                                 : predicate.comparison == TkqComparison::less  ? "<"
                                                                                : "==";
        parts.push_back("(" + tag + " " + comparator + " " + std::to_string(number) + ")");
        return {};
    }
    case TkqComparison::present:
        if (canonical == "rating" || canonical == "albumrating") {
            parts.push_back("(" + tag + " >= 1)");
            return {};
        }
        return std::unexpected(
            unsupported("PRESENT only translates for rating and albumrating on the server"));
    case TkqComparison::missing:
        return std::unexpected(
            unsupported("MISSING cannot run on the server; its filter grammar has no NOT"));
    }
    return std::unexpected(unsupported("unsupported comparison"));
}

[[nodiscard]] core::Result<void> translate_node(const CompiledTkq& compiled, std::size_t index,
                                                std::vector<std::string>& parts) {
    const auto& node = compiled.nodes[index];
    switch (node.kind) {
    case TkqNodeKind::predicate:
        return translate_predicate(compiled.predicates[node.predicate_index], parts);
    case TkqNodeKind::and_node:
        for (const auto child : node.children) {
            if (auto translated = translate_node(compiled, child, parts); !translated) {
                return translated;
            }
        }
        return {};
    case TkqNodeKind::or_node:
        return std::unexpected(
            unsupported("OR cannot run on the server; its filter grammar joins with AND only"));
    case TkqNodeKind::not_node:
        return std::unexpected(
            unsupported("NOT cannot run on the server; its filter grammar has no negation"));
    }
    return std::unexpected(unsupported("unsupported query shape"));
}

[[nodiscard]] core::Result<std::string> translate_sort(const TkqSort& sort) {
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
        return std::unexpected(
            unsupported("the server cannot sort by %" + field + "%; supported: artist, "
                        "albumartist, album, title, date, track, disc"));
    }
    return (sort.direction == TkqSortDirection::descending ? "-" : "") + field;
}

} // namespace

core::Result<MelodyTranslatedQuery> translate_tkq_to_melody(const CompiledTkq& compiled) {
    MelodyTranslatedQuery translated;
    if (compiled.match_all) {
        translated.filter_expression = "(base \"\")";
    } else {
        std::vector<std::string> parts;
        if (auto result = translate_node(compiled, compiled.root, parts); !result) {
            return std::unexpected(std::move(result.error()));
        }
        if (parts.empty()) {
            return std::unexpected(unsupported("the query has no server-translatable terms"));
        }
        if (parts.size() == 1U) {
            translated.filter_expression = parts.front();
        } else {
            std::string joined;
            for (const auto& part : parts) {
                if (!joined.empty()) {
                    joined += " AND ";
                }
                joined += part;
            }
            translated.filter_expression = "(" + joined + ")";
        }
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
