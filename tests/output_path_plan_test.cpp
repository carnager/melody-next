// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/operations/output_path_plan.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <optional>
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

trackknife::metadata::MetadataDocument document(std::string title, std::string track = "1",
                                                std::string album = "Album",
                                                std::string album_artist = "Artist") {
    return trackknife::metadata::MetadataDocument{
        .fields = {field("TITLE", {std::move(title)}), field("TRACKNUMBER", {std::move(track)}),
                   field("ALBUM", {std::move(album)}),
                   field("ALBUM ARTIST", {std::move(album_artist)})},
        .unsupported_native_objects = {},
    };
}

trackknife::core::LocalSourceRevision revision(const std::uint64_t inode) {
    return {.device = 7U,
            .inode = inode,
            .size = 1'024U,
            .modification_time_seconds = 10,
            .modification_time_nanoseconds = 20};
}

trackknife::operations::OutputLayoutProfile layout() {
    return trackknife::operations::OutputLayoutProfile{
        .schema_version = 1U,
        .name = "Album folders",
        .dialect = {},
        .relative_directory_expression = "%album artist%/%album%",
        .basename_expression = "$num(%tracknumber%,2) - %title%",
        .sanitization_policy = {"linux", 1U},
    };
}

trackknife::operations::DestinationProfile destination() {
    return trackknife::operations::DestinationProfile{
        .schema_version = 1U,
        .name = "Library",
        .root_raw_path = "/library",
        .containment_policy = {"lexical-beneath-root", 1U},
    };
}

bool has_issue(const trackknife::operations::OutputPathPlan& plan,
               const trackknife::operations::OutputPathPlanIssueKind kind) {
    return std::ranges::any_of(plan.issues,
                               [kind](const auto& issue) { return issue.kind == kind; });
}

void renameAndMoveUseFinalMetadataAndExposeSanitization() {
    using namespace trackknife::operations;
    const std::array items{
        OutputPathPlanningItem{.item_index = 0U,
                               .source_raw_path = "/incoming/a.flac",
                               .source_revision = revision(1U),
                               .final_metadata = document("One/Intro")},
        OutputPathPlanningItem{.item_index = 1U,
                               .source_raw_path = "/incoming/b.flac",
                               .source_revision = revision(2U),
                               .final_metadata = document("Two", "2")},
    };
    const auto plan = plan_output_paths(
        items, {.rename_files = true, .move_files = true}, layout(), destination(),
        OutputPathPlanningSnapshot{
            .existing_paths = {{"/library", ObservedOutputPathKind::directory}},
            .ascii_case_insensitive = false});
    if (!plan) {
        std::cerr << plan.error().message << '\n';
    }
    CHECK(plan.has_value());
    if (!plan) {
        return;
    }
    CHECK(plan->ready());
    CHECK(plan->issues.empty());
    CHECK(plan->sources.size() == 2U);
    CHECK(plan->sources[0].raw_relative_directory == "Artist/Album");
    CHECK(plan->sources[0].sanitized_relative_directory == "Artist/Album");
    CHECK(plan->sources[0].raw_basename == "01 - One/Intro");
    CHECK(plan->sources[0].sanitized_basename == "01 - One_Intro");
    CHECK(plan->sources[0].sanitized);
    CHECK(plan->sources[0].target_raw_path == "/library/Artist/Album/01 - One_Intro.flac");
    CHECK(plan->sources[1].target_raw_path == "/library/Artist/Album/02 - Two.flac");
}

