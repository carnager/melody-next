// SPDX-License-Identifier: GPL-3.0-only
#include "trackknife/lists/list_view.hpp"

#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/unicode.hpp"
#include "trackknife/lists/edit_plan.hpp"
#include "trackknife/titleformat/evaluator.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <map>
#include <numeric>
#include <set>
#include <utility>

namespace trackknife::lists {
namespace {

using Json = nlohmann::json;
using titleformat::FormatContextKind;

constexpr auto dialect_name = "tkfmt-1";
constexpr std::size_t maximum_views = 256U;
constexpr std::size_t maximum_columns = 64U;
constexpr std::size_t maximum_expression_bytes = 4096U;

core::Error problem(std::string message, core::ErrorCode code = core::ErrorCode::invalid_argument) {
    return {.code = code, .message = std::move(message), .context = {}};
}

[[nodiscard]] std::string clock(const std::int64_t milliseconds) {
    const auto seconds = std::max<std::int64_t>(0, milliseconds / 1000);
    const auto hours = seconds / 3600;
    const auto minutes = seconds / 60 % 60;
    const auto rest = seconds % 60;
    const auto two = [](const std::int64_t value) {
        return (value < 10 ? "0" : "") + std::to_string(value);
    };
    return hours > 0 ? std::to_string(hours) + ":" + two(minutes) + ":" + two(rest)
                     : std::to_string(minutes) + ":" + two(rest);
}

// A moment as the history fields show it: local date and time, which sorts
// as it reads.
[[nodiscard]] std::string local_time(const std::int64_t utc_milliseconds) {
    const std::chrono::sys_time<std::chrono::seconds> moment{
        std::chrono::seconds{utc_milliseconds / 1000}};
    try {
        return std::format("{:%Y-%m-%d %H:%M}",
                           std::chrono::zoned_time{std::chrono::current_zone(), moment});
    } catch (const std::exception&) {
        // No time zone database: as UTC, still in order.
        return std::format("{:%Y-%m-%d %H:%M}", moment);
    }
}

// A row as a list's expressions read it (ADR-0282): its tags, the host
// fields the engine's formatting offers, its technicals and file by $info.
class ViewRowContext final : public titleformat::EvaluationContext {
  public:
    ViewRowContext(const ViewRow& row, const FormatContextKind kind,
                   const std::map<std::string, std::string>* group = nullptr)
        : row_(row), kind_(kind), group_(group) {}

    FormatContextKind kind() const noexcept override { return kind_; }

    std::optional<MetadataValues> resolveMetadata(std::string_view name) const override {
        const auto canonical = metadata::canonicalize_field_name(name);
        if (auto host = hosted(canonical)) {
            return host->empty() ? std::nullopt : std::optional{MetadataValues{std::move(*host)}};
        }
        if (row_.metadata != nullptr) {
            auto values = row_.metadata->effective_values(canonical);
            if (!values.empty()) {
                return values;
            }
        }
        // The row's display fallbacks, as its built-in columns show them.
        const auto fallback = [&]() -> std::string_view {
            if (canonical == "title") {
                return row_.title;
            }
            if (canonical == "artist") {
                return row_.artist;
            }
            if (canonical == "albumartist") {
                return row_.album_artist.empty() ? row_.artist : row_.album_artist;
            }
            if (canonical == "album") {
                return row_.album;
            }
            if (canonical == "date") {
                return row_.date;
            }
            if (canonical == "tracknumber") {
                return row_.track_number;
            }
            return {};
        }();
        if (!fallback.empty()) {
            return MetadataValues{std::string{fallback}};
        }
        if (canonical == "title" && !row_.raw_path.empty()) {
            return MetadataValues{core::escape_raw_path(
                std::filesystem::path{std::string{row_.raw_path}}.stem().native())};
        }
        return std::nullopt;
    }

    std::optional<std::string> resolveField(std::string_view name) const override {
        auto values = resolveMetadata(name);
        return values && !values->empty() ? std::optional{std::move(values->front())}
                                          : std::nullopt;
    }

    std::optional<std::string> resolveTechnicalInfo(std::string_view name) const override {
        const auto canonical = metadata::canonicalize_native_field_name(name);
        const auto positive = [](const std::int64_t value) {
            return value > 0 ? std::optional{std::to_string(value)} : std::nullopt;
        };
        if (canonical == "codec") {
            return row_.codec.empty() ? std::nullopt : std::optional{std::string{row_.codec}};
        }
        if (canonical == "samplerate") {
            return positive(row_.sample_rate);
        }
        if (canonical == "bitspersample") {
            return positive(row_.bits);
        }
        if (canonical == "channels") {
            return positive(row_.channels);
        }
        if (canonical == "bitrate") {
            return positive(row_.bit_rate / 1000);
        }
        if (canonical == "lengthms") {
            return row_.duration_ms ? std::optional{std::to_string(*row_.duration_ms)}
                                    : std::nullopt;
        }
        if (row_.raw_path.empty()) {
            return std::nullopt;
        }
        const std::filesystem::path path{std::string{row_.raw_path}};
        if (canonical == "path") {
            return core::escape_raw_path(row_.raw_path);
        }
        if (canonical == "filename") {
            return core::escape_raw_path(path.stem().native());
        }
        if (canonical == "filename_ext") {
            return core::escape_raw_path(path.filename().native());
        }
        if (canonical == "directory") {
            return core::escape_raw_path(path.parent_path().native());
        }
        if (canonical == "extension") {
            auto extension = path.extension().native();
            if (extension.starts_with('.')) {
                extension.erase(extension.begin());
            }
            return core::escape_raw_path(extension);
        }
        return std::nullopt;
    }

