// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/stable_id.hpp"
#include "trackknife/metadata/write_plan.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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

[[nodiscard]] trackknife::core::LocalSourceRevision revision(const std::uint64_t inode,
                                                             const std::uint64_t size = 1'024U) {
    return trackknife::core::LocalSourceRevision{
        .device = 7U,
        .inode = inode,
        .size = size,
        .modification_time_seconds = 100,
        .modification_time_nanoseconds = 200,
    };
}

[[nodiscard]] trackknife::metadata::MetadataField
field(std::string name, std::vector<std::string> values,
      const trackknife::metadata::FieldProvenance provenance =
          trackknife::metadata::FieldProvenance::embedded) {
    return trackknife::metadata::MetadataField{
        .canonical_name = trackknife::metadata::canonicalize_field_name(name),
        .native_name = std::move(name),
        .values = std::move(values),
        .qualifier = {},
        .provenance = provenance,
    };
}

[[nodiscard]] trackknife::metadata::StagedMetadataSource
source(std::string path, std::optional<trackknife::core::LocalSourceRevision> source_revision,
       std::vector<trackknife::metadata::MetadataField> fields) {
    return trackknife::metadata::StagedMetadataSource{
        .raw_path = std::move(path),
        .source_revision = source_revision,
        .baseline =
            trackknife::metadata::MetadataDocument{
                .fields = std::move(fields),
                .unsupported_native_objects = {},
            },
    };
}

[[nodiscard]] trackknife::metadata::StagedMetadataSelection shared_source_selection() {
    using trackknife::metadata::FieldProvenance;
    using trackknife::metadata::StagedMetadataSelection;
    auto result = StagedMetadataSelection::create({
        source("/music/shared.flac", revision(11U),
               {field("TITLE", {"Physical title"}),
                field("TRACKNUMBER", {"1"}, FieldProvenance::segment)}),
        source("/music/shared.flac", revision(11U),
               {field("TITLE", {"Physical title"}),
                field("TRACKNUMBER", {"2"}, FieldProvenance::segment)}),
        source("/music/other.flac", revision(12U), {field("DATE", {"2024"})}),
    });
    CHECK(result.has_value());
    return result ? std::move(*result) : StagedMetadataSelection{};
}

[[nodiscard]] trackknife::metadata::LocalMetadataRead
read(std::string path, const trackknife::core::LocalSourceRevision source_revision,
     std::vector<trackknife::metadata::MetadataField> fields, const bool writable = true,
     const bool preserves_unknown = true) {
    return trackknife::metadata::LocalMetadataRead{
        .raw_path = std::move(path),
        .source_revision = source_revision,
        .document =
            trackknife::metadata::MetadataDocument{
                .fields = std::move(fields),
                .unsupported_native_objects = {},
            },
        .adapter_name = "fixture-writer-v1",
        .capabilities =
            trackknife::metadata::MetadataCapabilities{
                .fields_readable = true,
                .fields_writable = writable,
                .pictures_readable = true,
                .pictures_writable = false,
                .unknown_data_preserved_on_write = preserves_unknown,
            },
    };
}

[[nodiscard]] bool has_issue(const trackknife::metadata::MetadataWritePlanSource& source_plan,
                             const trackknife::metadata::MetadataWritePlanIssueKind kind) {
    return std::ranges::any_of(source_plan.issues,
                               [kind](const auto& issue) { return issue.kind == kind; });
}

[[nodiscard]] bool has_cue_issue(const trackknife::metadata::MetadataWritePlanCueSheet& sheet_plan,
                                 const trackknife::metadata::MetadataWritePlanIssueKind kind) {
    return std::ranges::any_of(sheet_plan.issues,
                               [kind](const auto& issue) { return issue.kind == kind; });
}