void independentTogglesPreserveDirectoryExtensionAndRawFilename() {
    using namespace trackknife::operations;
    auto rename_layout = layout();
    rename_layout.relative_directory_expression = "/ignored/when/rename-only";
    rename_layout.basename_expression = "%title%";
    const std::array rename_items{OutputPathPlanningItem{
        .item_index = 0U,
        .source_raw_path = "/incoming/Original.FLAC",
        .source_revision = revision(1U),
        .final_metadata = document("Renamed"),
    }};
    const auto renamed = plan_output_paths(
        rename_items, {.rename_files = true, .move_files = false}, rename_layout, std::nullopt);
    CHECK(renamed.has_value());
    CHECK(renamed && renamed->ready());
    CHECK(renamed && renamed->sources[0].raw_relative_directory.empty());
    CHECK(renamed && renamed->sources[0].target_raw_path == "/incoming/Renamed.FLAC");

    const std::string invalid_filename{"bad-\xff.flac", 10U};
    const std::string invalid_source = "/incoming/" + invalid_filename;
    const std::array move_items{OutputPathPlanningItem{
        .item_index = 0U,
        .source_raw_path = invalid_source,
        .source_revision = revision(1U),
        .final_metadata = document("Unused", "1", "Sorted", "Ignored"),
    }};
    const auto moved = plan_output_paths(move_items, {.rename_files = false, .move_files = true},
                                         layout(), destination());
    CHECK(moved.has_value());
    CHECK(moved && moved->ready());
    CHECK(moved && moved->sources[0].raw_basename.empty());
    CHECK(moved && moved->sources[0].target_raw_path ==
                       std::string{"/library/Ignored/Sorted/"} + invalid_filename);

    auto trailing_destination = destination();
    trailing_destination.root_raw_path += '/';
    const auto moved_below_trailing_root =
        plan_output_paths(move_items, {.rename_files = false, .move_files = true}, layout(),
                          std::move(trailing_destination));
    CHECK(moved_below_trailing_root.has_value());
    CHECK(moved_below_trailing_root && moved_below_trailing_root->ready());
    CHECK(moved_below_trailing_root &&
          moved_below_trailing_root->sources[0].target_raw_path ==
              std::string{"/library/Ignored/Sorted/"} + invalid_filename);
}

void linuxSanitizationAndTechnicalContextAreExact() {
    using namespace trackknife::operations;
    auto exact = layout();
    exact.relative_directory_expression = "%album%";
    exact.basename_expression = "$info(filename)-%title%";
    const std::string title_with_nul{"T\0x", 3U};
    const std::array items{OutputPathPlanningItem{
        .item_index = 0U,
        .source_raw_path = "/incoming/Original.FLAC",
        .source_revision = revision(1U),
        .final_metadata = document(title_with_nul, "1", "A//./../"),
    }};
    const auto planned =
        plan_output_paths(items, {.rename_files = true, .move_files = true}, exact, destination());
    CHECK(planned.has_value());
    CHECK(planned && planned->ready());
    CHECK(planned && planned->sources[0].raw_relative_directory == "A//./../");
    CHECK(planned && planned->sources[0].sanitized_relative_directory == "A/_/_/__/_");
    CHECK(planned && planned->sources[0].raw_basename == (std::string{"Original-T\0x", 12U}));
    CHECK(planned && planned->sources[0].sanitized_basename == "Original-T_x");
    CHECK(planned &&
          planned->sources[0].target_raw_path == "/library/A/_/_/__/_/Original-T_x.FLAC");

    auto invalid_utf8 = items;
    invalid_utf8[0].final_metadata = document(std::string{"\xff", 1U});
    exact.basename_expression = "%title%";
    const auto rejected = plan_output_paths(
        invalid_utf8, {.rename_files = true, .move_files = false}, exact, std::nullopt);
    CHECK(rejected.has_value());
    CHECK(rejected && !rejected->ready());
    CHECK(rejected && has_issue(*rejected, OutputPathPlanIssueKind::invalid_expression_output));
}

void portableSanitizationAvoidsCrossPlatformNames() {
    using namespace trackknife::operations;
    auto portable = layout();
    portable.sanitization_policy = {"portable", 1U};
    portable.relative_directory_expression = "%albumartist%/%album%";
    portable.basename_expression = "%title%";
    const std::array items{OutputPathPlanningItem{
        .item_index = 0U,
        .source_raw_path = "/incoming/Original.flac",
        .source_revision = revision(1U),
        .final_metadata = document("CON", "1", "Artist:Name", "Album. "),
    }};
    const auto planned = plan_output_paths(items, {.rename_files = true, .move_files = true},
                                           portable, destination());
    CHECK(planned.has_value());
    CHECK(planned && planned->ready());
    CHECK(planned && planned->sources[0].sanitized_relative_directory == "Album__/Artist_Name");
    CHECK(planned && planned->sources[0].sanitized_basename == "_CON");
    CHECK(planned &&
          planned->sources[0].target_raw_path == "/library/Album__/Artist_Name/_CON.flac");

    portable.sanitization_policy = {"portable", 2U};
    CHECK(!validate_output_layout_profile(portable).has_value());
}