  private:
    // The host's own fields win over a same-named tag; empty is absent.
    [[nodiscard]] std::optional<std::string> hosted(const std::string& canonical) const {
        if (group_ != nullptr) {
            if (const auto found = group_->find(canonical); found != group_->end()) {
                return found->second;
            }
        }
        if (canonical == "length") {
            return row_.duration_ms ? clock(*row_.duration_ms) : std::string{};
        }
        if (canonical == "path") {
            return core::escape_raw_path(row_.raw_path);
        }
        if (canonical == "rating") {
            return row_.rating > 0U ? std::to_string(row_.rating) : std::string{};
        }
        // ADR-0283: when it came into the library, as a date and in days.
        const auto days_since = [](const std::int64_t seconds) {
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
            return std::to_string(std::max<std::int64_t>(0, (now - seconds) / 86'400));
        };
        if (canonical == "added" || canonical == "albumadded") {
            const auto seconds = canonical == "added" ? row_.added : row_.album_added;
            return seconds > 0 ? local_time(seconds * 1000) : std::string{};
        }
        if (canonical == "dayssinceadded" || canonical == "albumdayssinceadded") {
            const auto seconds = canonical == "dayssinceadded" ? row_.added : row_.album_added;
            return seconds > 0 ? days_since(seconds) : std::string{};
        }
        // Listening history, as the engine keeps it; absent where unknown.
        if (const auto found = std::ranges::find(history_field_names, canonical);
            found != history_field_names.end()) {
            const auto at = static_cast<std::size_t>(found - history_field_names.begin());
            const auto value = row_.history ? (*row_.history)[at] : -1;
            if (value < 0) {
                return std::string{};
            }
            return canonical.ends_with("lastplayed") ? local_time(value) : std::to_string(value);
        }
        return std::nullopt;
    }

    const ViewRow& row_;
    FormatContextKind kind_;
    const std::map<std::string, std::string>* group_;
};

[[nodiscard]] titleformat::EvaluationOptions options(const core::CancellationToken& cancellation) {
    return {.maximum_steps = 100'000U,
            .maximum_output_bytes = 64U * 1024U,
            .maximum_expanded_results = 1U,
            .cancellation = cancellation};
}

// Single-line, as every cell and header is (ADR-0027).
[[nodiscard]] std::string one_line(std::string text) {
    std::ranges::replace_if(text, [](const char c) { return c == '\n' || c == '\r'; }, ' ');
    return text;
}

[[nodiscard]] std::string format(const titleformat::Program& program,
                                 const titleformat::EvaluationContext& context,
                                 const core::CancellationToken& cancellation) {
    auto value = titleformat::evaluate(program, context, options(cancellation));
    return value ? one_line(std::move(value->text)) : std::string{};
}

[[nodiscard]] core::Result<titleformat::Program> compiled_program(const std::string& source,
                                                                  const FormatContextKind kind) {
    auto output =
        titleformat::compile(source, {.context = kind, .dialect = {}, .parse_options = {}});
    if (!output.isValid()) {
        return std::unexpected(problem(list_view_expression_error(source, kind)));
    }
    return std::move(*output.program);
}

[[nodiscard]] const char* artwork_name(const ViewArtwork artwork) {
    switch (artwork) {
    case ViewArtwork::beside:
        return "beside";
    case ViewArtwork::header:
        return "header";
    case ViewArtwork::none:
        return "none";
    }
    return "none";
}

[[nodiscard]] const char* grouping_name(const ViewGrouping grouping) {
    switch (grouping) {
    case ViewGrouping::none:
        return "none";
    case ViewGrouping::album:
        return "album";
    case ViewGrouping::expression:
        return "expression";
    }
    return "none";
}

[[nodiscard]] const char* alignment_name(const ColumnAlignment alignment) {
    switch (alignment) {
    case ColumnAlignment::left:
        return "left";
    case ColumnAlignment::right:
        return "right";
    case ColumnAlignment::center:
        return "center";
    }
    return "left";
}

[[nodiscard]] std::vector<ViewColumn>
builtin_columns(const std::initializer_list<std::string_view> shown) {
    std::vector<ViewColumn> columns;
    columns.reserve(builtin_column_ids.size());
    for (const auto id : builtin_column_ids) {
        columns.push_back({.id = std::string{id},
                           .visible = std::ranges::contains(shown, id),
                           .title = {},
                           .display = {},
                           .sort = {},
                           .alignment = ColumnAlignment::left,
                           .expands = false,
                           .style = {},
                           .second_line = {},
                           .width = 0});
    }
    return columns;
}

// ADR-0284: a list that says much in little width -- no column for what a
// second line can carry.
// A list's tracks a line each: who, the number, the title, the year, the
// album, the rating and the length -- numbers right, where they line up.
[[nodiscard]] std::vector<ViewColumn> plain_columns() {
    const auto shown =
        builtin_columns({"artist", "track-number", "title", "date", "album", "rating", "length"});
    std::vector<ViewColumn> columns;
    columns.reserve(shown.size());
    for (const auto id : {"artwork", "artist", "track-number", "title", "date", "album", "rating",
                          "length", "play-count", "last-played"}) {
        columns.push_back(*std::ranges::find(shown, std::string_view{id}, &ViewColumn::id));
    }
    const auto own = [&columns](const std::string_view id, std::string title, std::string display) {
        auto& column = *std::ranges::find(columns, id, &ViewColumn::id);
        column.title = std::move(title);
        column.display = std::move(display);
        column.alignment = ColumnAlignment::right;
    };
    own("track-number", "#", "$num(%tracknumber%,2)");
    own("date", "Date", "$left(%date%,4)");
    own("length", {}, "%length%");
    return columns;
}

[[nodiscard]] ListView two_line_tracks() {
    auto columns = builtin_columns({"rating"});
    // How often it played beneath its stars.
    const auto rating = std::ranges::find(columns, std::string_view{"rating"}, &ViewColumn::id);
    rating->second_line = "$if(%playcount%,%playcount% $if($eq(%playcount%,1),play,plays),)";
    rating->width = 96;
    // The song and its length, and beneath, whose and from where: a list
    // of tracks from everywhere, each saying what it is in one place.
    columns.insert(
        rating, ViewColumn{.id = "song",
                           .visible = true,
                           .title = "Song",
                           .display = "%title%$if(%length%, [%length%],)",
                           .sort = {},
                           .alignment = ColumnAlignment::left,
                           .expands = true,
                           .style = {},
                           .second_line = "%artist%$if(%album%, • %album%,)$if(%date%, [%date%],)",
                           .width = 0});
    return {.id = std::string{two_line_view_id},
            .name = "Two-line tracks",
            .artwork = ViewArtwork::beside,
            .grouping = ViewGrouping::none,
            .group_key = {},
            .group_title = {},
            .group_details = {},
            .subgroup = std::nullopt,
            .row_style = {},
            .columns = std::move(columns),
            .builtin = true,
            .row_lines = 2};
}

} // namespace

std::string_view builtin_column_expression(const std::string_view id) {
    static const std::map<std::string_view, std::string_view> expressions{
        {"artist", "%artist%"},
        {"track-number", "%tracknumber%"},
        {"title", "%title%"},
        {"album", "%album%"},
        {"date", "%date%"},
        {"length", "%length%"},
        {"play-count", "%playcount%"},
        {"last-played", "%lastplayed%"}};
    const auto found = expressions.find(id);
    return found == expressions.end() ? std::string_view{} : found->second;
}

bool CompiledListView::builtin_shows_expression(const std::string_view id) const {
    const auto found = std::ranges::find(builtin_ids_, id);
    return found != builtin_ids_.end() &&
           builtin_displays_[static_cast<std::size_t>(found - builtin_ids_.begin())].has_value();
}

bool is_builtin_column(const std::string_view id) {
    return std::ranges::contains(builtin_column_ids, id);
}

const std::vector<ListView>& shipped_list_views() {
    // Two, deliberately: albums with their covers beside them, and plain
    // columns. Everything else is a view of the user's own, made in the
    // editor -- the many shipped before were more menu than help.
    static const std::vector<ListView> views{
        {.id = std::string{albums_beside_view_id},
         .name = "Albums with side artwork",
         .artwork = ViewArtwork::beside,
         .grouping = ViewGrouping::album,
         .group_key = {},
         .group_title = {},
         .group_details = {},
         .subgroup = std::nullopt,
         .row_style = {},
         // The album's header says the artist, album and date; a track whose
         // artist differs says so after its title.
         .columns = builtin_columns({"artwork", "track-number", "title", "length"}),
         .builtin = true,
         .row_lines = 1},
        {.id = std::string{columns_view_id},
         .name = "Plain columns",
         .artwork = ViewArtwork::none,
         .grouping = ViewGrouping::none,
         .group_key = {},
         .group_title = {},
         .group_details = {},
         .subgroup = std::nullopt,
         .row_style = {},
         .columns = plain_columns(),
         .builtin = true,
         .row_lines = 1},
        two_line_tracks(),
    };
    return views;
}

std::optional<std::string_view> retired_list_view_replacement(const std::string_view id) {
    // Albums with their covers in the header strip are albums still.
    if (id == "albums-header-artwork") {
        return albums_beside_view_id;
    }
    static constexpr std::array retired{
        "compact-queue",     "grouped-by-artist", "grouped-by-genre", "grouped-by-label",
        "audio-details",     "listening-history", "grouped-by-year",  "grouped-by-decade",
        "grouped-by-folder", "classical",         "tag-check",        "grouped-by-quality",
        "recently-added"};
    if (std::ranges::contains(retired, id)) {
        return columns_view_id;
    }
    return std::nullopt;
}

const ListView* shipped_list_view(const std::string_view id) {
    const auto& views = shipped_list_views();
    const auto found = std::ranges::find(views, id, &ListView::id);
    return found == views.end() ? nullptr : &*found;
}

std::string list_view_expression_error(const std::string_view source,
                                       const FormatContextKind context) {
    if (source.size() > maximum_expression_bytes) {
        return "An expression may be at most 4096 bytes";
    }
    const auto compiled = titleformat::compile(
        std::string{source}, {.context = context, .dialect = {}, .parse_options = {}});
    if (compiled.isValid()) {
        return {};
    }
    auto message = !compiled.parse_diagnostics.empty() ? compiled.parse_diagnostics.front().message
                   : !compiled.diagnostics.empty()     ? compiled.diagnostics.front().message
                                                       : std::string{"Not a tkfmt-1 expression"};
    // The usual stumble: text in parentheses, which tkfmt-1 reads as syntax.
    if (message.find("parenthesis") != std::string::npos ||
        message.find("comma") != std::string::npos) {
        message += " — write a literal ( ) or , as \\( \\) or \\,";
    }
    return message;
}

std::vector<ViewProblem> validate_list_view(const ListView& view) {
    std::vector<ViewProblem> problems;
    const auto add = [&problems](std::string where, std::string message) {
        problems.push_back({.where = std::move(where), .message = std::move(message)});
    };
    const auto expression = [&add](const std::string& where, const std::string& source,
                                   const FormatContextKind kind, const bool required) {
        if (source.empty()) {
            if (required) {
                add(where, "Needs an expression");
            }
            return;
        }
        if (auto error = list_view_expression_error(source, kind); !error.empty()) {
            add(where, std::move(error));
        }
    };
    if (view.id.empty()) {
        add({}, "A view needs an id");
    }
    if (view.name.empty()) {
        add({}, "A view needs a name");
    }
    if (view.columns.size() > maximum_columns) {
        add({}, "A view may have at most 64 columns");
    }
    if (view.row_lines < 1 || view.row_lines > 3) {
        add({}, "A row has one to three lines");
    }
    std::set<std::string> ids;
    bool shown = false;
    for (const auto& column : view.columns) {
        if (column.id.empty() || !ids.insert(column.id).second) {
            add({}, "Every column needs an id of its own");
            continue;
        }
        expression("column:" + column.id + ":second_line", column.second_line,
                   FormatContextKind::list_item, false);
        shown = shown || (column.visible && column.id != "artwork");
        if (column.builtin()) {
            // ADR-0285: its own expressions, where it has them.
            expression("column:" + column.id + ":display", column.display,
                       FormatContextKind::list_item, false);
            expression("column:" + column.id + ":sort", column.sort, FormatContextKind::sort,
                       false);
            expression("column:" + column.id + ":style", column.style, FormatContextKind::list_item,
                       false);
            continue;
        }
        if (column.title.empty()) {
            add("column:" + column.id + ":title", "A column needs a title");
        }
        expression("column:" + column.id + ":display", column.display, FormatContextKind::list_item,
                   true);
        expression("column:" + column.id + ":sort", column.sort, FormatContextKind::sort, false);
        expression("column:" + column.id + ":style", column.style, FormatContextKind::list_item,
                   false);
    }
    for (const auto id : builtin_column_ids) {
        if (!ids.contains(std::string{id})) {
            add({}, "Every built-in column has a place, shown or not");
            break;
        }
    }
    if (!shown) {
        add({}, "Show at least one column");
    }
    expression("view:row_style", view.row_style, FormatContextKind::list_item, false);
    if (view.grouping == ViewGrouping::expression) {
        expression("group:key", view.group_key, FormatContextKind::grouping, true);
        expression("group:title", view.group_title, FormatContextKind::list_item, true);
        expression("group:details", view.group_details, FormatContextKind::list_item, false);
        if (view.subgroup) {
            expression("subgroup:key", view.subgroup->key, FormatContextKind::grouping, true);
            expression("subgroup:label", view.subgroup->label, FormatContextKind::list_item, true);
        }
    }
    return problems;
}

std::string serialize_list_views(const std::vector<ListView>& views) {
    auto saved = Json::array();
    for (const auto& view : views) {
        if (view.builtin) {
            continue;
        }
        auto columns = Json::array();
        for (const auto& column : view.columns) {
            Json entry{{"id", column.id}, {"visible", column.visible}};
            if (!column.second_line.empty()) {
                entry["second_line"] = column.second_line;
            }
            if (column.width > 0) {
                entry["width"] = column.width;
            }
            if (column.builtin()) {
                // ADR-0285: what it was given of its own, and only that.
                for (const auto& [name, value] :
                     {std::pair{"title", &column.title}, std::pair{"display", &column.display},
                      std::pair{"sort", &column.sort}, std::pair{"style", &column.style}}) {
                    if (!value->empty()) {
                        entry[name] = *value;
                    }
                }
                if (column.alignment != ColumnAlignment::left) {
                    entry["align"] = alignment_name(column.alignment);
                }
            } else {
                entry["title"] = column.title;
                entry["display"] = column.display;
                entry["sort"] = column.sort;
                entry["align"] = alignment_name(column.alignment);
                entry["expands"] = column.expands;
                if (!column.style.empty()) {
                    entry["style"] = column.style;
                }
            }
            columns.push_back(std::move(entry));
        }
        Json entry{{"id", view.id},
                   {"name", view.name},
                   {"artwork", artwork_name(view.artwork)},
                   {"grouping", grouping_name(view.grouping)},
                   {"columns", std::move(columns)}};
        if (!view.row_style.empty()) {
            entry["row_style"] = view.row_style;
        }
        if (view.row_lines != 1) {
            entry["row_lines"] = view.row_lines;
        }
        if (view.grouping == ViewGrouping::expression) {
            entry["group"] = Json{{"key", view.group_key},
                                  {"title", view.group_title},
                                  {"details", view.group_details}};
            if (view.subgroup) {
                entry["subgroup"] =
                    Json{{"key", view.subgroup->key}, {"label", view.subgroup->label}};
            }
        }
        saved.push_back(std::move(entry));
    }
    return Json{{"schema", list_view_schema_version}, {"dialect", dialect_name}, {"views", saved}}
        .dump();
}

core::Result<std::vector<ListView>> deserialize_list_views(const std::string_view text) {
    const auto parsed = Json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::unexpected(problem("Saved list views are not JSON"));
    }
    const auto schema = parsed.value("schema", 0);
    if (schema != list_view_schema_version) {
        return std::unexpected(problem(schema > list_view_schema_version
                                           ? "Saved list views are from a newer Trackknife"
                                           : "Saved list views have no known schema",
                                       core::ErrorCode::unsupported));
    }
    if (parsed.value("dialect", std::string{}) != dialect_name) {
        return std::unexpected(
            problem("Saved list views are of another dialect", core::ErrorCode::unsupported));
    }
    const auto found = parsed.find("views");
    if (found == parsed.end() || !found->is_array() || found->size() > maximum_views) {
        return std::unexpected(problem("Saved list views are malformed"));
    }
    const auto text_of = [](const Json& object, const char* key) {
        const auto value = object.find(key);
        return value != object.end() && value->is_string() ? value->get<std::string>()
                                                           : std::string{};
    };
    std::vector<ListView> views;
    for (const auto& entry : *found) {
        if (!entry.is_object() || !entry.contains("columns") || !entry["columns"].is_array()) {
            return std::unexpected(problem("A saved list view is malformed"));
        }
        ListView view;
        view.id = text_of(entry, "id");
        view.name = text_of(entry, "name");
        const auto artwork = text_of(entry, "artwork");
        view.artwork = artwork == "header" ? ViewArtwork::header
                       : artwork == "none" ? ViewArtwork::none
                                           : ViewArtwork::beside;
        const auto grouping = text_of(entry, "grouping");
        view.grouping = grouping == "none"         ? ViewGrouping::none
                        : grouping == "expression" ? ViewGrouping::expression
                                                   : ViewGrouping::album;
        if (const auto group = entry.find("group"); group != entry.end() && group->is_object()) {
            view.group_key = text_of(*group, "key");
            view.group_title = text_of(*group, "title");
            view.group_details = text_of(*group, "details");
        }
        view.row_style = text_of(entry, "row_style");
        view.row_lines = entry.value("row_lines", 1);
        if (const auto subgroup = entry.find("subgroup");
            subgroup != entry.end() && subgroup->is_object()) {
            view.subgroup = ViewSubgroup{.key = text_of(*subgroup, "key"),
                                         .label = text_of(*subgroup, "label")};
        }
        for (const auto& saved : entry["columns"]) {
            if (!saved.is_object()) {
                return std::unexpected(problem("A saved list column is malformed"));
            }
            const auto align = text_of(saved, "align");
            view.columns.push_back(ViewColumn{
                .id = text_of(saved, "id"),
                .visible = saved.value("visible", true),
                .title = text_of(saved, "title"),
                .display = text_of(saved, "display"),
                .sort = text_of(saved, "sort"),
                .alignment = align == "right"    ? ColumnAlignment::right
                             : align == "center" ? ColumnAlignment::center
                                                 : ColumnAlignment::left,
                .expands = saved.value("expands", false),
                .style = text_of(saved, "style"),
                .second_line = text_of(saved, "second_line"),
                .width = std::clamp(saved.value("width", 0), 0, 4'096),
            });
        }
        // A built-in column added since it was saved goes last, hidden.
        for (const auto id : builtin_column_ids) {
            if (!std::ranges::contains(view.columns, id, &ViewColumn::id)) {
                view.columns.push_back({.id = std::string{id},
                                        .visible = false,
                                        .title = {},
                                        .display = {},
                                        .sort = {},
                                        .alignment = ColumnAlignment::left,
                                        .expands = false,
                                        .style = {},
                                        .second_line = {},
                                        .width = 0});
            }
        }
        if (const auto problems = validate_list_view(view); !problems.empty()) {
            return std::unexpected(problem("Saved list view \"" + view.name +
                                           "\" is not valid: " + problems.front().message));
        }
        if (shipped_list_view(view.id) != nullptr ||
            std::ranges::contains(views, view.id, &ListView::id)) {
            return std::unexpected(problem("Two saved list views share an id"));
        }
        views.push_back(std::move(view));
    }
    return views;
}

core::Result<CompiledListView> CompiledListView::compile(const ListView& view) {
    if (const auto problems = validate_list_view(view); !problems.empty()) {
        return std::unexpected(problem(problems.front().message));
    }
    CompiledListView compiled;
    compiled.view_ = view;
    for (const auto& column : view.columns) {
        if (column.second_line.empty()) {
            continue;
        }
        auto line = compiled_program(column.second_line, FormatContextKind::list_item);
        if (!line) {
            return std::unexpected(std::move(line.error()));
        }
        compiled.second_line_ids_.push_back(column.id);
        compiled.second_lines_.push_back(std::move(*line));
    }
    const auto reads = [](const titleformat::Program& program) {
        return std::ranges::any_of(program.fieldDependencies(), [](const std::string& field) {
            return std::ranges::contains(history_field_names,
                                         metadata::canonicalize_field_name(field));
        });
    };
    compiled.reads_history_ = std::ranges::any_of(view.columns, [](const ViewColumn& column) {
        return column.visible && (column.id == "play-count" || column.id == "last-played");
    });
    compiled.reads_history_ =
        compiled.reads_history_ || std::ranges::any_of(compiled.second_lines_, reads);
    for (const auto& column : view.columns) {
        if (!column.builtin() ||
            (column.display.empty() && column.sort.empty() && column.style.empty())) {
            continue;
        }
        // ADR-0285: a built-in column's own expressions.
        const auto optional_program =
            [&reads, &compiled](
                const std::string& source,
                const FormatContextKind kind) -> core::Result<std::optional<titleformat::Program>> {
            if (source.empty()) {
                return std::optional<titleformat::Program>{};
            }
            auto program = compiled_program(source, kind);
            if (!program) {
                return std::unexpected(std::move(program.error()));
            }
            compiled.reads_history_ = compiled.reads_history_ || reads(*program);
            return std::optional{std::move(*program)};
        };
        auto display = optional_program(column.display, FormatContextKind::list_item);
        auto sort = optional_program(column.sort, FormatContextKind::sort);
        auto style = optional_program(column.style, FormatContextKind::list_item);
        if (!display || !sort || !style) {
            return std::unexpected(!display ? display.error()
                                   : !sort  ? sort.error()
                                            : style.error());
        }
        compiled.builtin_ids_.push_back(column.id);
        compiled.builtin_displays_.push_back(std::move(*display));
        compiled.builtin_sorts_.push_back(std::move(*sort));
        compiled.builtin_styles_.push_back(std::move(*style));
    }
    for (const auto& column : view.columns) {
        if (column.builtin()) {
            continue;
        }
        auto display = compiled_program(column.display, FormatContextKind::list_item);
        if (!display) {
            return std::unexpected(std::move(display.error()));
        }
        compiled.reads_history_ = compiled.reads_history_ || reads(*display);
        compiled.expression_ids_.push_back(column.id);
        compiled.displays_.push_back(std::move(*display));
        if (column.style.empty()) {
            compiled.cell_styles_.emplace_back();
        } else {
            auto style = compiled_program(column.style, FormatContextKind::list_item);
            if (!style) {
                return std::unexpected(std::move(style.error()));
            }
            compiled.reads_history_ = compiled.reads_history_ || reads(*style);
            compiled.cell_styles_.emplace_back(std::move(*style));
        }
        if (column.sort.empty()) {
            compiled.sorts_.emplace_back();
        } else {
            auto sort = compiled_program(column.sort, FormatContextKind::sort);
            if (!sort) {
                return std::unexpected(std::move(sort.error()));
            }
            compiled.reads_history_ = compiled.reads_history_ || reads(*sort);
            compiled.sorts_.emplace_back(std::move(*sort));
        }
    }
    if (!view.row_style.empty()) {
        auto style = compiled_program(view.row_style, FormatContextKind::list_item);
        if (!style) {
            return std::unexpected(std::move(style.error()));
        }
        compiled.reads_history_ = compiled.reads_history_ || reads(*style);
        compiled.row_style_ = std::move(*style);
    }
    if (view.grouping == ViewGrouping::expression) {
        const auto program = [](const std::string& source, const FormatContextKind kind)
            -> core::Result<titleformat::Program> { return compiled_program(source, kind); };
        auto key = program(view.group_key, FormatContextKind::grouping);
        auto title = program(view.group_title, FormatContextKind::list_item);
        if (!key || !title) {
            return std::unexpected(!key ? key.error() : title.error());
        }
        compiled.reads_history_ = compiled.reads_history_ || reads(*key) || reads(*title);
        compiled.group_key_ = std::move(*key);
        compiled.group_title_ = std::move(*title);
        if (!view.group_details.empty()) {
            auto details = program(view.group_details, FormatContextKind::list_item);
            if (!details) {
                return std::unexpected(std::move(details.error()));
            }
            compiled.reads_history_ = compiled.reads_history_ || reads(*details);
            compiled.group_details_ = std::move(*details);
        }
        if (view.subgroup) {
            auto sub_key = program(view.subgroup->key, FormatContextKind::grouping);
            auto label = program(view.subgroup->label, FormatContextKind::list_item);
            if (!sub_key || !label) {
                return std::unexpected(!sub_key ? sub_key.error() : label.error());
            }
            compiled.reads_history_ = compiled.reads_history_ || reads(*sub_key) || reads(*label);
            compiled.subgroup_key_ = std::move(*sub_key);
            compiled.subgroup_label_ = std::move(*label);
        }
    }
    return compiled;
}

// Friend of CompiledListView: reads its programs.
struct ListViewEvaluator {
    static core::Result<std::vector<EvaluatedRow>>
    rows(const CompiledListView& compiled, const std::span<const ViewRow> rows,
         const core::CancellationToken& cancellation) {
        std::vector<EvaluatedRow> evaluated;
        evaluated.reserve(rows.size());
        for (const auto& source : rows) {
            if (cancellation.is_cancellation_requested()) {
                return std::unexpected(
                    problem("Evaluating the list was cancelled", core::ErrorCode::cancelled));
            }
            const ViewRowContext context{source, FormatContextKind::list_item};
            EvaluatedRow row;
            row.cells.reserve(compiled.displays_.size());
            for (const auto& display : compiled.displays_) {
                row.cells.push_back(format(display, context, cancellation));
            }
            if (compiled.row_style_) {
                row.row_style = format(*compiled.row_style_, context, cancellation);
            }
            row.second_lines.reserve(compiled.second_lines_.size());
            for (const auto& line : compiled.second_lines_) {
                row.second_lines.push_back(format(line, context, cancellation));
            }
            if (!compiled.builtin_ids_.empty()) {
                row.builtin_cells.reserve(compiled.builtin_ids_.size());
                row.builtin_styles.reserve(compiled.builtin_ids_.size());
                for (std::size_t at = 0U; at < compiled.builtin_ids_.size(); ++at) {
                    const auto& display = compiled.builtin_displays_[at];
                    const auto& style = compiled.builtin_styles_[at];
                    row.builtin_cells.push_back(display ? format(*display, context, cancellation)
                                                        : std::string{});
                    row.builtin_styles.push_back(style ? format(*style, context, cancellation)
                                                       : std::string{});
                }
            }
            if (std::ranges::any_of(compiled.cell_styles_,
                                    [](const auto& style) { return style.has_value(); })) {
                row.cell_styles.reserve(compiled.cell_styles_.size());
                for (const auto& style : compiled.cell_styles_) {
                    row.cell_styles.push_back(style ? format(*style, context, cancellation)
                                                    : std::string{});
                }
            }
            if (compiled.group_key_) {
                const ViewRowContext grouping{source, FormatContextKind::grouping};
                row.group_key = format(*compiled.group_key_, grouping, cancellation);
                if (compiled.subgroup_key_) {
                    row.subgroup_key = format(*compiled.subgroup_key_, grouping, cancellation);
                    row.subgroup_label = format(*compiled.subgroup_label_, context, cancellation);
                }
            }
            evaluated.push_back(std::move(row));
        }
        return evaluated;
    }