void groupsEveryIntentAndReportsLogicalConflicts() {
    using trackknife::metadata::MetadataWritePlanIssueKind;
    using trackknife::metadata::StagedMetadataPatchKind;
    using trackknife::metadata::StagedMetadataPatchSet;
    const auto selection = shared_source_selection();
    const auto title = *selection.field_index("title");
    const auto track_number = *selection.field_index("tracknumber");
    const auto date = *selection.field_index("date");
    auto extended = selection;
    const auto genre = extended.ensure_missing_field("GENRE", "GENRE");
    CHECK(genre.has_value());

    StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(extended, 0U, title, {"First intent"}).has_value());
    CHECK(patches.replace_values(extended, 1U, title, {"Second intent"}).has_value());
    CHECK(patches.replace_values(extended, 0U, *genre, {"Rock"}).has_value());
    CHECK(patches.replace_values(extended, 1U, *genre, {"Rock"}).has_value());
    CHECK(patches.replace_values(extended, 0U, track_number, {"9"}).has_value());
    CHECK(patches.remove_field(extended, 2U, date).has_value());

    std::unordered_map<std::string, int> reads;
    const auto planned = trackknife::metadata::build_metadata_write_plan(
        extended, patches,
        [&reads](const std::string& path, const trackknife::core::CancellationToken&) {
            ++reads[path];
            if (path == "/music/shared.flac") {
                return trackknife::core::Result<trackknife::metadata::LocalMetadataRead>{
                    read(path, revision(11U),
                         {field("TITLE", {"Physical title"}), field("TRACKNUMBER", {"1"})})};
            }
            return trackknife::core::Result<trackknife::metadata::LocalMetadataRead>{
                read(path, revision(12U), {field("DATE", {"2024"})})};
        });
    CHECK(planned.has_value());
    if (!planned) {
        return;
    }
    CHECK(planned->patch_count == 6U);
    CHECK(planned->sources.size() == 2U);
    CHECK(reads["/music/shared.flac"] == 1);
    CHECK(reads["/music/other.flac"] == 1);

    const auto& shared = planned->sources[0];
    CHECK(shared.raw_path == "/music/shared.flac");
    CHECK(shared.occurrence_indexes == (std::vector<std::size_t>{0U, 1U}));
    CHECK(shared.changes.size() == 3U);
    CHECK(!shared.ready());
    CHECK(has_issue(shared, MetadataWritePlanIssueKind::conflicting_logical_edits));
    CHECK(has_issue(shared, MetadataWritePlanIssueKind::unresolved_non_embedded_target));

    const auto title_change = std::ranges::find(
        shared.changes, title, &trackknife::metadata::MetadataWritePlanChange::field_index);
    CHECK(title_change != shared.changes.end());
    CHECK(title_change != shared.changes.end() && title_change->original_present);
    CHECK(title_change != shared.changes.end() &&
          title_change->original_values == (std::vector<std::string>{"Physical title"}));
    CHECK(title_change != shared.changes.end() && title_change->intents.size() == 2U);
    CHECK(title_change != shared.changes.end() && title_change->conflicting_intents);

    const auto genre_change = std::ranges::find(
        shared.changes, *genre, &trackknife::metadata::MetadataWritePlanChange::field_index);
    CHECK(genre_change != shared.changes.end());
    CHECK(genre_change != shared.changes.end() && !genre_change->original_present);
    CHECK(genre_change != shared.changes.end() && genre_change->intents.size() == 2U);
    CHECK(genre_change != shared.changes.end() && !genre_change->conflicting_intents);

    const auto track_change = std::ranges::find(
        shared.changes, track_number, &trackknife::metadata::MetadataWritePlanChange::field_index);
    CHECK(track_change != shared.changes.end());
    CHECK(track_change != shared.changes.end() && track_change->unresolved_non_embedded_target);
    // The refusal names the field it is about, so it can be acted on.
    CHECK(std::ranges::any_of(shared.issues, [](const auto& issue) {
        return issue.kind == MetadataWritePlanIssueKind::unresolved_non_embedded_target &&
               (issue.error.message.find("\u201ctracknumber\u201d") != std::string::npos ||
                issue.error.message.find("\u201cTRACKNUMBER\u201d") != std::string::npos);
    }));

    const auto& other = planned->sources[1];
    CHECK(other.ready());
    CHECK(other.changes.size() == 1U);
    CHECK(other.changes.front().intents.front().kind == StagedMetadataPatchKind::remove_field);
    CHECK(planned->ready_source_count() == 1U);
    CHECK(planned->blocking_issue_count() == 2U);
    CHECK(!planned->ready());
}