void forcedTargetExtensionRenamesAndResolvesInExpressions() {
    using namespace trackknife::operations;
    OutputLayoutProfile forced_layout;
    forced_layout.name = "forced";
    forced_layout.relative_directory_expression = "";
    forced_layout.basename_expression = "%title%.$info(extension)-suffix";
    const std::array items{OutputPathPlanningItem{
        .item_index = 0U,
        .source_raw_path = "/incoming/Original.FLAC",
        .source_revision = revision(1U),
        .final_metadata = document("Converted"),
    }};
    const auto planned = plan_output_paths(
        items, {.rename_files = true, .move_files = false}, forced_layout, std::nullopt, {}, {}, {},
        ConvertedPublicationPolicy{.target_extension = "opus", .mirror_source_root_raw_path = {}});
    CHECK(planned.has_value());
    CHECK(planned && planned->ready());
    // The expression's %extension% resolves to the forced target and the
    // planned filename carries it instead of the source's.
    CHECK(planned && planned->sources[0].target_raw_path == "/incoming/Converted.opus-suffix.opus");

    const auto unforced = plan_output_paths(items, {.rename_files = true, .move_files = false},
                                            forced_layout, std::nullopt);
    CHECK(unforced.has_value());
    CHECK(unforced &&
          unforced->sources[0].target_raw_path == "/incoming/Converted.FLAC-suffix.FLAC");

    // Cue subtracks share one physical source and fan out to one converted
    // target per item — never merged, never a shared-source conflict.
    OutputLayoutProfile fanout_layout;
    fanout_layout.name = "fanout";
    fanout_layout.relative_directory_expression = "";
    fanout_layout.basename_expression = "%title%";
    const std::array fanout_items{OutputPathPlanningItem{
                                      .item_index = 0U,
                                      .source_raw_path = "/incoming/Album Image.flac",
                                      .source_revision = revision(1U),
                                      .final_metadata = document("First"),
                                  },
                                  OutputPathPlanningItem{
                                      .item_index = 1U,
                                      .source_raw_path = "/incoming/Album Image.flac",
                                      .source_revision = revision(1U),
                                      .final_metadata = document("Second"),
                                  }};
    const auto fanned = plan_output_paths(
        fanout_items, {.rename_files = true, .move_files = false}, fanout_layout, std::nullopt, {},
        {}, {},
        ConvertedPublicationPolicy{.target_extension = "opus", .mirror_source_root_raw_path = {}});
    CHECK(fanned.has_value());
    CHECK(fanned && fanned->ready());
    CHECK(fanned && fanned->sources.size() == 2U);
    CHECK(fanned && fanned->sources[0].target_raw_path == "/incoming/First.opus");
    CHECK(fanned && fanned->sources[1].target_raw_path == "/incoming/Second.opus");

    // Without the converted policy the same items still merge and block.
    const auto relocated = plan_output_paths(
        fanout_items, {.rename_files = true, .move_files = false}, fanout_layout, std::nullopt);
    CHECK(relocated.has_value());
    CHECK(relocated && !relocated->ready());
}

