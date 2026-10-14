// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/lists/list_view.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace lists = trackknife::lists;
namespace metadata = trackknife::metadata;

int failures = 0;

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

[[nodiscard]] metadata::MetadataDocument
tags(std::initializer_list<std::pair<std::string, std::vector<std::string>>> fields) {
    metadata::MetadataDocument document;
    for (const auto& [name, values] : fields) {
        document.fields.push_back({.canonical_name = metadata::canonicalize_field_name(name),
                                   .native_name = name,
                                   .values = values,
                                   .qualifier = {},
                                   .provenance = metadata::FieldProvenance::embedded});
    }
    return document;
}

[[nodiscard]] lists::ViewColumn expression_column(std::string id, std::string title,
                                                  std::string display, std::string sort = {}) {
    return {.id = std::move(id),
            .visible = true,
            .title = std::move(title),
            .display = std::move(display),
            .sort = std::move(sort),
            .alignment = lists::ColumnAlignment::left,
            .expands = false,
            .style = {}};
}

// A copy of a shipped view with expression columns and a grouping of its own.
[[nodiscard]] lists::ListView user_view() {
    auto view = *lists::shipped_list_view(lists::columns_view_id);
    view.id = "user-1";
    view.name = "By label";
    view.builtin = false;
    view.columns.push_back(expression_column("expression-1", "Label", "%label%"));
    view.columns.push_back(
        expression_column("expression-2", "Where", "$info(filename_ext) · %length%"));
    view.columns.push_back(
        expression_column("expression-3", "Kind", "$info(codec) $info(samplerate)"));
    view.grouping = lists::ViewGrouping::expression;
    view.group_key = "%label%";
    view.group_title = "%label%";
    view.group_details = "%group_tracks% tracks · %group_length%";
    view.subgroup = lists::ViewSubgroup{.key = "%discnumber%", .label = "Disc %discnumber%"};
    // ADR-0283: rows of one label quiet, and the label itself bold.
    view.row_style = "$if($eq(%label%,Warp),quiet,)";
    view.columns[view.columns.size() - 3].style = "bold";
    return view;
}

void shippedViewsAreValidAndAsTheyWere() {
    const auto& shipped = lists::shipped_list_views();
    // Two: albums with side artwork, and plain columns.
    CHECK(shipped.size() == 2U);
    for (const auto& view : shipped) {
        CHECK(view.builtin);
        CHECK(lists::validate_list_view(view).empty());
        CHECK(lists::CompiledListView::compile(view).has_value());
    }
    // Beside the rows, the header says artist, album and date.
    const auto* beside = lists::shipped_list_view(lists::albums_beside_view_id);
    CHECK(beside != nullptr);
    const auto shown = [](const lists::ListView& view, std::string_view id) {
        const auto found = std::ranges::find(view.columns, id, &lists::ViewColumn::id);
        return found != view.columns.end() && found->visible;
    };
    CHECK(beside && shown(*beside, "title") && !shown(*beside, "artist") &&
          !shown(*beside, "album") && shown(*beside, "artwork"));
    CHECK(shown(*lists::shipped_list_view(lists::columns_view_id), "date"));
    // Views once shipped are drawn by the one in their place.
    CHECK(lists::retired_list_view_replacement("albums-header-artwork") ==
          lists::albums_beside_view_id);
    CHECK(lists::retired_list_view_replacement("compact-queue") == lists::columns_view_id);
    CHECK(lists::retired_list_view_replacement("recently-added") == lists::columns_view_id);
    CHECK(!lists::retired_list_view_replacement("plain-columns"));
    CHECK(!lists::retired_list_view_replacement("view-123"));
    // Shipped views are not saved: they are always there as they are.
    const auto saved = lists::serialize_list_views(shipped);
    const auto restored = lists::deserialize_list_views(saved);
    CHECK(restored && restored->empty());
}

