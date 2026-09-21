// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/persistence/tkq_row.hpp"

#include "library_query_internal.hpp"

#include "trackknife/core/unicode.hpp"
#include "trackknife/metadata/flac_mapping.hpp"
#include "trackknife/titleformat/evaluator.hpp"

#include <algorithm>
#include <charconv>
#include <utility>

namespace trackknife::persistence {
namespace internal {

const char* tkq_technical_column(const std::string& canonical) {
    if (canonical == "codec") {
        return "codec_name";
    }
    if (canonical == "samplerate") {
        return "sample_rate";
    }
    if (canonical == "bitspersample") {
        return "bits";
    }
    if (canonical == "channels") {
        return "channels";
    }
    if (canonical == "lengthms") {
        return "duration_ms";
    }
    return nullptr;
}

std::string tkq_canonical_field(const std::string& field) {
    return metadata::canonicalize_field_name(field);
}

} // namespace internal

namespace {

std::string lower(const std::string& value) {
    const auto result = core::unicodeSimpleLower(value);
    return result ? *result : value;
}

class RowFactsContext final : public titleformat::EvaluationContext {
  public:
    RowFactsContext(const TkqRowFacts& row, const titleformat::FormatContextKind kind)
        : row_(row), kind_(kind) {}
    titleformat::FormatContextKind kind() const noexcept override { return kind_; }
    std::optional<std::string> resolveField(std::string_view name) const override {
        const auto canonical = metadata::canonicalize_field_name(name);
        const auto found = row_.fields.find(canonical);
        if (found != row_.fields.end() && !found->second.empty()) {
            return found->second.front().first;
        }
        // The track row keeps display fallbacks even when the tag is absent.
        if (canonical == "title") {
            return row_.title;
        }
        if (canonical == "artist" || canonical == "albumartist") {
            return row_.artist;
        }
        if (canonical == "album") {
            return row_.album;
        }
        return std::nullopt;
    }
    std::optional<MetadataValues> resolveMetadata(std::string_view name) const override {
        const auto canonical = metadata::canonicalize_field_name(name);
        const auto found = row_.fields.find(canonical);
        if (found == row_.fields.end()) {
            return std::nullopt;
        }
        MetadataValues values;
        values.reserve(found->second.size());
        for (const auto& [original, normalized] : found->second) {
            static_cast<void>(normalized);
            values.push_back(original);
        }
        return values;
    }
    std::optional<std::string> resolveTechnicalInfo(std::string_view name) const override {
        const auto canonical = metadata::canonicalize_field_name(name);
        if (canonical == "codec") {
            return row_.codec.empty() ? std::nullopt : std::optional{row_.codec};
        }
        if (canonical == "samplerate" && row_.sample_rate > 0) {
            return std::to_string(row_.sample_rate);
        }
        if (canonical == "bitspersample" && row_.bits > 0) {
            return std::to_string(row_.bits);
        }
        if (canonical == "channels" && row_.channels > 0) {
            return std::to_string(row_.channels);
        }
        if (canonical == "lengthms" && row_.duration_ms >= 0) {
            return std::to_string(row_.duration_ms);
        }
        if (canonical == "rating" && row_.rating >= 0) {
            return std::to_string(row_.rating);
        }
        if (canonical == "albumrating" && row_.album_rating >= 0) {
            return std::to_string(row_.album_rating);
        }
        return std::nullopt;
    }