void sharedSourcesAndPathCollisionsFailClosed() {
    using namespace trackknife::operations;
    auto flat = layout();
    flat.relative_directory_expression = {};
    flat.basename_expression = "%title%";
    const std::array shared_items{
        OutputPathPlanningItem{.item_index = 0U,
                               .source_raw_path = "/incoming/shared.flac",
                               .source_revision = revision(1U),
                               .final_metadata = document("First")},
        OutputPathPlanningItem{.item_index = 1U,
                               .source_raw_path = "/incoming/shared.flac",
                               .source_revision = revision(1U),
                               .final_metadata = document("Second")},
    };
    const auto shared = plan_output_paths(shared_items, {.rename_files = true, .move_files = true},
                                          flat, destination());
    CHECK(shared.has_value());
    CHECK(shared && !shared->ready());
    CHECK(shared && shared->sources.size() == 1U);
    CHECK(shared && shared->sources[0].item_indexes == (std::vector<std::size_t>{0U, 1U}));
    CHECK(shared && has_issue(*shared, OutputPathPlanIssueKind::shared_source_target_conflict));

    const std::array collision_items{
        OutputPathPlanningItem{.item_index = 0U,
                               .source_raw_path = "/incoming/a.flac",
                               .source_revision = revision(1U),
                               .final_metadata = document("Same")},
        OutputPathPlanningItem{.item_index = 1U,
                               .source_raw_path = "/incoming/b.flac",
                               .source_revision = revision(2U),
                               .final_metadata = document("Same")},
    };
    const auto collision = plan_output_paths(
        collision_items, {.rename_files = true, .move_files = true}, flat, destination(),
        OutputPathPlanningSnapshot{
            .existing_paths = {{"/library/Same.flac", ObservedOutputPathKind::file},
                               {"/library", ObservedOutputPathKind::file}},
            .ascii_case_insensitive = false,
        });
    CHECK(collision.has_value());
    CHECK(collision && !collision->ready());
    CHECK(collision && has_issue(*collision, OutputPathPlanIssueKind::duplicate_target));
    CHECK(collision && has_issue(*collision, OutputPathPlanIssueKind::existing_target));
    CHECK(collision && has_issue(*collision, OutputPathPlanIssueKind::target_parent_not_directory));
}

void dependenciesCaseChangesAndContainmentAreVisible() {
    using namespace trackknife::operations;
    auto flat = layout();
    flat.relative_directory_expression = {};
    flat.basename_expression = "%title%";
    const std::array dependency_items{
        OutputPathPlanningItem{.item_index = 0U,
                               .source_raw_path = "/incoming/a.flac",
                               .source_revision = revision(1U),
                               .final_metadata = document("b")},
        OutputPathPlanningItem{.item_index = 1U,
                               .source_raw_path = "/incoming/b.flac",
                               .source_revision = revision(2U),
                               .final_metadata = document("c")},
    };
    const auto dependency = plan_output_paths(
        dependency_items, {.rename_files = true, .move_files = false}, flat, std::nullopt,
        OutputPathPlanningSnapshot{
            .existing_paths = {{"/incoming/a.flac", ObservedOutputPathKind::file},
                               {"/incoming/b.flac", ObservedOutputPathKind::file}},
            .ascii_case_insensitive = false,
        });
    CHECK(dependency.has_value());
    CHECK(dependency && !dependency->ready());
    CHECK(dependency && has_issue(*dependency, OutputPathPlanIssueKind::source_target_dependency));

    const std::array case_items{OutputPathPlanningItem{
        .item_index = 0U,
        .source_raw_path = "/incoming/song.flac",
        .source_revision = revision(1U),
        .final_metadata = document("Song"),
    }};
    const auto case_change = plan_output_paths(
        case_items, {.rename_files = true, .move_files = false}, flat, std::nullopt);
    CHECK(case_change.has_value());
    CHECK(case_change && case_change->ready());
    CHECK(case_change && has_issue(*case_change, OutputPathPlanIssueKind::case_only_change));
    CHECK(case_change && !case_change->issues.front().blocking);

    auto absolute = layout();
    absolute.relative_directory_expression = "/outside";
    const auto escaped = plan_output_paths(case_items, {.rename_files = true, .move_files = true},
                                           absolute, destination());
    CHECK(escaped.has_value());
    CHECK(escaped && !escaped->ready());
    CHECK(escaped && has_issue(*escaped, OutputPathPlanIssueKind::absolute_relative_directory));
    CHECK(escaped && escaped->sources[0].target_raw_path.starts_with("/library/"));
}

void physicalAliasesAndInconsistentSharedRevisionsFailClosed() {
    using namespace trackknife::operations;
    auto flat = layout();
    flat.relative_directory_expression = {};
    flat.basename_expression = "%title%";
    const std::array aliases{
        OutputPathPlanningItem{.item_index = 0U,
                               .source_raw_path = "/incoming/first.flac",
                               .source_revision = revision(9U),
                               .final_metadata = document("First")},
        OutputPathPlanningItem{.item_index = 1U,
                               .source_raw_path = "/incoming/alias.flac",
                               .source_revision = revision(9U),
                               .final_metadata = document("Alias")},
    };
    const auto alias_plan =
        plan_output_paths(aliases, {.rename_files = true, .move_files = false}, flat, std::nullopt);
    CHECK(alias_plan.has_value());
    CHECK(alias_plan && !alias_plan->ready());
    CHECK(alias_plan && has_issue(*alias_plan, OutputPathPlanIssueKind::physical_source_alias));

    auto changed_revision = aliases;
    changed_revision[1].source_raw_path = changed_revision[0].source_raw_path;
    changed_revision[1].source_revision.modification_time_nanoseconds = 21;
    const auto inconsistent = plan_output_paths(
        changed_revision, {.rename_files = true, .move_files = false}, flat, std::nullopt);
    CHECK(inconsistent.has_value());
    CHECK(inconsistent && !inconsistent->ready());
    CHECK(inconsistent &&
          has_issue(*inconsistent, OutputPathPlanIssueKind::shared_source_revision_conflict));
}

