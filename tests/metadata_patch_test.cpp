// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/staged_patch.hpp"

#include <array>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

trackknife::metadata::MetadataField field(std::string name, std::vector<std::string> values) {
    return trackknife::metadata::MetadataField{
        .canonical_name = trackknife::metadata::canonicalize_field_name(name),
        .native_name = std::move(name),
        .values = std::move(values),
        .qualifier = {},
        .provenance = trackknife::metadata::FieldProvenance::embedded,
    };
}

trackknife::metadata::StagedMetadataSelection selection() {
    using trackknife::metadata::MetadataDocument;
    using trackknife::metadata::StagedMetadataSelection;
    using trackknife::metadata::StagedMetadataSource;
    const std::vector<std::string_view> preferred{"Title", "Date", "Genre"};
    auto result = StagedMetadataSelection::create(
        {
            StagedMetadataSource{
                .raw_path = "/music/one.flac",
                .source_revision = std::nullopt,
                .baseline =
                    MetadataDocument{
                        .fields = {field("TITLE", {"One"}), field("DATE", {"2025"})},
                        .unsupported_native_objects = {},
                    },
            },
            StagedMetadataSource{
                .raw_path = "/music/two.flac",
                .source_revision = std::nullopt,
                .baseline =
                    MetadataDocument{
                        .fields = {field("TITLE", {"Two"})},
                        .unsupported_native_objects = {},
                    },
            },
        },
        preferred);
    if (!result) {
        std::cerr << "could not construct metadata patch fixture\n";
        ++failures;
        return {};
    }
    return std::move(*result);
}

void patchesAreSparseExplicitAndDeterministic() {
    using trackknife::metadata::MetadataSelectionFieldState;
    using trackknife::metadata::StagedMetadataPatchKind;
    using trackknife::metadata::StagedMetadataPatchSet;
    const auto baseline = selection();
    const auto title = *baseline.field_index("title");
    const auto date = *baseline.field_index("date");
    const auto genre = *baseline.field_index("genre");
    StagedMetadataPatchSet patches;

    const auto unchanged = patches.replace_values(baseline, 0U, title, {"One"});
    CHECK(unchanged.has_value());
    CHECK(!*unchanged);
    CHECK(patches.empty());

    const auto first = patches.replace_values(baseline, 1U, title, {"Same", "Credit"});
    const auto second = patches.replace_values(baseline, 0U, title, {"Same", "Credit"});
    CHECK(first && *first);
    CHECK(second && *second);
    CHECK(patches.patch_count() == 2U);
    CHECK(patches.field_patch_count(title) == 2U);
    CHECK(patches.total_text_bytes() == 20U);
    CHECK(patches.patch(0U, title)->values == (std::vector<std::string>{"Same", "Credit"}));
    CHECK(patches.project_field(baseline, title).state == MetadataSelectionFieldState::common);

    const auto ordered = patches.patches();
    CHECK(ordered.size() == 2U);
    CHECK(ordered[0].item_index == 0U);
    CHECK(ordered[1].item_index == 1U);

    const auto remove_date = patches.remove_field(baseline, 0U, date);
    CHECK(remove_date && *remove_date);
    CHECK(patches.patch(0U, date)->kind == StagedMetadataPatchKind::remove_field);
    const auto projected_date = patches.project_field(baseline, date);
    CHECK(projected_date.state == MetadataSelectionFieldState::missing);
    CHECK(projected_date.present_item_count == 0U);

    const auto missing_remove = patches.remove_field(baseline, 0U, genre);
    CHECK(missing_remove.has_value());
    CHECK(!*missing_remove);
    const auto add_genre = patches.replace_values(baseline, 0U, genre, {"Rock"});
    CHECK(add_genre && *add_genre);
    CHECK(patches.project_field(baseline, genre).state == MetadataSelectionFieldState::partial);
    CHECK(patches.replace_values(baseline, 1U, genre, {"Rock"}).has_value());
    CHECK(patches.project_field(baseline, genre).state == MetadataSelectionFieldState::common);
    CHECK(patches.replace_values(baseline, 1U, genre, {"Jazz"}).has_value());
    CHECK(patches.project_field(baseline, genre).state == MetadataSelectionFieldState::mixed);

    const auto reverted = patches.revert(baseline, 1U, genre);
    CHECK(reverted && *reverted);
    CHECK(patches.project_field(baseline, genre).state == MetadataSelectionFieldState::partial);

    const std::array both_items{std::size_t{0U}, std::size_t{1U}};
    const auto projected = patches.project_items(baseline, both_items);
    CHECK(projected.has_value());
    if (projected) {
        CHECK((*projected)[title].state == MetadataSelectionFieldState::common);
        CHECK((*projected)[title].common_values == (std::vector<std::string>{"Same", "Credit"}));
        CHECK((*projected)[title].staged_item_count == 2U);
        CHECK((*projected)[date].state == MetadataSelectionFieldState::missing);
        CHECK((*projected)[date].staged_item_count == 1U);
        CHECK((*projected)[genre].state == MetadataSelectionFieldState::partial);
        CHECK((*projected)[genre].present_item_count == 1U);
        CHECK((*projected)[genre].staged_item_count == 1U);
        CHECK((*projected)[genre].common_values.empty());
    }

    // Copies are immutable snapshots: a later edit detaches the live patch
    // state instead of changing an in-flight background projection.
    const auto snapshot = patches;
    CHECK(patches.replace_values(baseline, 1U, genre, {"Jazz"}).has_value());
    const auto old_projection = snapshot.project_items(baseline, both_items);
    const auto new_projection = patches.project_items(baseline, both_items);
    CHECK(old_projection && (*old_projection)[genre].state == MetadataSelectionFieldState::partial);
    CHECK(new_projection && (*new_projection)[genre].state == MetadataSelectionFieldState::mixed);

    const std::array duplicate_items{std::size_t{0U}, std::size_t{0U}};
    CHECK(!patches.project_items(baseline, duplicate_items).has_value());
    const std::array reversed_items{std::size_t{1U}, std::size_t{0U}};
    CHECK(!patches.project_items(baseline, reversed_items).has_value());
    const std::array invalid_items{std::size_t{2U}};
    CHECK(!patches.project_items(baseline, invalid_items).has_value());
    trackknife::core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto cancelled = patches.project_items(baseline, both_items, cancellation.token());
    CHECK(!cancelled.has_value());
    CHECK(cancelled.error().code == trackknife::core::ErrorCode::cancelled);

    patches.clear();
    CHECK(patches.empty());
    CHECK(patches.total_text_bytes() == 0U);
}