void reportsRevisionCapabilityReadAndAliasBlockers() {
    using trackknife::metadata::MetadataWritePlanIssueKind;
    using trackknife::metadata::StagedMetadataPatchSet;

    auto changed_selection = trackknife::metadata::StagedMetadataSelection::create({
        source("/music/changed.flac", revision(21U), {field("TITLE", {"Old"})}),
        source("/music/missing-revision.flac", std::nullopt, {field("TITLE", {"Old"})}),
        source("/music/alias.flac", revision(21U), {field("TITLE", {"Old"})}),
    });
    CHECK(changed_selection.has_value());
    if (!changed_selection) {
        return;
    }
    const auto title = *changed_selection->field_index("title");
    StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*changed_selection, 0U, title, {"New"}).has_value());
    CHECK(patches.replace_values(*changed_selection, 1U, title, {"New"}).has_value());
    CHECK(patches.replace_values(*changed_selection, 2U, title, {"New"}).has_value());

    const auto planned = trackknife::metadata::build_metadata_write_plan(
        *changed_selection, patches,
        [](const std::string& path, const trackknife::core::CancellationToken&) {
            if (path == "/music/missing-revision.flac") {
                return trackknife::core::Result<trackknife::metadata::LocalMetadataRead>{
                    std::unexpected(trackknife::core::Error{
                        .code = trackknife::core::ErrorCode::io,
                        .message = "fixture read failure",
                        .context = {},
                    })};
            }
            // changed.flac and alias.flac deliberately resolve to the same
            // observed physical identity. Neither exposes a proven writer.
            return trackknife::core::Result<trackknife::metadata::LocalMetadataRead>{
                read(path, revision(99U, 2'048U), {field("TITLE", {"Current"})}, false, false)};
        });
    CHECK(planned.has_value());
    if (!planned) {
        return;
    }
    const auto& changed = planned->sources[0];
    CHECK(has_issue(changed, MetadataWritePlanIssueKind::source_changed));
    CHECK(has_issue(changed, MetadataWritePlanIssueKind::writer_unavailable));
    CHECK(has_issue(changed, MetadataWritePlanIssueKind::preservation_unproven));
    CHECK(has_issue(changed, MetadataWritePlanIssueKind::physical_source_alias));

    const auto& missing = planned->sources[1];
    CHECK(has_issue(missing, MetadataWritePlanIssueKind::missing_baseline_revision));
    CHECK(has_issue(missing, MetadataWritePlanIssueKind::source_revalidation_failed));
    CHECK(!missing.observed_revision.has_value());

    const auto& alias = planned->sources[2];
    CHECK(has_issue(alias, MetadataWritePlanIssueKind::source_changed));
    CHECK(has_issue(alias, MetadataWritePlanIssueKind::physical_source_alias));
}

void rejectsInconsistentCapturedRevisionsForOnePath() {
    using trackknife::metadata::MetadataWritePlanIssueKind;
    using trackknife::metadata::StagedMetadataPatchSet;
    auto selection = trackknife::metadata::StagedMetadataSelection::create({
        source("/music/shared.flac", revision(31U), {field("TITLE", {"One"})}),
        source("/music/shared.flac", revision(32U), {field("TITLE", {"Two"})}),
    });
    CHECK(selection.has_value());
    if (!selection) {
        return;
    }
    const auto title = *selection->field_index("title");
    StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*selection, 0U, title, {"Shared"}).has_value());
    CHECK(patches.replace_values(*selection, 1U, title, {"Shared"}).has_value());
    const auto planned = trackknife::metadata::build_metadata_write_plan(
        *selection, patches,
        [](const std::string& path, const trackknife::core::CancellationToken&) {
            return trackknife::core::Result<trackknife::metadata::LocalMetadataRead>{
                read(path, revision(31U), {field("TITLE", {"One"})})};
        });
    CHECK(planned.has_value());
    CHECK(planned && planned->sources.size() == 1U);
    CHECK(planned && has_issue(planned->sources.front(),
                               MetadataWritePlanIssueKind::inconsistent_baseline_revision));
}

void rejectsEmptyInvalidAndCancelledPlanning() {
    const auto selection = shared_source_selection();
    trackknife::metadata::StagedMetadataPatchSet patches;
    const auto reader = [](const std::string&, const trackknife::core::CancellationToken&) {
        return trackknife::core::Result<trackknife::metadata::LocalMetadataRead>{
            std::unexpected(trackknife::core::Error{
                .code = trackknife::core::ErrorCode::invariant,
                .message = "reader should not run",
                .context = {},
            })};
    };
    const auto empty = trackknife::metadata::build_metadata_write_plan(selection, patches, reader);
    CHECK(!empty.has_value());
    CHECK(empty.error().code == trackknife::core::ErrorCode::invalid_argument);

    const auto title = *selection.field_index("title");
    CHECK(patches.replace_values(selection, 0U, title, {"Draft"}).has_value());
    const auto no_reader = trackknife::metadata::build_metadata_write_plan(
        selection, patches, trackknife::metadata::MetadataWritePlanReader{});
    CHECK(!no_reader.has_value());
    CHECK(no_reader.error().code == trackknife::core::ErrorCode::invalid_argument);

    trackknife::core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto cancelled = trackknife::metadata::build_metadata_write_plan(
        selection, patches, reader, cancellation.token());
    CHECK(!cancelled.has_value());
    CHECK(cancelled.error().code == trackknife::core::ErrorCode::cancelled);
}