void viewsRoundTripAndRefuseWhatTheyCannotRead() {
    const auto view = user_view();
    CHECK(lists::validate_list_view(view).empty());
    const auto saved = lists::serialize_list_views({view});
    const auto restored = lists::deserialize_list_views(saved);
    CHECK(restored && restored->size() == 1U && restored->front() == view);

    CHECK(!lists::deserialize_list_views(R"({"schema":2,"dialect":"tkfmt-1","views":[]})"));
    CHECK(!lists::deserialize_list_views(R"({"schema":1,"dialect":"tkfmt-2","views":[]})"));
    CHECK(!lists::deserialize_list_views("not json"));
    // A built-in column that came later joins a saved view, hidden.
    const auto without_rating = std::string{saved}.replace(
        saved.find(R"({"id":"rating","visible":false},)"),
        std::string_view{R"({"id":"rating","visible":false},)"}.size(), "");
    const auto completed = lists::deserialize_list_views(without_rating);
    CHECK(completed && completed->size() == 1U &&
          std::ranges::contains(completed->front().columns, std::string{"rating"},
                                &lists::ViewColumn::id));
}

void problemsSayWhereTheyAre() {
    auto view = user_view();
    view.columns.back().display = "$if(%genre%";
    view.group_key.clear();
    const auto problems = lists::validate_list_view(view);
    const auto at = [&problems](std::string_view where) {
        return std::ranges::contains(problems, where, &lists::ViewProblem::where);
    };
    CHECK(at("column:expression-3:display"));
    CHECK(at("group:key"));
    CHECK(!lists::CompiledListView::compile(view));

    auto duplicate = user_view();
    duplicate.columns.push_back(duplicate.columns.back());
    CHECK(!lists::validate_list_view(duplicate).empty());
    auto nothing_shown = user_view();
    for (auto& column : nothing_shown.columns) {
        column.visible = false;
    }
    CHECK(!lists::validate_list_view(nothing_shown).empty());
}

void rowsAreEvaluatedAndGrouped() {
    const auto compiled = lists::CompiledListView::compile(user_view());
    CHECK(compiled.has_value());
    if (!compiled) {
        return;
    }
    const auto first = tags({{"LABEL", {"Interscope"}}, {"DISCNUMBER", {"1"}}});
    const auto second = tags({{"LABEL", {"Interscope"}}, {"DISCNUMBER", {"2"}}});
    const auto third = tags({{"LABEL", {"Interscope"}}, {"DISCNUMBER", {"2"}}});
    const auto other = tags({{"LABEL", {"Warp"}}, {"DISCNUMBER", {"1"}}});
    const auto make = [](const metadata::MetadataDocument& document, std::string_view path,
                         const std::int64_t duration) {
        return lists::ViewRow{.raw_path = path,
                              .metadata = &document,
                              .title = {},
                              .artist = {},
                              .album = {},
                              .album_artist = {},
                              .date = {},
                              .track_number = {},
                              .duration_ms = duration,
                              .codec = "flac",
                              .sample_rate = 44'100,
                              .bits = 16,
                              .channels = 2,
                              .bit_rate = 0,
                              .rating = 0U,
                              .history = std::nullopt,
                              .added = 0,
                              .album_added = 0};
    };
    const std::vector rows{
        make(first, "/music/a/One.flac", 61'000), make(second, "/music/a/Two.flac", 59'000),
        make(third, "/music/a/Three.flac", 60'000), make(other, "/music/b/Four.flac", 1'000)};
    const auto evaluated = lists::evaluate_list_rows(*compiled, rows);
    CHECK(evaluated && evaluated->size() == 4U);
    if (!evaluated) {
        return;
    }
    CHECK((evaluated->front().cells ==
           std::vector<std::string>{"Interscope", "One.flac · 1:01", "flac 44100"}));
    CHECK(evaluated->back().cells[1] == "Four.flac · 0:01");
    const auto groups = lists::evaluate_list_groups(*compiled, rows, *evaluated);
    CHECK(groups && groups->size() == 2U);
    if (!groups || groups->size() != 2U) {
        return;
    }
    const auto& label = groups->front();
    CHECK(label.first == 0U && label.count == 3U && label.title == "Interscope");
    CHECK(label.details == "3 tracks · 3:00");
    // Discs 1 and 2: each said where it starts.
    CHECK((label.subgroups ==
           std::vector<std::pair<std::size_t, std::string>>{{0U, "Disc 1"}, {1U, "Disc 2"}}));
    // One disc only: nothing to tell apart.
    CHECK(groups->back().first == 3U && groups->back().subgroups.empty());
}

