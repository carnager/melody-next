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

struct ViewColumn {
    // A built-in id, or an expression column's own ("expression-1", ...).
    std::string id;
    bool visible{true};
    // Expression columns only.
    std::string title;
    std::string display;
    // Empty: sorted by what it shows.
    std::string sort;
    ColumnAlignment alignment{ColumnAlignment::left};
    bool expands{false};

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
    // In their order; every built-in column is here, shown or not.
    std::vector<ViewColumn> columns;
    bool builtin{false};

    friend bool operator==(const ListView&, const ListView&) = default;
};

// The shipped views: the four presentations ADR-0027 had, each as it was.
inline constexpr std::string_view albums_beside_view_id = "albums-side-artwork";
inline constexpr std::string_view albums_header_view_id = "albums-header-artwork";
inline constexpr std::string_view columns_view_id = "plain-columns";
inline constexpr std::string_view compact_view_id = "compact-queue";
[[nodiscard]] const std::vector<ListView>& shipped_list_views();
[[nodiscard]] const ListView* shipped_list_view(std::string_view id);

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
};

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
                .rating = rating};
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
};

// What a view makes of one row.
struct EvaluatedRow {
    // One per expression column, in the view's order.
    std::vector<std::string> cells;
    // An expression grouping's.
    std::string group_key;
    std::string subgroup_key;
    std::string subgroup_label;
    friend bool operator==(const EvaluatedRow&, const EvaluatedRow&) = default;
};

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

// The keys a column sorts by, one per row: an expression column's sort
// expression, else its display; a built-in column by what it shows.
[[nodiscard]] core::Result<std::vector<std::string>>
list_sort_keys(const CompiledListView& compiled, std::string_view column_id,
               std::span<const ViewRow> rows, const core::CancellationToken& cancellation = {});

// New row -> old row for those keys: natural order (digit runs as numbers,
// case ignored), stable both ways, empty keys last.
[[nodiscard]] std::vector<int> list_sort_order(std::span<const std::string> keys, bool descending);

} // namespace trackknife::lists