void patchesRejectInvalidAndExcessiveDrafts() {
    using trackknife::metadata::StagedMetadataPatchLimits;
    using trackknife::metadata::StagedMetadataPatchSet;
    const auto baseline = selection();
    const auto title = *baseline.field_index("title");
    const auto date = *baseline.field_index("date");

    StagedMetadataPatchSet count_limited{
        StagedMetadataPatchLimits{.patches = 1U, .values_per_patch = 2U, .total_text_bytes = 8U}};
    CHECK(count_limited.replace_values(baseline, 0U, title, {"Draft"}).has_value());
    const auto too_many_patches = count_limited.remove_field(baseline, 0U, date);
    CHECK(!too_many_patches);
    CHECK(too_many_patches.error().code == trackknife::core::ErrorCode::limit_exceeded);

    const auto too_many_values = count_limited.replace_values(baseline, 0U, title, {"a", "b", "c"});
    CHECK(!too_many_values);
    CHECK(too_many_values.error().code == trackknife::core::ErrorCode::limit_exceeded);
    const auto too_much_text = count_limited.replace_values(baseline, 0U, title, {"123456789"});
    CHECK(!too_much_text);
    CHECK(too_much_text.error().code == trackknife::core::ErrorCode::limit_exceeded);

    const auto empty = count_limited.replace_values(baseline, 0U, title, {});
    CHECK(!empty);
    CHECK(empty.error().code == trackknife::core::ErrorCode::invalid_argument);
    const auto invalid = count_limited.remove_field(baseline, 4U, title);
    CHECK(!invalid);
    CHECK(invalid.error().code == trackknife::core::ErrorCode::invalid_argument);
}

void newlyAddedFieldsUseTheOrdinaryPatchContract() {
    using trackknife::metadata::MetadataSelectionFieldState;
    using trackknife::metadata::StagedMetadataPatchSet;
    auto baseline = selection();
    const auto mood = baseline.ensure_missing_field("Mood", "Mood");
    CHECK(mood.has_value());
    if (!mood) {
        return;
    }

    StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(baseline, 0U, *mood, {"Energetic"}).has_value());
    CHECK(patches.project_field(baseline, *mood).state == MetadataSelectionFieldState::partial);
    CHECK(patches.replace_values(baseline, 1U, *mood, {"Energetic"}).has_value());
    CHECK(patches.project_field(baseline, *mood).state == MetadataSelectionFieldState::common);
    CHECK(patches.remove_field(baseline, 0U, *mood).has_value());
    CHECK(patches.patch(0U, *mood) == nullptr);
    CHECK(patches.project_field(baseline, *mood).state == MetadataSelectionFieldState::partial);
}

} // namespace