// ADR-0282: listening history read by expressions, under tkq-1's names; a
// view that reads it -- or shows a history column -- says so, so its list's
// history is fetched first.
// What a row says of itself: its technicals, its file, and (ADR-0283) when
// it came into the library.
void rowsSayWhatTheyAre() {
    const auto document = tags({{"ALBUM", {"Mezzanine"}}});
    lists::ViewRow row;
    row.raw_path = "/music/Mezzanine/01 Angel.flac";
    row.metadata = &document;
    row.sample_rate = 44'100;
    row.bits = 16;
    row.bit_rate = 1'009'000;
    // Came into the library 2023-11-14, its album's newest a day later.
    row.added = 1'700'000'000;
    row.album_added = 1'700'086'400;
    auto view = *lists::shipped_list_view(lists::columns_view_id);
    view.id = "facts";
    view.builtin = false;
    for (const auto& [id, display] :
         {std::pair{"rate", "$decimal($info(samplerate),1000,1) kHz"},
          std::pair{"bits", "$info(bitspersample)-bit"}, std::pair{"bitrate", "$info(bitrate)"},
          std::pair{"folder", "$info(directory)"}, std::pair{"added", "$left(%added%,10)"},
          std::pair{"album-added", "$left(%albumadded%,10)"},
          std::pair{"since", "%dayssinceadded%"}}) {
        view.columns.push_back(expression_column(id, id, display));
    }
    const auto compiled = lists::CompiledListView::compile(view);
    CHECK(compiled.has_value());
    if (!compiled) {
        return;
    }
    const std::vector rows{row};
    const auto evaluated = lists::evaluate_list_rows(*compiled, rows);
    CHECK(evaluated && evaluated->size() == 1U);
    if (!evaluated) {
        return;
    }
    const auto& cells = evaluated->front().cells;
    CHECK(cells[0] == "44.1 kHz" && cells[1] == "16-bit" && cells[2] == "1009");
    CHECK(cells[3] == "/music/Mezzanine");
    CHECK(cells[4].starts_with("2023-11-1") && cells[4].size() == 10U);
    CHECK(cells[5].starts_with("2023-11-1") && cells[5] >= cells[4]);
    CHECK(!cells[6].empty() && std::stoll(cells[6]) > 300);
}

// ADR-0283: style words as expressions answer them, a cell's over its row's.
void stylesAreWordsExpressionsAnswer() {
    const auto quiet_bold = lists::parse_cell_style("quiet, bold  sparkly");
    CHECK(quiet_bold.quiet && quiet_bold.bold && !quiet_bold.italic && !quiet_bold.warning);
    CHECK(!lists::parse_cell_style("").any());
    const auto warned = lists::combined_style(quiet_bold, lists::parse_cell_style("warning"));
    // One colour, the cell's; and the row's weight still.
    CHECK(warned.warning && !warned.quiet && warned.bold);
    const auto kept = lists::combined_style(quiet_bold, lists::parse_cell_style("italic"));
    CHECK(kept.quiet && kept.bold && kept.italic);

    const auto compiled = lists::CompiledListView::compile(user_view());
    const auto warp = tags({{"LABEL", {"Warp"}}});
    const auto other = tags({{"LABEL", {"Interscope"}}});
    std::vector<lists::ViewRow> rows(2);
    rows[0].metadata = &warp;
    rows[1].metadata = &other;
    const auto evaluated = lists::evaluate_list_rows(*compiled, rows);
    CHECK(evaluated && evaluated->size() == 2U);
    if (!evaluated) {
        return;
    }
    CHECK((*evaluated)[0].row_style == "quiet" && (*evaluated)[1].row_style.empty());
    CHECK(((*evaluated)[0].cell_styles == std::vector<std::string>{"bold", "", ""}));
    // A bad style expression is said where it is.
    auto bad = user_view();
    bad.row_style = "$if(";
    CHECK(std::ranges::contains(lists::validate_list_view(bad), std::string{"view:row_style"},
                                &lists::ViewProblem::where));
}

