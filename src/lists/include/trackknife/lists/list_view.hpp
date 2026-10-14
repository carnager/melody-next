// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/titleformat/compiler.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::lists {

// ADR-0282: how a list looks -- its columns, built in or expressions, and how
// its rows are grouped. Qt-free: the window draws it, this decides it.

inline constexpr int list_view_schema_version = 1;

enum class ViewArtwork { beside, header, none };
enum class ColumnAlignment { left, right, center };
enum class ViewGrouping { none, album, expression };

// The built-in columns, by their saved ids: the album view's own, drawn as
// they always were.
inline constexpr std::array<std::string_view, 10> builtin_column_ids{
    "artwork", "artist", "track-number", "title",      "album",
    "date",    "length", "rating",       "play-count", "last-played"};

[[nodiscard]] bool is_builtin_column(std::string_view id);

// ADR-0285: what a built-in column shows, as an expression -- what its
// "Shows" says until it is changed. Empty where no expression draws it as
// Trackknife does (the cover, the rating's stars to click).
[[nodiscard]] std::string_view builtin_column_expression(std::string_view id);

struct ViewColumn {
    // A built-in id, or an expression column's own ("expression-1", ...).
    std::string id;
    bool visible{true};
    // Its header and what it shows. ADR-0285: a built-in column's too --
    // empty, its usual name and as Trackknife draws it (its playing mark,
    // its stars to click, its own way of sorting stay whatever it shows).
    std::string title;
    std::string display;
    // Empty: sorted by what it shows (a built-in, as it always sorted).
    std::string sort;
    // A built-in column's, where it shows an expression of its own.
    ColumnAlignment alignment{ColumnAlignment::left};
    // Expression columns only.
    bool expands{false};
    // ADR-0283: how its cell is drawn, as a style expression says (below);
    // empty, as its row is.
    std::string style;
    // ADR-0284: what its cell says beneath, smaller, in a view of rows of
    // more than one line -- any column, built-in or not; empty, nothing.
    std::string second_line;
    // How wide it starts, in pixels, until a list is given a width of its
    // own; 0, as wide as such a column usually is.
    int width{0};

    [[nodiscard]] bool builtin() const { return is_builtin_column(id); }
    friend bool operator==(const ViewColumn&, const ViewColumn&) = default;
};

struct ViewSubgroup {
    std::string key;
    std::string label;
    friend bool operator==(const ViewSubgroup&, const ViewSubgroup&) = default;
};

struct ListView {
    std::string id;
    std::string name;
    ViewArtwork artwork{ViewArtwork::beside};
    ViewGrouping grouping{ViewGrouping::album};
    // An expression grouping's: what groups rows, and what its header says.
    std::string group_key;
    std::string group_title;
    std::string group_details;
    std::optional<ViewSubgroup> subgroup;
    // ADR-0283: how each row is drawn -- an expression answering style words
    // (`quiet`, `accent`, `warning`, `bold`, `italic`); empty, as always.
    std::string row_style;
    // In their order; every built-in column is here, shown or not.
    std::vector<ViewColumn> columns;
    bool builtin{false};
    // ADR-0284: how many lines each row has, 1-3: its cells' first line, and
    // their second lines beneath.
    int row_lines{1};

    friend bool operator==(const ListView&, const ListView&) = default;
};

// The shipped views: albums with side artwork, and plain columns.
inline constexpr std::string_view albums_beside_view_id = "albums-side-artwork";
inline constexpr std::string_view columns_view_id = "plain-columns";
// ADR-0284: tracks in two lines -- the title with its artist and album
// beneath, the year with its genre, the length with its format, the rating
// with how often it played.
inline constexpr std::string_view two_line_view_id = "two-line-tracks";
[[nodiscard]] const std::vector<ListView>& shipped_list_views();
[[nodiscard]] const ListView* shipped_list_view(std::string_view id);
// A view once shipped and since retired -- header artwork, the compact
// queue, the starting points -- and the shipped one a list using it is drawn
// by now.
[[nodiscard]] std::optional<std::string_view> retired_list_view_replacement(std::string_view id);

// One problem with a view, and which of its expressions it is in:
// "column:<id>:display", "column:<id>:sort", "group:key", "group:title",
// "group:details", "subgroup:key", "subgroup:label", or "" for the view.
struct ViewProblem {
    std::string where;
    std::string message;
    friend bool operator==(const ViewProblem&, const ViewProblem&) = default;
};

// Every problem, empty when the view can be used: a name, unique column ids,
// every built-in column once, at least one shown, valid expressions.
[[nodiscard]] std::vector<ViewProblem> validate_list_view(const ListView& view);

// The error of one expression as an editor shows it; empty when it compiles.
[[nodiscard]] std::string list_view_expression_error(std::string_view source,
                                                     titleformat::FormatContextKind context);