void blocksUntouchedExactEmptyFlacValues() {
    using trackknife::metadata::MetadataWritePlanIssueKind;
    using trackknife::metadata::StagedMetadataPatchSet;
    auto selection = trackknife::metadata::StagedMetadataSelection::create({
        source("/music/empty-value.flac", revision(41U),
               {field("TITLE", {"Old"}), field("CUSTOM_FIELD", {""})}),
    });
    CHECK(selection.has_value());
    if (!selection) {
        return;
    }
    const auto title = selection->field_index("title");
    CHECK(title.has_value());
    if (!title) {
        return;
    }
    StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*selection, 0U, *title, {"New"}).has_value());
    const auto planned = trackknife::metadata::build_metadata_write_plan(
        *selection, patches,
        [](const std::string& path, const trackknife::core::CancellationToken&) {
            auto result =
                read(path, revision(41U), {field("TITLE", {"Old"}), field("CUSTOM_FIELD", {""})});
            result.adapter_name = "taglib-flac-v1";
            return trackknife::core::Result<trackknife::metadata::LocalMetadataRead>{
                std::move(result)};
        });
    CHECK(planned.has_value());
    CHECK(planned && !planned->ready());
    CHECK(planned && has_issue(planned->sources.front(),
                               MetadataWritePlanIssueKind::unsupported_field_mapping));
}

void logicalLoudnessRequiresItsOwnStorageTarget() {
    using namespace trackknife::metadata;
    // Even one selected logical row among duplicate occurrences must never
    // publish track gain as a whole-file tag. Existing embedded fields and
    // exact-native addresses need the same guard as newly proposed fields.
    for (const auto logical : {false, true}) {
        for (const auto existing : {false, true}) {
            for (const auto exact : {false, true}) {
                for (const std::string name :
                     {"REPLAYGAIN_TRACK_GAIN", "REPLAYGAIN_TRACK_PEAK", "REPLAYGAIN_ALBUM_GAIN",
                      "REPLAYGAIN_ALBUM_PEAK", "R128_TRACK_GAIN", "R128_ALBUM_GAIN", "TITLE",
                      "REPLAYGAIN_NOTES"}) {
                    auto input = source("/music/album.flac", revision(11U),
                                        existing ? std::vector{field(name, {"0"})}
                                                 : std::vector<MetadataField>{});
                    input.logical_track = logical;
                    auto selection = StagedMetadataSelection::create({input, input});
                    CHECK(selection.has_value());
                    if (!selection) {
                        continue;
                    }
                    const auto gain = exact ? selection->ensure_exact_native_field(name, name)
                                            : selection->ensure_missing_field(name, name);
                    CHECK(gain.has_value());
                    if (!gain) {
                        continue;
                    }
                    StagedMetadataPatchSet patches;
                    CHECK(patches.replace_values(*selection, 1U, *gain, {"-3.00 dB"}).has_value());
                    const auto plan = build_metadata_write_plan(
                        *selection, patches,
                        [existing, name](const std::string& path,
                                         const trackknife::core::CancellationToken&) {
                            return trackknife::core::Result<LocalMetadataRead>{
                                read(path, revision(11U),
                                     existing ? std::vector{field(name, {"0"})}
                                              : std::vector<MetadataField>{})};
                        });
                    CHECK(plan.has_value());
                    const auto blocked = logical && name != "TITLE" && name != "REPLAYGAIN_NOTES";
                    CHECK(plan && plan->ready() == !blocked);
                    CHECK(plan &&
                          has_issue(plan->sources.front(),
                                    MetadataWritePlanIssueKind::unresolved_non_embedded_target) ==
                              blocked);
                }
            }
        }
    }
}