void historyIsReadByExpressions() {
    auto view = user_view();
    CHECK(lists::CompiledListView::compile(view).has_value() &&
          !lists::CompiledListView::compile(view)->reads_history());
    view.columns.push_back(expression_column(
        "expression-9", "Plays", "$if(%play_count%,%playcount%×,never) · %days_since_played%"));
    auto compiled = lists::CompiledListView::compile(view);
    CHECK(compiled && compiled->reads_history());
    if (!compiled) {
        return;
    }
    const auto document = tags({});
    lists::ViewRow played;
    played.metadata = &document;
    played.history =
        std::array<std::int64_t, 6>{7, 1'700'000'000'000, 12, 30, 1'700'000'000'000, 2};
    lists::ViewRow unplayed;
    unplayed.metadata = &document;
    const std::vector rows{played, unplayed};
    const auto evaluated = lists::evaluate_list_rows(*compiled, rows);
    CHECK(evaluated && evaluated->size() == 2U);
    if (!evaluated) {
        return;
    }
    CHECK(evaluated->front().cells.back() == "7× · 12");
    CHECK(evaluated->back().cells.back() == "never · ");
    // Last played as a date and time, which sorts as it reads.
    auto dated = user_view();
    dated.columns.push_back(expression_column("expression-9", "Last", "%lastplayed%"));
    const auto with_date = lists::CompiledListView::compile(dated);
    const auto shown = lists::evaluate_list_rows(*with_date, rows);
    CHECK(shown && shown->front().cells.back().size() == 16U &&
          shown->front().cells.back().starts_with("2023-11-1"));
    // A history column shown is history read.
    auto column = *lists::shipped_list_view(lists::columns_view_id);
    for (auto& placed : column.columns) {
        placed.visible = placed.visible || placed.id == "play-count";
    }
    CHECK(lists::CompiledListView::compile(column)->reads_history());
}

void columnsSortNaturallyAndStably() {
    const auto compiled = lists::CompiledListView::compile(user_view());
    CHECK(compiled.has_value());
    if (!compiled) {
        return;
    }
    const auto ten = tags({{"TITLE", {"Track 10"}}, {"TRACKNUMBER", {"10"}}});
    const auto two = tags({{"TITLE", {"track 2"}}, {"TRACKNUMBER", {"2"}}});
    const auto none = tags({});
    const auto also_two = tags({{"TITLE", {"Track 2"}}, {"TRACKNUMBER", {"2"}}});
    std::vector<lists::ViewRow> rows;
    for (const auto* document : {&ten, &two, &none, &also_two}) {
        lists::ViewRow row;
        row.metadata = document;
        rows.push_back(row);
    }
    const auto keys = lists::list_sort_keys(*compiled, "title", rows);
    CHECK(keys.has_value());
    if (!keys) {
        return;
    }
    // Digit runs as numbers, case ignored, equal keys in their order, empty last.
    CHECK((lists::list_sort_order(*keys, false) == std::vector<int>{1, 3, 0, 2}));
    CHECK((lists::list_sort_order(*keys, true) == std::vector<int>{0, 1, 3, 2}));
    const auto numbers = lists::list_sort_keys(*compiled, "track-number", rows);
    CHECK(numbers && (lists::list_sort_order(*numbers, false) == std::vector<int>{1, 3, 0, 2}));
    // Play counts as the engine keeps them: most played last ascending, the
    // unknown after every one.
    rows[0].history = std::array<std::int64_t, 6>{12, 1'700'000'000'000, 3, 40, 1, 1};
    rows[1].history = std::array<std::int64_t, 6>{2, 1'600'000'000'000, 900, 2, 1, 1};
    rows[3].history = std::array<std::int64_t, 6>{-1, -1, -1, -1, -1, -1};
    const auto counts = lists::list_sort_keys(*compiled, "play-count", rows);
    CHECK(counts && (lists::list_sort_order(*counts, false) == std::vector<int>{1, 0, 2, 3}));
    const auto played = lists::list_sort_keys(*compiled, "last-played", rows);
    CHECK(played && (lists::list_sort_order(*played, true) == std::vector<int>{0, 1, 2, 3}));
    // An expression column with no sort expression sorts by what it shows.
    const auto labelled = tags({{"LABEL", {"Warp"}}});
    const auto other_label = tags({{"LABEL", {"Interscope"}}});
    std::vector<lists::ViewRow> labelled_rows;
    for (const auto* document : {&labelled, &other_label}) {
        lists::ViewRow row;
        row.metadata = document;
        labelled_rows.push_back(row);
    }
    const auto labels = lists::list_sort_keys(*compiled, "expression-1", labelled_rows);
    CHECK(labels && (*labels == std::vector<std::string>{"warp", "interscope"}));
}

} // namespace

int main() {
    shippedViewsAreValidAndAsTheyWere();
    viewsRoundTripAndRefuseWhatTheyCannotRead();
    problemsSayWhereTheyAre();
    rowsAreEvaluatedAndGrouped();
    columnsSortNaturallyAndStably();
    historyIsReadByExpressions();
    stylesAreWordsExpressionsAnswer();
    rowsSayWhatTheyAre();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