// Saved as JSON with its schema and dialect; views of a newer schema or
// another dialect are refused, not misread.
[[nodiscard]] std::string serialize_list_views(const std::vector<ListView>& views);
[[nodiscard]] core::Result<std::vector<ListView>> deserialize_list_views(std::string_view text);

// What one row of a list offers its expressions.
struct ViewRow {
    std::string_view raw_path;
    const metadata::MetadataDocument* metadata{nullptr};
    // Display fallbacks when the tag is absent.
    std::string_view title;
    std::string_view artist;
    std::string_view album;
    std::string_view album_artist;
    std::string_view date;
    std::string_view track_number;
    std::optional<std::int64_t> duration_ms;
    std::string_view codec;
    int sample_rate{0};
    int bits{0};
    int channels{0};
    std::int64_t bit_rate{0};
    // 1-10; 0 unrated.
    unsigned rating{0U};
    // The engine's listening history (ADR-0282): play count, last played
    // (UTC milliseconds), days since played, and the same for the album, -1
    // where unknown -- or none at all when it was not asked for.
    std::optional<std::array<std::int64_t, 6>> history;
    // ADR-0283: when the track, and its album's newest track, came into the
    // library -- Unix seconds, 0 not known.
    std::int64_t added{0};
    std::int64_t album_added{0};
};

// The history fields, in the order ViewRow::history holds them -- the names
// tkq-1's HISTORY(...) reads them by.
inline constexpr std::array<std::string_view, 6> history_field_names{
    "playcount",      "lastplayed",      "dayssinceplayed",
    "albumplaycount", "albumlastplayed", "albumdayssinceplayed"};

// A row's own copy of all that, for a worker: the list it came from may
// change while it is evaluated.
struct OwnedViewRow {
    std::string raw_path;
    metadata::MetadataDocument metadata;
    std::string title;
    std::string artist;
    std::string album;
    std::string album_artist;
    std::string date;
    std::string track_number;
    std::optional<std::int64_t> duration_ms;
    std::string codec;
    int sample_rate{0};
    int bits{0};
    int channels{0};
    std::int64_t bit_rate{0};
    unsigned rating{0U};
    std::optional<std::array<std::int64_t, 6>> history;
    std::int64_t added{0};
    std::int64_t album_added{0};

    [[nodiscard]] ViewRow view() const {
        return {.raw_path = raw_path,
                .metadata = &metadata,
                .title = title,
                .artist = artist,
                .album = album,
                .album_artist = album_artist,
                .date = date,
                .track_number = track_number,
                .duration_ms = duration_ms,
                .codec = codec,
                .sample_rate = sample_rate,
                .bits = bits,
                .channels = channels,
                .bit_rate = bit_rate,
                .rating = rating,
                .history = history,
                .added = added,
                .album_added = album_added};
    }
};

// A view's expressions, compiled once.
class CompiledListView final {
  public:
    // Fails with the view's first problem.
    [[nodiscard]] static core::Result<CompiledListView> compile(const ListView& view);

    [[nodiscard]] const ListView& view() const noexcept { return view_; }
    // The shown and hidden expression columns, in the view's order.
    [[nodiscard]] const std::vector<std::string>& expression_columns() const noexcept {
        return expression_ids_;
    }
    [[nodiscard]] bool groups_by_expression() const noexcept {
        return view_.grouping == ViewGrouping::expression;
    }
    // Something it shows or sorts by is listening history -- a history
    // field in an expression, or a history column shown -- so the list's
    // history is fetched before its rows are evaluated.
    [[nodiscard]] bool reads_history() const noexcept { return reads_history_; }
    // ADR-0284: the columns with a second line, built-in or not, in the
    // view's order -- EvaluatedRow::second_lines holds theirs so.
    [[nodiscard]] const std::vector<std::string>& second_line_columns() const noexcept {
        return second_line_ids_;
    }
    // ADR-0285: the built-in columns with an expression of their own -- to
    // show, or only a style -- in the view's order; EvaluatedRow::
    // builtin_cells and builtin_styles hold theirs so.
    [[nodiscard]] const std::vector<std::string>& builtin_expression_columns() const noexcept {
        return builtin_ids_;
    }
    // Whether the built-in column shows an expression of its own.
    [[nodiscard]] bool builtin_shows_expression(std::string_view id) const;

  private:
    friend struct ListViewEvaluator;
    ListView view_;
    std::vector<std::string> expression_ids_;
    std::vector<titleformat::Program> displays_;
    std::vector<std::optional<titleformat::Program>> sorts_;
    std::optional<titleformat::Program> group_key_;
    std::optional<titleformat::Program> group_title_;
    std::optional<titleformat::Program> group_details_;
    std::optional<titleformat::Program> subgroup_key_;
    std::optional<titleformat::Program> subgroup_label_;
    std::optional<titleformat::Program> row_style_;
    std::vector<std::optional<titleformat::Program>> cell_styles_;
    std::vector<std::string> second_line_ids_;
    std::vector<titleformat::Program> second_lines_;
    std::vector<std::string> builtin_ids_;
    std::vector<std::optional<titleformat::Program>> builtin_displays_;
    std::vector<std::optional<titleformat::Program>> builtin_sorts_;
    std::vector<std::optional<titleformat::Program>> builtin_styles_;
    bool reads_history_{false};
};