// ADR-0139: ReplayGain drafts on CUE-bound logical tracks resolve to a
// per-sheet plan section instead of the unresolved-target block, without
// ever invoking the tag reader.
void routesCueReplayGainIntoSheetPlans() {
    using namespace trackknife::metadata;
    const auto root =
        std::filesystem::temp_directory_path() /
        ("trackknife-write-plan-cue-" + trackknife::core::StableId::random().to_string());
    std::error_code fs_error;
    CHECK(std::filesystem::create_directories(root, fs_error));
    const auto cue = root / "album.cue";
    {
        std::ofstream output{cue, std::ios::binary};
        output << "FILE \"disc.flac\" WAVE\n"
                  "TRACK 01 AUDIO\n"
                  "INDEX 01 00:00:00\n"
                  "TRACK 02 AUDIO\n"
                  "INDEX 01 00:01:00\n";
    }
    const auto cue_revision = trackknife::core::observe_local_source_revision(cue.native());
    CHECK(cue_revision.has_value());
    if (!cue_revision) {
        return;
    }
    const auto bound_source =
        [&](const std::size_t track_index,
            const std::optional<trackknife::core::LocalSourceRevision>& sheet_revision) {
            auto result = source("/music/disc.flac", revision(11U), {});
            result.logical_track = true;
            result.cue_sheet = StagedCueSheetBinding{
                .raw_cue_path = cue.native(),
                .cue_revision = sheet_revision,
                .file_index = 0U,
                .track_index = track_index,
            };
            return result;
        };
    int reader_calls = 0;
    const auto reader = [&reader_calls](const std::string& path,
                                        const trackknife::core::CancellationToken&) {
        ++reader_calls;
        return trackknife::core::Result<LocalMetadataRead>{read(path, revision(11U), {})};
    };

    auto selection = StagedMetadataSelection::create(
        {bound_source(0U, *cue_revision), bound_source(1U, *cue_revision)});
    CHECK(selection.has_value());
    if (!selection) {
        return;
    }
    const auto track_gain =
        selection->ensure_missing_field("REPLAYGAIN_TRACK_GAIN", "REPLAYGAIN_TRACK_GAIN");
    const auto track_peak =
        selection->ensure_missing_field("REPLAYGAIN_TRACK_PEAK", "REPLAYGAIN_TRACK_PEAK");
    const auto album_gain =
        selection->ensure_missing_field("REPLAYGAIN_ALBUM_GAIN", "REPLAYGAIN_ALBUM_GAIN");
    CHECK(track_gain && track_peak && album_gain);
    if (!track_gain || !track_peak || !album_gain) {
        return;
    }
    StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*selection, 0U, *track_gain, {"-3.46 dB"}).has_value());
    CHECK(patches.replace_values(*selection, 0U, *track_peak, {"0.994629"}).has_value());
    CHECK(patches.replace_values(*selection, 0U, *album_gain, {"-5.53 dB"}).has_value());
    CHECK(patches.replace_values(*selection, 1U, *track_gain, {"1.25 dB"}).has_value());
    CHECK(patches.replace_values(*selection, 1U, *album_gain, {"-5.53 dB"}).has_value());
    const auto plan = build_metadata_write_plan(*selection, patches, reader);
    CHECK(plan.has_value());
    CHECK(reader_calls == 0);
    if (plan) {
        CHECK(plan->ready());
        CHECK(plan->sources.empty());
        CHECK(plan->cue_sheets.size() == 1U);
        if (plan->cue_sheets.size() == 1U) {
            const auto& sheet = plan->cue_sheets.front();
            CHECK(sheet.raw_cue_path == cue.native());
            CHECK(sheet.expected_revision == *cue_revision);
            CHECK(sheet.observed_revision == *cue_revision);
            CHECK(sheet.album_fields.size() == 1U);
            CHECK(sheet.tracks.size() == 2U);
            CHECK(sheet.tracks.size() == 2U && sheet.tracks[0].track_index == 0U &&
                  sheet.tracks[0].fields.size() == 2U && sheet.tracks[1].track_index == 1U &&
                  sheet.tracks[1].fields.size() == 1U);
            CHECK(sheet.tracks.size() == 2U &&
                  sheet.tracks[0].occurrence_indexes == std::vector<std::size_t>{0U} &&
                  sheet.tracks[1].occurrence_indexes == std::vector<std::size_t>{1U});
        }
    }

    // Disagreeing album values across the sheet block the plan.
    StagedMetadataPatchSet conflicting;
    CHECK(conflicting.replace_values(*selection, 0U, *album_gain, {"-5.53 dB"}).has_value());
    CHECK(conflicting.replace_values(*selection, 1U, *album_gain, {"-9.99 dB"}).has_value());
    const auto conflicted = build_metadata_write_plan(*selection, conflicting, reader);
    CHECK(conflicted && !conflicted->ready());
    CHECK(conflicted && has_cue_issue(conflicted->cue_sheets.front(),
                                      MetadataWritePlanIssueKind::conflicting_logical_edits));

    // Unparseable staged text blocks before any write is planned.
    StagedMetadataPatchSet invalid;
    CHECK(invalid.replace_values(*selection, 0U, *track_gain, {"loud"}).has_value());
    const auto unparseable = build_metadata_write_plan(*selection, invalid, reader);
    CHECK(unparseable && !unparseable->ready());
    CHECK(unparseable && has_cue_issue(unparseable->cue_sheets.front(),
                                       MetadataWritePlanIssueKind::unsupported_field_mapping));

    // A sheet that changed since capture blocks with source_changed.
    auto stale_revision = *cue_revision;
    ++stale_revision.size;
    auto stale_selection = StagedMetadataSelection::create({bound_source(0U, stale_revision)});
    CHECK(stale_selection.has_value());
    if (stale_selection) {
        const auto stale_gain =
            stale_selection->ensure_missing_field("REPLAYGAIN_TRACK_GAIN", "REPLAYGAIN_TRACK_GAIN");
        CHECK(stale_gain.has_value());
        StagedMetadataPatchSet stale_patches;
        CHECK(stale_patches.replace_values(*stale_selection, 0U, *stale_gain, {"-1.00 dB"})
                  .has_value());
        const auto stale_plan = build_metadata_write_plan(*stale_selection, stale_patches, reader);
        CHECK(stale_plan && !stale_plan->ready());
        CHECK(stale_plan && has_cue_issue(stale_plan->cue_sheets.front(),
                                          MetadataWritePlanIssueKind::source_changed));
    }

    // A missing captured sheet revision blocks like a missing baseline.
    auto unbound_selection = StagedMetadataSelection::create({bound_source(0U, std::nullopt)});
    CHECK(unbound_selection.has_value());
    if (unbound_selection) {
        const auto unbound_gain = unbound_selection->ensure_missing_field("REPLAYGAIN_TRACK_GAIN",
                                                                          "REPLAYGAIN_TRACK_GAIN");
        CHECK(unbound_gain.has_value());
        StagedMetadataPatchSet unbound_patches;
        CHECK(unbound_patches.replace_values(*unbound_selection, 0U, *unbound_gain, {"-1.00 dB"})
                  .has_value());
        const auto unbound_plan =
            build_metadata_write_plan(*unbound_selection, unbound_patches, reader);
        CHECK(unbound_plan && !unbound_plan->ready());
        CHECK(unbound_plan && has_cue_issue(unbound_plan->cue_sheets.front(),
                                            MetadataWritePlanIssueKind::missing_baseline_revision));
    }

    std::filesystem::remove_all(root, fs_error);
}