void validationLimitsAndCancellationRejectInvalidPlans() {
    using namespace trackknife;
    auto invalid_layout = layout();
    invalid_layout.basename_expression = "$unknown(%title%)";
    CHECK(!operations::validate_output_layout_profile(invalid_layout));

    auto root = destination();
    root.root_raw_path = "/";
    CHECK(!operations::validate_destination_profile(root));

    const std::array items{operations::OutputPathPlanningItem{
        .item_index = 0U,
        .source_raw_path = "/incoming/a.flac",
        .source_revision = revision(1U),
        .final_metadata = document("A very long title"),
    }};
    CHECK(!operations::plan_output_paths(items, {}, layout(), std::nullopt));

    auto missing_revision = items;
    missing_revision[0].source_revision = {};
    CHECK(!operations::plan_output_paths(
        missing_revision, {.rename_files = true, .move_files = false}, layout(), std::nullopt));

    auto limited = operations::plan_output_paths(
        items, {.rename_files = true, .move_files = false}, layout(), std::nullopt, {}, {},
        operations::OutputPathPlanningLimits{.items = 1U,
                                             .issues = 8U,
                                             .profile_name_bytes = 128U,
                                             .expression_bytes = 1'024U,
                                             .root_path_bytes = 1'024U,
                                             .component_bytes = 4U,
                                             .path_bytes = 4'095U});
    CHECK(limited.has_value());
    CHECK(limited && !limited->ready());
    CHECK(limited && has_issue(*limited, operations::OutputPathPlanIssueKind::component_too_long));

    core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto cancelled =
        operations::plan_output_paths(items, {.rename_files = true, .move_files = false}, layout(),
                                      std::nullopt, {}, cancellation.token());
    CHECK(!cancelled);
    CHECK(cancelled.error().code == core::ErrorCode::cancelled);
}

} // namespace

// ADR-0132: mirror mode reproduces the source structure below the inferred
// root byte-exactly, swaps the extension, and fails closed for sources
// outside the root or logical items sharing one file.
void mirrorModeReproducesSourceStructureBelowTheRoot() {
    using namespace trackknife::operations;
    const std::vector<std::string> paths{"/music/Artist/Album/01 - One.flac",
                                         "/music/Artist/Album/CD2/02.flac",
                                         "/music/Artist/one.flac"};
    CHECK(common_source_directory_raw_path(paths) == "/music/Artist");
    CHECK(common_source_directory_raw_path({}).empty());
    const std::vector<std::string> disjoint{"/music/a.flac", "/video/b.flac"};
    CHECK(common_source_directory_raw_path(disjoint) == "/");
    const std::vector<std::string> relative{"relative.flac"};
    CHECK(common_source_directory_raw_path(relative).empty());

    const auto mirrored_policy = [](std::string root) {
        return ConvertedPublicationPolicy{.target_extension = "opus",
                                          .mirror_source_root_raw_path = std::move(root)};
    };
    const std::array items{
        OutputPathPlanningItem{.item_index = 0U,
                               .source_raw_path = paths[0],
                               .source_revision = revision(1U),
                               .final_metadata = document("Ignored")},
        OutputPathPlanningItem{.item_index = 1U,
                               .source_raw_path = paths[1],
                               .source_revision = revision(2U),
                               .final_metadata = document("Ignored")},
        OutputPathPlanningItem{.item_index = 2U,
                               .source_raw_path = paths[2],
                               .source_revision = revision(3U),
                               .final_metadata = document("Ignored")},
    };
    const auto plan =
        plan_output_paths(items, {.rename_files = true, .move_files = true}, layout(),
                          destination(), {}, {}, {}, mirrored_policy("/music/Artist"));
    CHECK(plan.has_value());
    if (!plan) {
        std::cerr << plan.error().message << '\n';
        return;
    }
    CHECK(plan->ready());
    CHECK(plan->issues.empty());
    CHECK(plan->sources.size() == 3U);
    CHECK(plan->sources[0].raw_relative_directory == "Album");
    CHECK(plan->sources[0].raw_basename == "01 - One");
    CHECK(plan->sources[0].target_raw_path == "/library/Album/01 - One.opus");
    CHECK(plan->sources[1].target_raw_path == "/library/Album/CD2/02.opus");
    CHECK(plan->sources[2].raw_relative_directory.empty());
    CHECK(plan->sources[2].target_raw_path == "/library/one.opus");

    // Non-UTF-8 source names mirror byte-exactly; only the extension changes.
    const std::string invalid_filename{"bad-\xff.flac", 10U};
    const std::array raw_items{OutputPathPlanningItem{
        .item_index = 0U,
        .source_raw_path = "/incoming/deep/" + invalid_filename,
        .source_revision = revision(4U),
        .final_metadata = document("Ignored"),
    }};
    const auto raw_plan =
        plan_output_paths(raw_items, {.rename_files = true, .move_files = true}, layout(),
                          destination(), {}, {}, {}, mirrored_policy("/incoming"));
    CHECK(raw_plan.has_value() && raw_plan->ready());
    const std::string mirrored_raw_target{"/library/deep/bad-\xff.opus", 24U};
    CHECK(raw_plan && raw_plan->sources[0].target_raw_path == mirrored_raw_target);

    // A source outside the mirror root blocks instead of guessing.
    const std::array outside_items{OutputPathPlanningItem{
        .item_index = 0U,
        .source_raw_path = "/elsewhere/track.flac",
        .source_revision = revision(5U),
        .final_metadata = document("Ignored"),
    }};
    const auto outside =
        plan_output_paths(outside_items, {.rename_files = true, .move_files = true}, layout(),
                          destination(), {}, {}, {}, mirrored_policy("/music/Artist"));
    CHECK(outside.has_value() && !outside->ready());
    CHECK(outside && has_issue(*outside, OutputPathPlanIssueKind::mirror_source_outside_root));

    // Logical items sharing one physical source map onto one mirrored name:
    // the duplicate-target conflict fails closed, mirror mode never splits.
    const std::array shared_items{
        OutputPathPlanningItem{.item_index = 0U,
                               .source_raw_path = "/music/Artist/image.flac",
                               .source_revision = revision(6U),
                               .final_metadata = document("First")},
        OutputPathPlanningItem{.item_index = 1U,
                               .source_raw_path = "/music/Artist/image.flac",
                               .source_revision = revision(6U),
                               .final_metadata = document("Second")},
    };
    const auto shared =
        plan_output_paths(shared_items, {.rename_files = true, .move_files = true}, layout(),
                          destination(), {}, {}, {}, mirrored_policy("/music/Artist"));
    CHECK(shared.has_value() && !shared->ready());
    CHECK(shared && has_issue(*shared, OutputPathPlanIssueKind::duplicate_target));

    // The mirror root itself must be a normalized absolute path.
    CHECK(!plan_output_paths(items, {.rename_files = true, .move_files = true}, layout(),
                             destination(), {}, {}, {}, mirrored_policy("relative"))
               .has_value());
}

int main() {
    mirrorModeReproducesSourceStructureBelowTheRoot();
    renameAndMoveUseFinalMetadataAndExposeSanitization();
    independentTogglesPreserveDirectoryExtensionAndRawFilename();
    forcedTargetExtensionRenamesAndResolvesInExpressions();
    linuxSanitizationAndTechnicalContextAreExact();
    portableSanitizationAvoidsCrossPlatformNames();
    sharedSourcesAndPathCollisionsFailClosed();
    dependenciesCaseChangesAndContainmentAreVisible();
    physicalAliasesAndInconsistentSharedRevisionsFailClosed();
    validationLimitsAndCancellationRejectInvalidPlans();
    return failures == 0 ? 0 : 1;
}