// What a view makes of one row.
struct EvaluatedRow {
    // One per expression column, in the view's order.
    std::vector<std::string> cells;
    // ADR-0283: the row's style words, and each expression column's own
    // (empty where it has no style expression).
    std::string row_style;
    std::vector<std::string> cell_styles;
    // ADR-0284: each column's second line, by second_line_columns() order.
    std::vector<std::string> second_lines;
    // ADR-0285: the built-in columns' own, by builtin_expression_columns()
    // order -- empty where one has only a style, or only a display.
    std::vector<std::string> builtin_cells;
    std::vector<std::string> builtin_styles;
    // An expression grouping's.
    std::string group_key;
    std::string subgroup_key;
    std::string subgroup_label;
    friend bool operator==(const EvaluatedRow&, const EvaluatedRow&) = default;
};

// One expression of a view, on one row, as an editor shows what it gives:
// a header's with group_tracks and group_length as `tracks` and
// `length_ms` say. The error when it does not compile.
[[nodiscard]] core::Result<std::string>
evaluate_view_expression(std::string_view source, titleformat::FormatContextKind context,
                         const ViewRow& row, std::size_t tracks = 1U, std::int64_t length_ms = 0);

// Evaluates rows; an expression that fails leaves its cell empty -- a list
// is not refused for one odd file.
[[nodiscard]] core::Result<std::vector<EvaluatedRow>>
evaluate_list_rows(const CompiledListView& compiled, std::span<const ViewRow> rows,
                   const core::CancellationToken& cancellation = {});

// An expression grouping's groups by their keys alone, nothing evaluated:
// runs of equal key in the list's order, each one's length, and where its
// subgroups begin -- only in a group with more than one.
struct GroupRun {
    std::size_t first{0U};
    std::size_t count{0U};
    std::int64_t length_ms{0};
    std::vector<std::size_t> subgroup_starts;
    friend bool operator==(const GroupRun&, const GroupRun&) = default;
};
[[nodiscard]] std::vector<GroupRun> list_group_runs(std::span<const EvaluatedRow* const> rows,
                                                    std::span<const std::int64_t> durations_ms);

// A group's header, read from its first row with group_tracks and
// group_length beside its tags.
struct GroupHeader {
    std::string title;
    std::string details;
    friend bool operator==(const GroupHeader&, const GroupHeader&) = default;
};
[[nodiscard]] GroupHeader evaluate_group_header(const CompiledListView& compiled,
                                                const ViewRow& first, std::size_t tracks,
                                                std::int64_t length_ms,
                                                const core::CancellationToken& cancellation = {});

// An expression grouping's groups, from evaluated rows: runs of equal key in
// the list's order.
struct EvaluatedGroup {
    std::size_t first{0U};
    std::size_t count{0U};
    std::string title;
    std::string details;
    // Where its subgroups begin, by row, and what each says -- only in a
    // group with more than one.
    std::vector<std::pair<std::size_t, std::string>> subgroups;
    friend bool operator==(const EvaluatedGroup&, const EvaluatedGroup&) = default;
};

[[nodiscard]] core::Result<std::vector<EvaluatedGroup>>
evaluate_list_groups(const CompiledListView& compiled, std::span<const ViewRow> rows,
                     std::span<const EvaluatedRow> evaluated,
                     const core::CancellationToken& cancellation = {});

// ADR-0283: style words as an expression answers them -- separated by
// spaces or commas, unknown ones ignored, so a newer word reads as nothing.
struct CellStyle {
    bool quiet{false};
    bool accent{false};
    bool warning{false};
    bool bold{false};
    bool italic{false};
    [[nodiscard]] bool any() const noexcept { return quiet || accent || warning || bold || italic; }
    friend bool operator==(const CellStyle&, const CellStyle&) = default;
};
[[nodiscard]] CellStyle parse_cell_style(std::string_view words);
// A row's style with its cell's over it: a cell says what it says, and the
// row the rest.
[[nodiscard]] CellStyle combined_style(const CellStyle& row, const CellStyle& cell);

// The keys a column sorts by, one per row: an expression column's sort
// expression, else its display; a built-in column by what it shows.
[[nodiscard]] core::Result<std::vector<std::string>>
list_sort_keys(const CompiledListView& compiled, std::string_view column_id,
               std::span<const ViewRow> rows, const core::CancellationToken& cancellation = {});

// New row -> old row for those keys: natural order (digit runs as numbers,
// case ignored), stable both ways, empty keys last.
[[nodiscard]] std::vector<int> list_sort_order(std::span<const std::string> keys, bool descending);

} // namespace trackknife::lists