// ADR-0257: drafts staged on a provisional selection -- cached tags -- carry
// onto the selection built from the files' own, by row and field name.
void draftsCarryOntoTheTagsRead() {
    using trackknife::metadata::MetadataDocument;
    using trackknife::metadata::StagedMetadataPatchKind;
    using trackknife::metadata::StagedMetadataPatchSet;
    using trackknife::metadata::StagedMetadataSelection;
    using trackknife::metadata::StagedMetadataSource;
    const std::vector<std::string_view> preferred{"Title"};
    // What the library cached: titles and an artist.
    auto cached = StagedMetadataSelection::create(
        {StagedMetadataSource{.raw_path = "/music/one.flac",
                              .source_revision = std::nullopt,
                              .baseline = MetadataDocument{.fields = {field("TITLE", {"One"}),
                                                                      field("ARTIST", {"A"})},
                                                           .unsupported_native_objects = {}}},
         StagedMetadataSource{.raw_path = "/music/two.flac",
                              .source_revision = std::nullopt,
                              .baseline = MetadataDocument{.fields = {field("TITLE", {"Two"}),
                                                                      field("ARTIST", {"A"})},
                                                           .unsupported_native_objects = {}}}},
        preferred);
    CHECK(cached.has_value());
    // What the files say: a field the cache did not have, in another order,
    // and the second title already what the draft makes it.
    auto read = StagedMetadataSelection::create(
        {StagedMetadataSource{.raw_path = "/music/one.flac",
                              .source_revision = trackknife::core::LocalSourceRevision{},
                              .baseline = MetadataDocument{.fields = {field("COMMENT", {"x"}),
                                                                      field("TITLE", {"One"}),
                                                                      field("ARTIST", {"A"})},
                                                           .unsupported_native_objects = {}}},
         StagedMetadataSource{.raw_path = "/music/two.flac",
                              .source_revision = trackknife::core::LocalSourceRevision{},
                              .baseline = MetadataDocument{.fields = {field("TITLE", {"New"})},
                                                           .unsupported_native_objects = {}}}},
        preferred);
    CHECK(read.has_value());
    if (!cached || !read) {
        return;
    }
    StagedMetadataPatchSet drafts;
    const auto title = *cached->field_index("title");
    const auto artist = *cached->field_index("artist");
    CHECK(drafts.replace_values(*cached, 0U, title, {"New"}).value_or(false));
    CHECK(drafts.replace_values(*cached, 1U, title, {"New"}).value_or(false));
    CHECK(drafts.remove_field(*cached, 1U, artist).value_or(false));
    const auto genre = cached->ensure_missing_field("genre", "Genre");
    CHECK(genre.has_value());
    CHECK(drafts.replace_values(*cached, 0U, *genre, {"Rock"}).value_or(false));

    auto onto = *read;
    const auto carried = trackknife::metadata::carry_staged_patches(*cached, drafts, onto);
    CHECK(carried.has_value());
    if (!carried) {
        return;
    }
    // One's title and genre carry; two's title is already "New" and two has
    // no artist to remove: both dropped, and counted.
    CHECK(carried->carried == 2U && carried->dropped == 2U);
    const auto read_title = *onto.field_index("title");
    const auto* one_title = carried->patches.patch(0U, read_title);
    CHECK(one_title != nullptr && one_title->values == std::vector<std::string>{"New"} &&
          one_title->kind == StagedMetadataPatchKind::replace_values);
    const auto read_genre = onto.field_index("genre");
    CHECK(read_genre.has_value());
    CHECK(read_genre && carried->patches.patch(0U, *read_genre) != nullptr);
    CHECK(onto.field_index("comment").has_value());

    // Other rows -- another selection altogether -- are refused.
    auto other = StagedMetadataSelection::create(
        {StagedMetadataSource{.raw_path = "/music/else.flac",
                              .source_revision = std::nullopt,
                              .baseline = MetadataDocument{.fields = {},
                                                           .unsupported_native_objects = {}}},
         StagedMetadataSource{.raw_path = "/music/two.flac",
                              .source_revision = std::nullopt,
                              .baseline = MetadataDocument{.fields = {},
                                                           .unsupported_native_objects = {}}}},
        preferred);
    CHECK(other.has_value());
    if (other) {
        CHECK(!trackknife::metadata::carry_staged_patches(*cached, drafts, *other));
    }
}

int main() {
    patchesAreSparseExplicitAndDeterministic();
    patchesRejectInvalidAndExcessiveDrafts();
    newlyAddedFieldsUseTheOrdinaryPatchContract();
    draftsCarryOntoTheTagsRead();
    return failures == 0 ? 0 : 1;
}