    static GroupHeader header(const CompiledListView& compiled, const ViewRow& first,
                              const std::size_t tracks, const std::int64_t length_ms,
                              const core::CancellationToken& cancellation) {
        if (!compiled.group_title_) {
            return {};
        }
        const std::map<std::string, std::string> host{{"grouptracks", std::to_string(tracks)},
                                                      {"grouplength", clock(length_ms)}};
        const ViewRowContext context{first, FormatContextKind::list_item, &host};
        return {.title = format(*compiled.group_title_, context, cancellation),
                .details = compiled.group_details_
                               ? format(*compiled.group_details_, context, cancellation)
                               : std::string{}};
    }

    static core::Result<std::vector<std::string>>
    sort_keys(const CompiledListView& compiled, const std::string_view column_id,
              const std::span<const ViewRow> rows, const core::CancellationToken& cancellation) {
        const titleformat::Program* program = nullptr;
        std::optional<titleformat::Program> builtin;
        const auto own = std::ranges::find(compiled.builtin_ids_, column_id);
        const auto own_at = static_cast<std::size_t>(own - compiled.builtin_ids_.begin());
        if (const auto found = std::ranges::find(compiled.expression_ids_, column_id);
            found != compiled.expression_ids_.end()) {
            const auto at = static_cast<std::size_t>(found - compiled.expression_ids_.begin());
            program = compiled.sorts_[at] ? &*compiled.sorts_[at] : &compiled.displays_[at];
        } else if (own != compiled.builtin_ids_.end() && compiled.builtin_sorts_[own_at]) {
            // ADR-0285: a built-in column by its own sort; whatever it
            // shows, it sorts as it always did otherwise.
            program = &*compiled.builtin_sorts_[own_at];
        } else {
            // A built-in column sorts by what it shows; the number by disc
            // first, the history by the engine's (ADR-0282).
            const std::map<std::string_view, std::string_view> sources{
                {"artist", "%artist%"},
                {"track-number", "$if(%discnumber%,%discnumber%|)%tracknumber%"},
                {"title", "%title%"},
                {"album", "%album%"},
                {"date", "%date%"},
                {"length", "$info(lengthms)"},
                {"rating", "%rating%"},
                {"play-count", "%playcount%"},
                {"last-played", "%lastplayed%"},
                {"artwork", "%album%"}};
            const auto source = sources.find(column_id);
            if (source == sources.end()) {
                return std::unexpected(
                    problem("This column cannot sort the list", core::ErrorCode::unsupported));
            }
            auto compiled_builtin =
                compiled_program(std::string{source->second}, FormatContextKind::sort);
            if (!compiled_builtin) {
                return std::unexpected(std::move(compiled_builtin.error()));
            }
            builtin = std::move(*compiled_builtin);
            program = &*builtin;
        }
        std::vector<std::string> keys;
        keys.reserve(rows.size());
        for (std::size_t at = 0U; at < rows.size(); ++at) {
            if (cancellation.is_cancellation_requested()) {
                return std::unexpected(
                    problem("Sorting the list was cancelled", core::ErrorCode::cancelled));
            }
            // In the context its program was compiled for: a display shown sorts too.
            const ViewRowContext context{rows[at], program->context()};
            auto lowered = core::unicodeSimpleLower(format(*program, context, cancellation));
            keys.push_back(lowered ? std::move(*lowered) : std::string{});
        }
        return keys;
    }
};

core::Result<std::vector<EvaluatedRow>>
evaluate_list_rows(const CompiledListView& compiled, const std::span<const ViewRow> rows,
                   const core::CancellationToken& cancellation) {
    return ListViewEvaluator::rows(compiled, rows, cancellation);
}

std::vector<GroupRun> list_group_runs(const std::span<const EvaluatedRow* const> rows,
                                      const std::span<const std::int64_t> durations_ms) {
    std::vector<GroupRun> runs;
    for (std::size_t first = 0U; first < rows.size();) {
        auto end = first + 1U;
        while (end < rows.size() && rows[end]->group_key == rows[first]->group_key) {
            ++end;
        }
        GroupRun run{.first = first, .count = end - first, .length_ms = 0, .subgroup_starts = {}};
        std::set<std::string_view> keys;
        for (auto row = first; row < end; ++row) {
            run.length_ms += row < durations_ms.size() ? durations_ms[row] : 0;
            if (!rows[row]->subgroup_key.empty()) {
                keys.insert(rows[row]->subgroup_key);
            }
        }
        // Only a group with more than one subgroup says which is which, as
        // an album says its discs only when it has several.
        if (keys.size() > 1U) {
            for (auto row = first; row < end; ++row) {
                const auto& key = rows[row]->subgroup_key;
                if (!key.empty() && (row == first || key != rows[row - 1U]->subgroup_key)) {
                    run.subgroup_starts.push_back(row);
                }
            }
        }
        runs.push_back(std::move(run));
        first = end;
    }
    return runs;
}

GroupHeader evaluate_group_header(const CompiledListView& compiled, const ViewRow& first,
                                  const std::size_t tracks, const std::int64_t length_ms,
                                  const core::CancellationToken& cancellation) {
    return ListViewEvaluator::header(compiled, first, tracks, length_ms, cancellation);
}

core::Result<std::vector<EvaluatedGroup>>
evaluate_list_groups(const CompiledListView& compiled, const std::span<const ViewRow> rows,
                     const std::span<const EvaluatedRow> evaluated,
                     const core::CancellationToken& cancellation) {
    std::vector<EvaluatedGroup> groups;
    if (!compiled.groups_by_expression() || rows.size() != evaluated.size()) {
        return groups;
    }
    std::vector<const EvaluatedRow*> pointers;
    std::vector<std::int64_t> durations;
    pointers.reserve(rows.size());
    durations.reserve(rows.size());
    for (std::size_t at = 0U; at < rows.size(); ++at) {
        pointers.push_back(&evaluated[at]);
        durations.push_back(rows[at].duration_ms.value_or(0));
    }
    for (auto& run : list_group_runs(pointers, durations)) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(
                problem("Grouping the list was cancelled", core::ErrorCode::cancelled));
        }
        auto header = evaluate_group_header(compiled, rows[run.first], run.count, run.length_ms,
                                            cancellation);
        EvaluatedGroup group{.first = run.first,
                             .count = run.count,
                             .title = std::move(header.title),
                             .details = std::move(header.details),
                             .subgroups = {}};
        for (const auto start : run.subgroup_starts) {
            group.subgroups.emplace_back(start, evaluated[start].subgroup_label);
        }
        groups.push_back(std::move(group));
    }
    return groups;
}

core::Result<std::string> evaluate_view_expression(const std::string_view source,
                                                   const FormatContextKind context,
                                                   const ViewRow& row, const std::size_t tracks,
                                                   const std::int64_t length_ms) {
    auto program = compiled_program(std::string{source}, context);
    if (!program) {
        return std::unexpected(std::move(program.error()));
    }
    const std::map<std::string, std::string> host{{"grouptracks", std::to_string(tracks)},
                                                  {"grouplength", clock(length_ms)}};
    const ViewRowContext evaluation{row, context, &host};
    return format(*program, evaluation, {});
}

CellStyle parse_cell_style(const std::string_view words) {
    CellStyle style;
    std::size_t at = 0U;
    while (at < words.size()) {
        const auto end = words.find_first_of(" ,", at);
        const auto word = words.substr(at, end == std::string_view::npos ? end : end - at);
        style.quiet = style.quiet || word == "quiet";
        style.accent = style.accent || word == "accent";
        style.warning = style.warning || word == "warning";
        style.bold = style.bold || word == "bold";
        style.italic = style.italic || word == "italic";
        if (end == std::string_view::npos) {
            break;
        }
        at = end + 1U;
    }
    return style;
}

CellStyle combined_style(const CellStyle& row, const CellStyle& cell) {
    // One colour: the cell's, where it gives one.
    const bool coloured = cell.quiet || cell.accent || cell.warning;
    return {.quiet = coloured ? cell.quiet : row.quiet,
            .accent = coloured ? cell.accent : row.accent,
            .warning = coloured ? cell.warning : row.warning,
            .bold = row.bold || cell.bold,
            .italic = row.italic || cell.italic};
}

core::Result<std::vector<std::string>> list_sort_keys(const CompiledListView& compiled,
                                                      const std::string_view column_id,
                                                      const std::span<const ViewRow> rows,
                                                      const core::CancellationToken& cancellation) {
    return ListViewEvaluator::sort_keys(compiled, column_id, rows, cancellation);
}

std::vector<int> list_sort_order(const std::span<const std::string> keys, const bool descending) {
    std::vector<int> order(keys.size());
    std::iota(order.begin(), order.end(), 0);
    std::ranges::stable_sort(order, [&keys, descending](const int left, const int right) {
        const auto& a = keys[static_cast<std::size_t>(left)];
        const auto& b = keys[static_cast<std::size_t>(right)];
        // Empty last, whichever way.
        if (a.empty() != b.empty()) {
            return b.empty();
        }
        return descending ? natural_less(b, a) : natural_less(a, b);
    });
    return order;
}

} // namespace trackknife::lists