// ADR-0141: ReplayGain drafts on non-CUE logical tracks resolve to a
// per-file loudness-sidecar plan section keyed by logical identity.
void routesNonCueLogicalLoudnessIntoSidecarPlans() {
    using namespace trackknife::metadata;
    const auto root =
        std::filesystem::temp_directory_path() /
        ("trackknife-write-plan-sidecar-" + trackknife::core::StableId::random().to_string());
    std::error_code fs_error;
    CHECK(std::filesystem::create_directories(root, fs_error));
    const auto audio = root / "book.m4b";
    {
        std::ofstream output{audio, std::ios::binary};
        output << "audio-bytes";
    }
    const auto audio_revision = trackknife::core::observe_local_source_revision(audio.native());
    CHECK(audio_revision.has_value());
    if (!audio_revision) {
        return;
    }
    const auto chapter_source = [&](const std::int64_t start, const std::int64_t end) {
        auto result = source(audio.native(), *audio_revision, {});
        result.logical_track = true;
        result.logical_identity = StagedLogicalIdentity{
            .stream_index = std::nullopt,
            .subsong_index = std::nullopt,
            .start_sample = start,
            .end_sample = end,
        };
        return result;
    };
    int reader_calls = 0;
    const auto reader = [&reader_calls](const std::string& path,
                                        const trackknife::core::CancellationToken&) {
        ++reader_calls;
        return trackknife::core::Result<LocalMetadataRead>{read(path, revision(11U), {})};
    };
    auto selection = StagedMetadataSelection::create(
        {chapter_source(0, 44'100), chapter_source(44'100, 88'200)});
    CHECK(selection.has_value());
    if (!selection) {
        return;
    }
    const auto track_gain =
        selection->ensure_missing_field("REPLAYGAIN_TRACK_GAIN", "REPLAYGAIN_TRACK_GAIN");
    const auto album_gain =
        selection->ensure_missing_field("REPLAYGAIN_ALBUM_GAIN", "REPLAYGAIN_ALBUM_GAIN");
    CHECK(track_gain && album_gain);
    if (!track_gain || !album_gain) {
        return;
    }
    StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*selection, 0U, *track_gain, {"-3.46 dB"}).has_value());
    CHECK(patches.replace_values(*selection, 0U, *album_gain, {"-5.53 dB"}).has_value());
    CHECK(patches.replace_values(*selection, 1U, *track_gain, {"1.25 dB"}).has_value());
    const auto plan = build_metadata_write_plan(*selection, patches, reader);
    CHECK(plan.has_value());
    CHECK(reader_calls == 0);
    if (plan) {
        CHECK(plan->ready());
        CHECK(plan->sources.empty());
        CHECK(plan->cue_sheets.empty());
        CHECK(plan->sidecars.size() == 1U);
        if (plan->sidecars.size() == 1U) {
            const auto& sidecar = plan->sidecars.front();
            CHECK(sidecar.raw_audio_path == audio.native());
            CHECK(sidecar.expected_revision == *audio_revision);
            CHECK(sidecar.observed_revision == *audio_revision);
            CHECK(sidecar.entries.size() == 2U);
            CHECK(sidecar.entries.size() == 2U && sidecar.entries[0].fields.size() == 2U &&
                  sidecar.entries[1].fields.size() == 1U);
            CHECK(sidecar.entries.size() == 2U &&
                  sidecar.entries[0].occurrence_indexes == std::vector<std::size_t>{0U} &&
                  sidecar.entries[1].occurrence_indexes == std::vector<std::size_t>{1U});
        }
    }

    // A changed audio file blocks the sidecar plan like every write.
    {
        std::ofstream output{audio, std::ios::binary | std::ios::app};
        output << "!";
    }
    const auto stale_plan = build_metadata_write_plan(*selection, patches, reader);
    CHECK(stale_plan && !stale_plan->ready());
    CHECK(stale_plan && stale_plan->sidecars.size() == 1U &&
          std::ranges::any_of(stale_plan->sidecars.front().issues, [](const auto& issue) {
              return issue.kind == MetadataWritePlanIssueKind::source_changed;
          }));

    std::filesystem::remove_all(root, fs_error);
}

// ADR-0143: clean conventional ReplayGain on an adapter without a safe
// tag writer diverts to the whole-file sidecar entry; everything else
// keeps the visible writer block, and writable formats are untouched.
void divertsUnwritableWholeFileLoudnessToSidecars() {
    using namespace trackknife::metadata;
    // The sidecar section observes the real path, so the source must
    // exist on disk with the staged revision.
    const auto root =
        std::filesystem::temp_directory_path() /
        ("trackknife-write-plan-policy-" + trackknife::core::StableId::random().to_string());
    std::error_code fs_error;
    CHECK(std::filesystem::create_directories(root, fs_error));
    const auto wav = (root / "take.wav").native();
    {
        std::ofstream output{wav, std::ios::binary};
        output << "audio-bytes";
    }
    const auto real_revision = trackknife::core::observe_local_source_revision(wav);
    CHECK(real_revision.has_value());
    if (!real_revision) {
        return;
    }
    const auto unwritable_reader = [&real_revision](const std::string& path,
                                                    const trackknife::core::CancellationToken&) {
        return trackknife::core::Result<LocalMetadataRead>{
            read(path, *real_revision, {}, false, false)};
    };
    auto selection = StagedMetadataSelection::create(
        {source(wav, *real_revision, {}), source(wav, *real_revision, {})});
    CHECK(selection.has_value());
    if (!selection) {
        return;
    }
    const auto track_gain =
        selection->ensure_missing_field("REPLAYGAIN_TRACK_GAIN", "REPLAYGAIN_TRACK_GAIN");
    const auto track_peak =
        selection->ensure_missing_field("REPLAYGAIN_TRACK_PEAK", "REPLAYGAIN_TRACK_PEAK");
    const auto title = selection->ensure_missing_field("TITLE", "TITLE");
    CHECK(track_gain && track_peak && title);
    if (!track_gain || !track_peak || !title) {
        return;
    }

    StagedMetadataPatchSet loudness_only;
    CHECK(loudness_only.replace_values(*selection, 0U, *track_gain, {"-6.02 dB"}).has_value());
    CHECK(loudness_only.replace_values(*selection, 0U, *track_peak, {"1.000000"}).has_value());
    const auto diverted = build_metadata_write_plan(*selection, loudness_only, unwritable_reader);
    CHECK(diverted.has_value());
    if (diverted) {
        CHECK(diverted->ready());
        CHECK(diverted->sources.empty());
        CHECK(diverted->sidecars.size() == 1U);
        if (diverted->sidecars.size() == 1U) {
            const auto& sidecar = diverted->sidecars.front();
            CHECK(sidecar.raw_audio_path == wav);
            CHECK(sidecar.expected_revision == *real_revision);
            CHECK(sidecar.observed_revision == *real_revision);
            CHECK(sidecar.entries.size() == 1U);
            const std::vector<std::size_t> both_occurrences{0U, 1U};
            CHECK(sidecar.entries.size() == 1U &&
                  sidecar.entries.front().identity == StagedLogicalIdentity{} &&
                  sidecar.entries.front().fields.size() == 2U &&
                  sidecar.entries.front().occurrence_indexes == both_occurrences);
        }
    }

    // A mixed draft still blocks visibly: the title keeps the writer
    // block while the loudness half waits in the sidecar section.
    StagedMetadataPatchSet mixed;
    CHECK(mixed.replace_values(*selection, 0U, *track_gain, {"-6.02 dB"}).has_value());
    CHECK(mixed.replace_values(*selection, 0U, *title, {"New title"}).has_value());
    const auto blocked = build_metadata_write_plan(*selection, mixed, unwritable_reader);
    CHECK(blocked.has_value());
    if (blocked) {
        CHECK(!blocked->ready());
        CHECK(blocked->sources.size() == 1U);
        CHECK(blocked->sources.size() == 1U &&
              has_issue(blocked->sources.front(), MetadataWritePlanIssueKind::writer_unavailable));
        CHECK(blocked->sidecars.size() == 1U);
    }

    // Writable adapters keep ReplayGain in ordinary tags.
    const auto writable_reader = [&real_revision](const std::string& path,
                                                  const trackknife::core::CancellationToken&) {
        auto result = read(path, *real_revision, {});
        result.adapter_name = "taglib-flac-v1";
        return trackknife::core::Result<LocalMetadataRead>{std::move(result)};
    };
    const auto tagged = build_metadata_write_plan(*selection, loudness_only, writable_reader);
    CHECK(tagged.has_value());
    CHECK(tagged && tagged->ready() && tagged->sources.size() == 1U && tagged->sidecars.empty());

    // ADR-0146: the sidecar-only policy diverts loudness even on
    // writable formats, as a whole-file entry.
    const auto policy = build_metadata_write_plan(*selection, loudness_only, writable_reader, {},
                                                  {.sidecar_loudness = true});
    CHECK(policy.has_value());
    CHECK(policy && policy->ready() && policy->sources.empty() && policy->sidecars.size() == 1U);
    CHECK(policy && policy->sidecars.size() == 1U &&
          policy->sidecars.front().entries.size() == 1U &&
          policy->sidecars.front().entries.front().identity == StagedLogicalIdentity{});
    CHECK(policy && policy->sidecars.size() == 1U &&
          !policy->sidecars.front().entries.front().true_peak);

    // ADR-0148: the true-peak policy stamps every planned sidecar entry.
    const auto true_peak_policy =
        build_metadata_write_plan(*selection, loudness_only, writable_reader, {},
                                  {.sidecar_loudness = true, .true_peak_loudness = true});
    CHECK(true_peak_policy.has_value());
    CHECK(true_peak_policy && true_peak_policy->sidecars.size() == 1U &&
          true_peak_policy->sidecars.front().entries.size() == 1U &&
          true_peak_policy->sidecars.front().entries.front().true_peak);
    std::filesystem::remove_all(root, fs_error);
}

} // namespace

int main() {
    groupsEveryIntentAndReportsLogicalConflicts();
    reportsRevisionCapabilityReadAndAliasBlockers();
    rejectsInconsistentCapturedRevisionsForOnePath();
    rejectsEmptyInvalidAndCancelledPlanning();
    blocksUntouchedExactEmptyFlacValues();
    logicalLoudnessRequiresItsOwnStorageTarget();
    routesCueReplayGainIntoSheetPlans();
    routesNonCueLogicalLoudnessIntoSidecarPlans();
    divertsUnwritableWholeFileLoudnessToSidecars();
    return failures == 0 ? 0 : 1;
}