  private:
    const TkqRowFacts& row_;
    titleformat::FormatContextKind kind_;
};

[[nodiscard]] std::optional<std::int64_t> leading_integer(const std::string& text) {
    std::int64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr == text.data()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] bool compare_number(const std::int64_t value, const query::TkqComparison comparison,
                                  const std::int64_t operand) {
    switch (comparison) {
    case query::TkqComparison::greater:
        return value > operand;
    case query::TkqComparison::less:
        return value < operand;
    default:
        return value == operand;
    }
}

[[nodiscard]] bool evaluate_text_comparison(const std::string& normalized_text,
                                            const query::TkqPredicate& predicate) {
    using query::TkqComparison;
    switch (predicate.comparison) {
    case TkqComparison::is:
        return normalized_text == predicate.normalized;
    case TkqComparison::has:
        return std::ranges::all_of(predicate.words, [&](const std::string& word) {
            return normalized_text.find(word) != std::string::npos;
        });
    case TkqComparison::present:
        return !normalized_text.empty();
    case TkqComparison::missing:
        return normalized_text.empty();
    case TkqComparison::greater:
    case TkqComparison::less:
    case TkqComparison::equal: {
        const auto value = leading_integer(normalized_text);
        return value && compare_number(*value, predicate.comparison, predicate.number);
    }
    }
    return false;
}

[[nodiscard]] bool evaluate_predicate(const query::CompiledTkq& compiled,
                                      const query::TkqPredicate& predicate, const TkqRowFacts& row,
                                      const core::CancellationToken& cancellation) {
    using query::TkqComparison;
    using query::TkqOperandKind;
    if (predicate.operand == TkqOperandKind::expression) {
        const RowFactsContext context{row, titleformat::FormatContextKind::grouping};
        titleformat::EvaluationOptions options;
        options.cancellation = cancellation;
        const auto value =
            titleformat::evaluate(compiled.programs[predicate.program_index], context, options);
        if (!value) {
            return false;
        }
        return evaluate_text_comparison(lower(value->text), predicate);
    }
    if (predicate.operand == TkqOperandKind::any_field) {
        return std::ranges::all_of(predicate.words, [&](const std::string& word) {
            if (row.search_text.find(word) != std::string::npos) {
                return true;
            }
            for (const auto& [name, values] : row.fields) {
                static_cast<void>(name);
                for (const auto& [original, normalized] : values) {
                    static_cast<void>(original);
                    if (normalized.find(word) != std::string::npos) {
                        return true;
                    }
                }
            }
            return false;
        });
    }
    if (predicate.operand == TkqOperandKind::history) {
        constexpr std::array names{"playcount",      "lastplayed",      "dayssinceplayed",
                                   "albumplaycount", "albumlastplayed", "albumdayssinceplayed"};
        const auto found = std::ranges::find(names, predicate.field);
        const auto value = row.history && found != names.end()
                               ? (*row.history)[static_cast<std::size_t>(found - names.begin())]
                               : -1;
        const bool present = value >= 0;
        if (predicate.comparison == TkqComparison::present)
            return present;
        if (predicate.comparison == TkqComparison::missing)
            return !present;
        if (!present)
            return false;
        if (predicate.comparison == TkqComparison::is || predicate.comparison == TkqComparison::has)
            return evaluate_text_comparison(std::to_string(value), predicate);
        return compare_number(value, predicate.comparison, predicate.number);
    }
    const auto canonical = internal::tkq_canonical_field(predicate.field);
    if (canonical == "rating" || canonical == "albumrating") {
        // ADR-0179: the rating store shadows same-named tags, exactly like
        // the technical pseudo-fields shadow theirs.
        const auto value = canonical == "rating" ? row.rating : row.album_rating;
        const auto present = value >= 0;
        switch (predicate.comparison) {
        case TkqComparison::present:
            return present;
        case TkqComparison::missing:
            return !present;
        case TkqComparison::is:
        case TkqComparison::has:
            return present && evaluate_text_comparison(std::to_string(value), predicate);
        default:
            return present && compare_number(value, predicate.comparison, predicate.number);
        }
    }
    if (internal::tkq_technical_column(canonical) != nullptr) {
        if (canonical == "codec") {
            return evaluate_text_comparison(row.codec, predicate);
        }
        const auto value = canonical == "samplerate"      ? row.sample_rate
                           : canonical == "bitspersample" ? row.bits
                           : canonical == "channels"      ? row.channels
                                                          : row.duration_ms;
        const auto present = canonical == "lengthms" ? value >= 0 : value > 0;
        switch (predicate.comparison) {
        case TkqComparison::present:
            return present;
        case TkqComparison::missing:
            return !present;
        case TkqComparison::is:
        case TkqComparison::has:
            return present && evaluate_text_comparison(std::to_string(value), predicate);
        default:
            return present && compare_number(value, predicate.comparison, predicate.number);
        }
    }
    if (canonical == "date" && (predicate.comparison == TkqComparison::greater ||
                                predicate.comparison == TkqComparison::less ||
                                predicate.comparison == TkqComparison::equal)) {
        const auto year = leading_integer(row.date);
        return year && compare_number(*year, predicate.comparison, predicate.number);
    }
    const auto found = row.fields.find(canonical);
    switch (predicate.comparison) {
    case TkqComparison::present:
        return found != row.fields.end();
    case TkqComparison::missing:
        return found == row.fields.end();
    case TkqComparison::is:
        return found != row.fields.end() &&
               std::ranges::any_of(found->second, [&](const auto& value) {
                   return value.second == predicate.normalized;
               });
    case TkqComparison::has:
        return found != row.fields.end() &&
               std::ranges::all_of(predicate.words, [&](const std::string& word) {
                   return std::ranges::any_of(found->second, [&](const auto& value) {
                       return value.second.find(word) != std::string::npos;
                   });
               });
    case TkqComparison::greater:
    case TkqComparison::less:
    case TkqComparison::equal:
        return found != row.fields.end() &&
               std::ranges::any_of(found->second, [&](const auto& value) {
                   const auto number = leading_integer(value.second);
                   return number && compare_number(*number, predicate.comparison, predicate.number);
               });
    }
    return false;
}

[[nodiscard]] bool evaluate_node(const query::CompiledTkq& compiled, const std::size_t index,
                                 const TkqRowFacts& row,
                                 const core::CancellationToken& cancellation) {
    const auto& node = compiled.nodes[index];
    switch (node.kind) {
    case query::TkqNodeKind::predicate:
        return evaluate_predicate(compiled, compiled.predicates[node.predicate_index], row,
                                  cancellation);
    case query::TkqNodeKind::not_node:
        return !evaluate_node(compiled, node.children.front(), row, cancellation);
    case query::TkqNodeKind::and_node:
        return std::ranges::all_of(node.children, [&](const std::size_t child) {
            return evaluate_node(compiled, child, row, cancellation);
        });
    case query::TkqNodeKind::or_node:
        return std::ranges::any_of(node.children, [&](const std::size_t child) {
            return evaluate_node(compiled, child, row, cancellation);
        });
    }
    return false;
}

} // namespace

TkqRowFacts make_tkq_row_facts(const metadata::MetadataDocument& document, const std::string& title,
                               const std::string& artist, const std::string& album,
                               const std::optional<std::int64_t> duration_ms,
                               const std::optional<TkqRowTechnicals>& technicals) {
    TkqRowFacts facts;
    facts.title = title;
    facts.artist = artist;
    facts.album = album;
    facts.duration_ms = duration_ms.value_or(-1);
    if (technicals) {
        facts.codec = technicals->codec;
        facts.sample_rate = technicals->sample_rate;
        facts.bits = technicals->bits;
        facts.channels = technicals->channels;
    }
    for (const auto& field : document.fields) {
        if (field.canonical_name.empty()) {
            continue;
        }
        auto& values = facts.fields[field.canonical_name];
        for (const auto& value : field.values) {
            if (value.empty()) {
                continue;
            }
            values.emplace_back(value, lower(value));
        }
        if (values.empty()) {
            facts.fields.erase(field.canonical_name);
        }
    }
    if (const auto found = facts.fields.find("date");
        found != facts.fields.end() && !found->second.empty()) {
        facts.date = found->second.front().first;
    }
    facts.search_text = lower(title) + ' ' + lower(artist) + ' ' + lower(album);
    if (const auto found = facts.fields.find("artist"); found != facts.fields.end()) {
        for (const auto& [original, normalized] : found->second) {
            static_cast<void>(original);
            facts.search_text += ' ' + normalized;
        }
    }
    return facts;
}

bool tkq_matches(const query::CompiledTkq& compiled, const TkqRowFacts& facts,
                 const core::CancellationToken& cancellation) {
    if (compiled.match_all) {
        return true;
    }
    return evaluate_node(compiled, compiled.root, facts, cancellation);
}

core::Result<std::string> tkq_sort_key(const query::CompiledTkq& compiled, const TkqRowFacts& facts,
                                       const core::CancellationToken& cancellation) {
    if (!compiled.sort) {
        return std::string{};
    }
    const RowFactsContext context{facts, titleformat::FormatContextKind::sort};
    titleformat::EvaluationOptions options;
    options.cancellation = cancellation;
    auto value = titleformat::evaluate(compiled.sort->program, context, options);
    if (!value) {
        return std::unexpected(std::move(value.error()));
    }
    return lower(value->text);
}

} // namespace trackknife::persistence
