// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/write_plan.hpp"
#include "trackknife/metadata/artwork_write_plan.hpp"

#include "trackknife/metadata/flac_mapping.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <map>
#include <ranges>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace trackknife::metadata {
namespace {

[[nodiscard]] core::Error planner_error(const core::ErrorCode code, std::string message,
                                        const std::string& raw_path = {}) {
    core::Error result{.code = code, .message = std::move(message), .context = {}};
    if (!raw_path.empty()) {
        result.context.push_back({.key = "path", .value = core::escape_raw_path(raw_path)});
    }
    return result;
}

void add_issue(MetadataWritePlanSource& source, const MetadataWritePlanIssueKind kind,
               core::Error error, const std::optional<std::size_t> field_index = std::nullopt,
               std::vector<std::size_t> item_indexes = {}) {
    source.issues.push_back(MetadataWritePlanIssue{
        .kind = kind,
        .error = std::move(error),
        .field_index = field_index,
        .item_indexes = std::move(item_indexes),
        .blocking = true,
    });
}

[[nodiscard]] bool same_intent(const MetadataWritePlanIntent& left,
                               const MetadataWritePlanIntent& right) {
    return left.kind == right.kind && left.values == right.values;
}

[[nodiscard]] bool is_non_embedded(const StagedMetadataSelection& selection,
                                   const MetadataWritePlanIntent& intent,
                                   const std::size_t field_index) {
    const auto& field = selection.field(field_index);
    if (selection.source(intent.item_index).logical_track &&
        (field.canonical_name == "replaygaintrackgain" ||
         field.canonical_name == "replaygaintrackpeak" ||
         field.canonical_name == "replaygainalbumgain" ||
         field.canonical_name == "replaygainalbumpeak" || field.canonical_name == "r128trackgain" ||
         field.canonical_name == "r128albumgain")) {
        return true;
    }
    const auto* cell = selection.cell(intent.item_index, field_index);
    return cell != nullptr && cell->provenance != FieldProvenance::embedded;
}

// Why a field cannot be written into the file, in the words the person
// applying the edit can act on: which field, and where its value came from.
[[nodiscard]] std::string non_embedded_reason(const StagedMetadataSelection& selection,
                                              const MetadataWritePlanIntent& intent,
                                              const std::size_t field_index,
                                              const std::string& name) {
    const auto quoted = "\u201c" + name + "\u201d ";
    const auto& field = selection.field(field_index);
    if (selection.source(intent.item_index).logical_track &&
        (field.canonical_name.starts_with("replaygain") || field.canonical_name.starts_with("r128"))) {
        return quoted + "belongs to one track inside a larger file, which has nowhere to keep it yet";
    }
    const auto* cell = selection.cell(intent.item_index, field_index);
    switch (cell != nullptr ? cell->provenance : FieldProvenance::embedded) {
    case FieldProvenance::stream:
        return quoted + "is not a tag in this file: it was read from the audio stream, so there "
                        "is nothing here to change";
    case FieldProvenance::cached_snapshot:
        return quoted + "was taken from a stored copy rather than read from the file; reload the "
                        "file and apply again";
    case FieldProvenance::sidecar:
        return quoted + "comes from a file beside this one, not from its tags";
    case FieldProvenance::segment:
        return quoted + "belongs to a chapter or part of the file, which cannot hold tags";
    case FieldProvenance::annotation:
        return quoted + "is Trackknife's own note, not a tag in the file";
    case FieldProvenance::embedded:
        break;
    }
    return quoted + "cannot be written into this file";
}

[[nodiscard]] core::Result<MetadataWritePlan> cancelled() {
    return std::unexpected(core::Error{
        .code = core::ErrorCode::cancelled,
        .message = "metadata write-plan revalidation was cancelled",
        .context = {},
    });
}

// ADR-0139: ReplayGain fields on a CUE-bound logical track resolve to a
// sheet rewrite. R128 fields have no CUE convention and stay blocked.
[[nodiscard]] bool is_cue_replay_gain_field(const std::string& canonical_name) {
    return canonical_name == "replaygaintrackgain" || canonical_name == "replaygaintrackpeak" ||
           canonical_name == "replaygainalbumgain" || canonical_name == "replaygainalbumpeak";
}

[[nodiscard]] bool is_cue_album_field(const std::string& canonical_name) {
    return canonical_name == "replaygainalbumgain" || canonical_name == "replaygainalbumpeak";
}

// Staged text must already be a number the sheet committer can
// canonicalize: optional +, optional dB suffix on gains, sane ranges.
[[nodiscard]] bool valid_cue_replay_gain_value(const std::string& canonical_name,
                                               std::string_view text) {
    const bool gain =
        canonical_name == "replaygaintrackgain" || canonical_name == "replaygainalbumgain";
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1U);
    }
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1U);
    }
    double value = 0.0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || !std::isfinite(value)) {
        return false;
    }
    auto suffix = text.substr(static_cast<std::size_t>(parsed.ptr - text.data()));
    while (!suffix.empty() && (suffix.front() == ' ' || suffix.front() == '\t')) {
        suffix.remove_prefix(1U);
    }
    if (gain &&
        (suffix.starts_with("dB") || suffix.starts_with("db") || suffix.starts_with("DB"))) {
        suffix.remove_prefix(2U);
    }
    while (!suffix.empty() && (suffix.front() == ' ' || suffix.front() == '\t')) {
        suffix.remove_prefix(1U);
    }
    return suffix.empty() && (gain ? std::abs(value) <= 60.0 : value >= 0.0);
}

void add_cue_issue(MetadataWritePlanCueSheet& sheet, const MetadataWritePlanIssueKind kind,
                   core::Error error, const std::optional<std::size_t> field_index = std::nullopt,
                   std::vector<std::size_t> item_indexes = {}) {
    sheet.issues.push_back(MetadataWritePlanIssue{
        .kind = kind,
        .error = std::move(error),
        .field_index = field_index,
        .item_indexes = std::move(item_indexes),
        .blocking = true,
    });
}

void add_sidecar_issue(MetadataWritePlanSidecar& sidecar, const MetadataWritePlanIssueKind kind,
                       core::Error error,
                       const std::optional<std::size_t> field_index = std::nullopt,
                       std::vector<std::size_t> item_indexes = {}) {
    sidecar.issues.push_back(MetadataWritePlanIssue{
        .kind = kind,
        .error = std::move(error),
        .field_index = field_index,
        .item_indexes = std::move(item_indexes),
        .blocking = true,
    });
}

struct CueIntentRecord {
    std::size_t field_index{0U};
    std::string canonical_name;
    std::size_t item_index{0U};
    StagedMetadataPatchKind kind{StagedMetadataPatchKind::replace_values};
    std::vector<std::string> values;
    std::size_t file_index{0U};
    std::size_t track_index{0U};
    std::optional<core::LocalSourceRevision> cue_revision;
};

struct SidecarIntentRecord {
    std::size_t field_index{0U};
    std::string canonical_name;
    std::size_t item_index{0U};
    StagedMetadataPatchKind kind{StagedMetadataPatchKind::replace_values};
    std::vector<std::string> values;
    StagedLogicalIdentity identity;
    std::optional<core::LocalSourceRevision> source_revision;
};

// Ordered map key for one logical identity inside a physical file.
[[nodiscard]] std::tuple<int, int, std::int64_t, std::int64_t>
identity_key(const StagedLogicalIdentity& identity) {
    return {identity.stream_index.value_or(-1), identity.subsong_index.value_or(-1),
            identity.start_sample.value_or(-1), identity.end_sample.value_or(-1)};
}

} // namespace

std::string_view metadata_write_plan_issue_kind_name(const MetadataWritePlanIssueKind kind) {
    switch (kind) {
    case MetadataWritePlanIssueKind::missing_baseline_revision:
        return "missing baseline revision";
    case MetadataWritePlanIssueKind::inconsistent_baseline_revision:
        return "inconsistent baseline revision";
    case MetadataWritePlanIssueKind::source_revalidation_failed:
        return "source revalidation failed";
    case MetadataWritePlanIssueKind::source_changed:
        return "source changed";
    case MetadataWritePlanIssueKind::physical_source_alias:
        return "physical source alias";
    case MetadataWritePlanIssueKind::conflicting_logical_edits:
        return "conflicting logical edits";
    case MetadataWritePlanIssueKind::unresolved_non_embedded_target:
        return "not a tag in the file";
    case MetadataWritePlanIssueKind::writer_unavailable:
        return "writer unavailable";
    case MetadataWritePlanIssueKind::preservation_unproven:
        return "preservation unproven";
    case MetadataWritePlanIssueKind::unsupported_field_mapping:
        return "unsupported field mapping";
    }
    return "source revalidation failed";
}

bool MetadataWritePlanSource::ready() const noexcept {
    if (artwork && !artwork->ready()) {
        return false;
    }
    return std::ranges::none_of(issues, [](const auto& issue) { return issue.blocking; });
}

std::size_t MetadataWritePlanSource::blocking_issue_count() const noexcept {
    return static_cast<std::size_t>(
        std::ranges::count_if(issues, [](const auto& issue) { return issue.blocking; }));
}

bool MetadataWritePlanCueSheet::ready() const noexcept {
    return std::ranges::none_of(issues, [](const auto& issue) { return issue.blocking; });
}

std::size_t MetadataWritePlanCueSheet::blocking_issue_count() const noexcept {
    return static_cast<std::size_t>(
        std::ranges::count_if(issues, [](const auto& issue) { return issue.blocking; }));
}

bool MetadataWritePlanSidecar::ready() const noexcept {
    return std::ranges::none_of(issues, [](const auto& issue) { return issue.blocking; });
}

std::size_t MetadataWritePlanSidecar::blocking_issue_count() const noexcept {
    return static_cast<std::size_t>(
        std::ranges::count_if(issues, [](const auto& issue) { return issue.blocking; }));
}

bool MetadataWritePlan::ready() const noexcept {
    return (!sources.empty() || !cue_sheets.empty() || !sidecars.empty()) &&
           std::ranges::all_of(sources, &MetadataWritePlanSource::ready) &&
           std::ranges::all_of(cue_sheets, &MetadataWritePlanCueSheet::ready) &&
           std::ranges::all_of(sidecars, &MetadataWritePlanSidecar::ready);
}

std::size_t MetadataWritePlan::ready_source_count() const noexcept {
    return static_cast<std::size_t>(
        std::ranges::count_if(sources, &MetadataWritePlanSource::ready));
}

std::size_t MetadataWritePlan::blocking_issue_count() const noexcept {
    std::size_t count = 0U;
    for (const auto& source : sources) {
        count += source.blocking_issue_count();
    }
    for (const auto& sheet : cue_sheets) {
        count += sheet.blocking_issue_count();
    }
    for (const auto& sidecar : sidecars) {
        count += sidecar.blocking_issue_count();
    }
    return count;
}

core::Result<MetadataWritePlan> build_metadata_write_plan(
    const StagedMetadataSelection& selection, const StagedMetadataPatchSet& patches,
    const MetadataWritePlanReader& reader, const core::CancellationToken& cancellation,
    const MetadataWritePlanOptions& options) {
    if (cancellation.is_cancellation_requested()) {
        return cancelled();
    }
    if (!reader) {
        return std::unexpected(planner_error(core::ErrorCode::invalid_argument,
                                             "metadata write-plan reader is not configured"));
    }
    if (patches.empty()) {
        return std::unexpected(planner_error(core::ErrorCode::invalid_argument,
                                             "metadata write plan requires staged changes"));
    }

    MetadataWritePlan plan{.sources = {}, .patch_count = patches.patch_count(), .cue_sheets = {}};
    std::unordered_map<std::string, std::size_t> source_positions;
    source_positions.reserve(std::min(patches.patch_count(), selection.item_count()));
    std::vector<std::unordered_map<std::size_t, std::size_t>> change_positions;
    std::map<std::string, std::vector<CueIntentRecord>> cue_buckets;
    std::map<std::string, std::vector<SidecarIntentRecord>> sidecar_buckets;

    const auto staged_patches = patches.patches();
    for (const auto& patch : staged_patches) {
        if (cancellation.is_cancellation_requested()) {
            return cancelled();
        }
        if (patch.item_index >= selection.item_count() ||
            patch.field_index >= selection.field_count()) {
            return std::unexpected(
                planner_error(core::ErrorCode::invariant,
                              "metadata write plan contains a patch outside its staged selection"));
        }
        const auto& staged_source = selection.source(patch.item_index);
        if (staged_source.cue_sheet && staged_source.logical_track &&
            is_cue_replay_gain_field(selection.field(patch.field_index).canonical_name)) {
            cue_buckets[staged_source.cue_sheet->raw_cue_path].push_back(CueIntentRecord{
                .field_index = patch.field_index,
                .canonical_name = selection.field(patch.field_index).canonical_name,
                .item_index = patch.item_index,
                .kind = patch.kind,
                .values = patch.values,
                .file_index = staged_source.cue_sheet->file_index,
                .track_index = staged_source.cue_sheet->track_index,
                .cue_revision = staged_source.cue_sheet->cue_revision,
            });
            continue;
        }
        // ADR-0141: the same fields on a non-CUE logical track resolve to
        // its loudness sidecar, keyed by the captured logical identity.
        // ADR-0146: the sidecar-only policy widens the ADR-0141 routing to
        // every non-CUE source; whole-file drafts use the empty identity.
        const auto sidecar_target =
            !staged_source.cue_sheet &&
            is_cue_replay_gain_field(selection.field(patch.field_index).canonical_name) &&
            (options.sidecar_loudness ||
             (staged_source.logical_track && staged_source.logical_identity));
        if (sidecar_target) {
            sidecar_buckets[staged_source.raw_path].push_back(SidecarIntentRecord{
                .field_index = patch.field_index,
                .canonical_name = selection.field(patch.field_index).canonical_name,
                .item_index = patch.item_index,
                .kind = patch.kind,
                .values = patch.values,
                .identity = staged_source.logical_identity.value_or(StagedLogicalIdentity{}),
                .source_revision = staged_source.source_revision,
            });
            continue;
        }
        auto [source_position, inserted] =
            source_positions.emplace(staged_source.raw_path, plan.sources.size());
        if (inserted) {
            plan.sources.push_back(MetadataWritePlanSource{
                .raw_path = staged_source.raw_path,
                .occurrence_indexes = {},
                .expected_revision = std::nullopt,
                .observed_revision = std::nullopt,
                .adapter_name = {},
                .changes = {},
                .issues = {},
            });
            change_positions.emplace_back();
        }
        auto& source = plan.sources[source_position->second];
        auto& positions = change_positions[source_position->second];
        auto [change_position, change_inserted] =
            positions.emplace(patch.field_index, source.changes.size());
        if (change_inserted) {
            const auto& field = selection.field(patch.field_index);
            source.changes.push_back(MetadataWritePlanChange{
                .field_index = patch.field_index,
                .canonical_name = field.canonical_name,
                .display_name = field.display_name,
                .native_name = {},
                .original_present = false,
                .original_values = {},
                .intents = {},
                .conflicting_intents = false,
                .unresolved_non_embedded_target = false,
                .exact_native_name = field.exact_native_name,
            });
        }
        source.changes[change_position->second].intents.push_back(MetadataWritePlanIntent{
            .item_index = patch.item_index,
            .kind = patch.kind,
            .values = patch.values,
        });
    }

    for (std::size_t item_index = 0U; item_index < selection.item_count(); ++item_index) {
        const auto found = source_positions.find(selection.source(item_index).raw_path);
        if (found != source_positions.end()) {
            plan.sources[found->second].occurrence_indexes.push_back(item_index);
        }
    }

    for (auto& [raw_cue_path, records] : cue_buckets) {
        MetadataWritePlanCueSheet sheet{
            .raw_cue_path = raw_cue_path,
            .expected_revision = std::nullopt,
            .observed_revision = std::nullopt,
            .tracks = {},
            .album_fields = {},
            .issues = {},
        };

        std::optional<core::LocalSourceRevision> expected;
        std::vector<std::size_t> missing_revision_items;
        std::vector<std::size_t> inconsistent_revision_items;
        for (const auto& record : records) {
            if (!record.cue_revision) {
                missing_revision_items.push_back(record.item_index);
            } else if (!expected) {
                expected = record.cue_revision;
            } else if (*expected != *record.cue_revision) {
                inconsistent_revision_items.push_back(record.item_index);
            }
        }
        sheet.expected_revision = expected;
        if (!missing_revision_items.empty()) {
            add_cue_issue(sheet, MetadataWritePlanIssueKind::missing_baseline_revision,
                          planner_error(core::ErrorCode::conflict,
                                        "a staged CUE occurrence has no captured sheet revision",
                                        raw_cue_path),
                          std::nullopt, std::move(missing_revision_items));
        }
        if (!inconsistent_revision_items.empty()) {
            add_cue_issue(sheet, MetadataWritePlanIssueKind::inconsistent_baseline_revision,
                          planner_error(core::ErrorCode::conflict,
                                        "staged CUE occurrences disagree about the sheet revision",
                                        raw_cue_path),
                          std::nullopt, std::move(inconsistent_revision_items));
        }

        const auto finalize_field =
            [&sheet, &raw_cue_path](const std::vector<const CueIntentRecord*>& intents)
            -> MetadataWritePlanLoudnessField {
            MetadataWritePlanLoudnessField field{
                .field_index = intents.front()->field_index,
                .canonical_name = intents.front()->canonical_name,
                .kind = intents.front()->kind,
                .values = intents.front()->values,
                .item_indexes = {},
            };
            bool conflicting = false;
            for (const auto* intent : intents) {
                field.item_indexes.push_back(intent->item_index);
                conflicting =
                    conflicting || intent->kind != field.kind || intent->values != field.values;
            }
            if (conflicting) {
                add_cue_issue(
                    sheet, MetadataWritePlanIssueKind::conflicting_logical_edits,
                    planner_error(core::ErrorCode::conflict,
                                  "staged occurrences disagree about one CUE ReplayGain value",
                                  raw_cue_path),
                    field.field_index, field.item_indexes);
            } else if (field.kind == StagedMetadataPatchKind::replace_values &&
                       (field.values.size() != 1U ||
                        !valid_cue_replay_gain_value(field.canonical_name, field.values.front()))) {
                add_cue_issue(sheet, MetadataWritePlanIssueKind::unsupported_field_mapping,
                              planner_error(core::ErrorCode::unsupported,
                                            "a CUE ReplayGain value must be a single number the "
                                            "sheet writer can canonicalize",
                                            raw_cue_path),
                              field.field_index, field.item_indexes);
            }
            return field;
        };

        std::map<std::string, std::vector<const CueIntentRecord*>> album_groups;
        std::map<std::tuple<std::size_t, std::size_t, std::string>,
                 std::vector<const CueIntentRecord*>>
            track_groups;
        for (const auto& record : records) {
            if (is_cue_album_field(record.canonical_name)) {
                album_groups[record.canonical_name].push_back(&record);
            } else {
                track_groups[{record.file_index, record.track_index, record.canonical_name}]
                    .push_back(&record);
            }
        }
        for (const auto& [name, intents] : album_groups) {
            sheet.album_fields.push_back(finalize_field(intents));
        }
        std::map<std::pair<std::size_t, std::size_t>, std::size_t> track_positions;
        for (const auto& [key, intents] : track_groups) {
            const auto track_key = std::pair{std::get<0>(key), std::get<1>(key)};
            auto [position, inserted] = track_positions.emplace(track_key, sheet.tracks.size());
            if (inserted) {
                sheet.tracks.push_back(MetadataWritePlanCueTrack{
                    .file_index = track_key.first,
                    .track_index = track_key.second,
                    .occurrence_indexes = {},
                    .fields = {},
                });
            }
            sheet.tracks[position->second].fields.push_back(finalize_field(intents));
        }
        for (auto& track : sheet.tracks) {
            for (std::size_t item_index = 0U; item_index < selection.item_count(); ++item_index) {
                const auto& binding = selection.source(item_index).cue_sheet;
                if (binding && binding->raw_cue_path == raw_cue_path &&
                    binding->file_index == track.file_index &&
                    binding->track_index == track.track_index) {
                    track.occurrence_indexes.push_back(item_index);
                }
            }
        }

        auto observed = core::observe_local_source_revision(raw_cue_path);
        if (!observed) {
            add_cue_issue(sheet, MetadataWritePlanIssueKind::source_revalidation_failed,
                          std::move(observed.error()));
        } else {
            sheet.observed_revision = *observed;
            if (sheet.expected_revision && *sheet.expected_revision != *observed) {
                add_cue_issue(
                    sheet, MetadataWritePlanIssueKind::source_changed,
                    planner_error(core::ErrorCode::conflict,
                                  "the CUE sheet changed after the ReplayGain draft was captured",
                                  raw_cue_path));
            }
        }
        plan.cue_sheets.push_back(std::move(sheet));
    }

    for (auto& [raw_audio_path, records] : sidecar_buckets) {
        MetadataWritePlanSidecar sidecar{
            .raw_audio_path = raw_audio_path,
            .expected_revision = std::nullopt,
            .observed_revision = std::nullopt,
            .entries = {},
            .issues = {},
        };

        std::optional<core::LocalSourceRevision> expected;
        std::vector<std::size_t> missing_revision_items;
        std::vector<std::size_t> inconsistent_revision_items;
        for (const auto& record : records) {
            if (!record.source_revision) {
                missing_revision_items.push_back(record.item_index);
            } else if (!expected) {
                expected = record.source_revision;
            } else if (*expected != *record.source_revision) {
                inconsistent_revision_items.push_back(record.item_index);
            }
        }
        sidecar.expected_revision = expected;
        if (!missing_revision_items.empty()) {
            add_sidecar_issue(sidecar, MetadataWritePlanIssueKind::missing_baseline_revision,
                              planner_error(core::ErrorCode::conflict,
                                            "a staged occurrence has no captured source revision",
                                            raw_audio_path),
                              std::nullopt, std::move(missing_revision_items));
        }
        if (!inconsistent_revision_items.empty()) {
            add_sidecar_issue(sidecar, MetadataWritePlanIssueKind::inconsistent_baseline_revision,
                              planner_error(core::ErrorCode::conflict,
                                            "staged occurrences disagree about the source revision",
                                            raw_audio_path),
                              std::nullopt, std::move(inconsistent_revision_items));
        }

        const auto finalize_field =
            [&sidecar, &raw_audio_path](const std::vector<const SidecarIntentRecord*>& intents)
            -> MetadataWritePlanLoudnessField {
            MetadataWritePlanLoudnessField field{
                .field_index = intents.front()->field_index,
                .canonical_name = intents.front()->canonical_name,
                .kind = intents.front()->kind,
                .values = intents.front()->values,
                .item_indexes = {},
            };
            bool conflicting = false;
            for (const auto* intent : intents) {
                field.item_indexes.push_back(intent->item_index);
                conflicting =
                    conflicting || intent->kind != field.kind || intent->values != field.values;
            }
            if (conflicting) {
                add_sidecar_issue(
                    sidecar, MetadataWritePlanIssueKind::conflicting_logical_edits,
                    planner_error(core::ErrorCode::conflict,
                                  "staged occurrences disagree about one sidecar loudness value",
                                  raw_audio_path),
                    field.field_index, field.item_indexes);
            } else if (field.kind == StagedMetadataPatchKind::replace_values &&
                       (field.values.size() != 1U ||
                        !valid_cue_replay_gain_value(field.canonical_name, field.values.front()))) {
                add_sidecar_issue(sidecar, MetadataWritePlanIssueKind::unsupported_field_mapping,
                                  planner_error(core::ErrorCode::unsupported,
                                                "a sidecar loudness value must be a single "
                                                "number the writer can canonicalize",
                                                raw_audio_path),
                                  field.field_index, field.item_indexes);
            }
            return field;
        };

        std::map<std::tuple<int, int, std::int64_t, std::int64_t>,
                 std::map<std::string, std::vector<const SidecarIntentRecord*>>>
            identity_groups;
        for (const auto& record : records) {
            identity_groups[identity_key(record.identity)][record.canonical_name].push_back(
                &record);
        }
        for (const auto& [key, field_groups] : identity_groups) {
            MetadataWritePlanSidecarEntry entry{
                .identity = field_groups.begin()->second.front()->identity,
                .occurrence_indexes = {},
                .fields = {},
                .true_peak = options.true_peak_loudness,
            };
            for (const auto& [name, intents] : field_groups) {
                entry.fields.push_back(finalize_field(intents));
            }
            for (std::size_t item_index = 0U; item_index < selection.item_count(); ++item_index) {
                const auto& source = selection.source(item_index);
                if (source.raw_path == raw_audio_path && source.logical_identity &&
                    identity_key(*source.logical_identity) == key) {
                    entry.occurrence_indexes.push_back(item_index);
                }
            }
            sidecar.entries.push_back(std::move(entry));
        }

        auto observed = core::observe_local_source_revision(raw_audio_path);
        if (!observed) {
            add_sidecar_issue(sidecar, MetadataWritePlanIssueKind::source_revalidation_failed,
                              std::move(observed.error()));
        } else {
            sidecar.observed_revision = *observed;
            if (sidecar.expected_revision && *sidecar.expected_revision != *observed) {
                add_sidecar_issue(
                    sidecar, MetadataWritePlanIssueKind::source_changed,
                    planner_error(core::ErrorCode::conflict,
                                  "the source changed after the loudness draft was captured",
                                  raw_audio_path));
            }
        }
        plan.sidecars.push_back(std::move(sidecar));
    }

    for (auto& source : plan.sources) {
        std::optional<core::LocalSourceRevision> expected;
        std::vector<std::size_t> missing_revision_items;
        std::vector<std::size_t> inconsistent_revision_items;
        for (const auto& change : source.changes) {
            for (const auto& intent : change.intents) {
                const auto& revision = selection.source(intent.item_index).source_revision;
                if (!revision) {
                    missing_revision_items.push_back(intent.item_index);
                } else if (!expected) {
                    expected = revision;
                } else if (*expected != *revision) {
                    inconsistent_revision_items.push_back(intent.item_index);
                }
            }
        }
        std::ranges::sort(missing_revision_items);
        missing_revision_items.erase(
            std::unique(missing_revision_items.begin(), missing_revision_items.end()),
            missing_revision_items.end());
        std::ranges::sort(inconsistent_revision_items);
        inconsistent_revision_items.erase(
            std::unique(inconsistent_revision_items.begin(), inconsistent_revision_items.end()),
            inconsistent_revision_items.end());
        source.expected_revision = expected;
        if (!missing_revision_items.empty()) {
            add_issue(source, MetadataWritePlanIssueKind::missing_baseline_revision,
                      planner_error(core::ErrorCode::conflict,
                                    "a staged occurrence has no captured source revision",
                                    source.raw_path),
                      std::nullopt, std::move(missing_revision_items));
        }
        if (!inconsistent_revision_items.empty()) {
            add_issue(source, MetadataWritePlanIssueKind::inconsistent_baseline_revision,
                      planner_error(core::ErrorCode::conflict,
                                    "staged occurrences disagree about the source revision",
                                    source.raw_path),
                      std::nullopt, std::move(inconsistent_revision_items));
        }

        for (auto& change : source.changes) {
            const auto& first = change.intents.front();
            change.conflicting_intents =
                std::ranges::any_of(change.intents, [&first](const auto& intent) {
                    return !same_intent(first, intent);
                });
            std::vector<std::size_t> intent_items;
            intent_items.reserve(change.intents.size());
            for (const auto& intent : change.intents) {
                intent_items.push_back(intent.item_index);
                change.unresolved_non_embedded_target =
                    change.unresolved_non_embedded_target ||
                    is_non_embedded(selection, intent, change.field_index);
            }
            if (change.conflicting_intents) {
                add_issue(source, MetadataWritePlanIssueKind::conflicting_logical_edits,
                          planner_error(
                              core::ErrorCode::conflict,
                              "logical occurrences stage different results for one physical field",
                              source.raw_path),
                          change.field_index, intent_items);
            }
            if (change.unresolved_non_embedded_target) {
                const auto blocked = std::ranges::find_if(change.intents, [&](const auto& intent) {
                    return is_non_embedded(selection, intent, change.field_index);
                });
                const auto& name =
                    change.display_name.empty() ? change.canonical_name : change.display_name;
                add_issue(source, MetadataWritePlanIssueKind::unresolved_non_embedded_target,
                          planner_error(core::ErrorCode::unsupported,
                                        non_embedded_reason(selection, *blocked,
                                                            change.field_index, name),
                                        source.raw_path),
                          change.field_index, std::move(intent_items));
            }
        }
        for (std::size_t left = 0U; left < source.changes.size(); ++left) {
            for (std::size_t right = left + 1U; right < source.changes.size(); ++right) {
                const auto& first = source.changes[left];
                const auto& second = source.changes[right];
                if (first.canonical_name != second.canonical_name) {
                    continue;
                }
                if (first.exact_native_name && second.exact_native_name &&
                    *first.exact_native_name != *second.exact_native_name) {
                    continue;
                }
                if (first.exact_native_name != second.exact_native_name) {
                    const auto& exact = first.exact_native_name ? first : second;
                    const auto& logical = first.exact_native_name ? second : first;
                    const auto identity = resolve_text_property_identity(*exact.exact_native_name);
                    if (!identity.conventional ||
                        identity.canonical_name != logical.canonical_name) {
                        continue;
                    }
                }
                add_issue(source, MetadataWritePlanIssueKind::conflicting_logical_edits,
                          planner_error(core::ErrorCode::conflict,
                                        "logical and exact-native edits overlap one physical "
                                        "metadata field",
                                        source.raw_path));
            }
        }
    }

    for (auto& source : plan.sources) {
        if (cancellation.is_cancellation_requested()) {
            return cancelled();
        }
        auto read = reader(source.raw_path, cancellation);
        if (!read) {
            if (read.error().code == core::ErrorCode::cancelled ||
                cancellation.is_cancellation_requested()) {
                return cancelled();
            }
            add_issue(source, MetadataWritePlanIssueKind::source_revalidation_failed,
                      std::move(read.error()));
            continue;
        }
        if (read->raw_path != source.raw_path) {
            add_issue(source, MetadataWritePlanIssueKind::source_revalidation_failed,
                      planner_error(core::ErrorCode::invariant,
                                    "metadata reader returned a different raw source path",
                                    source.raw_path));
            continue;
        }
        source.observed_revision = read->source_revision;
        source.adapter_name = read->adapter_name;
        if (source.expected_revision && *source.expected_revision != read->source_revision) {
            add_issue(source, MetadataWritePlanIssueKind::source_changed,
                      planner_error(core::ErrorCode::conflict,
                                    "the source changed after the metadata draft was captured",
                                    source.raw_path));
        }

        const auto effective = read->document.effective_fields();
        const auto effective_native = read->document.effective_native_fields();
        std::unordered_map<std::string, const EffectiveMetadataField*> effective_positions;
        effective_positions.reserve(effective.size());
        for (const auto& field : effective) {
            effective_positions.emplace(field.canonical_name, &field);
        }
        for (auto& change : source.changes) {
            if (change.exact_native_name) {
                const auto found =
                    std::ranges::find_if(effective_native, [&change](const auto& field) {
                        return canonicalize_native_field_name(field.native_name) ==
                               *change.exact_native_name;
                    });
                if (found != effective_native.end()) {
                    change.original_present = true;
                    change.native_name = found->native_name;
                    change.original_values = found->values;
                }
            } else if (const auto found = effective_positions.find(change.canonical_name);
                       found != effective_positions.end()) {
                change.original_present = true;
                change.native_name = found->second->native_name;
                change.original_values = found->second->values;
            }
        }

        // ADR-0143: an adapter that cannot take a safe tag write still
        // gets its ReplayGain — clean conventional REPLAYGAIN_* changes
        // divert into the file's loudness sidecar as a whole-file entry.
        // Everything else keeps the visible writer block below.
        const auto tag_write_safe = read->capabilities.fields_writable &&
                                    read->capabilities.unknown_data_preserved_on_write;
        if (!tag_write_safe && source.ready()) {
            std::vector<MetadataWritePlanLoudnessField> diverted;
            std::erase_if(source.changes, [&diverted](const MetadataWritePlanChange& change) {
                if (change.conflicting_intents || change.exact_native_name ||
                    change.intents.empty() || !is_cue_replay_gain_field(change.canonical_name)) {
                    return false;
                }
                MetadataWritePlanLoudnessField field{
                    .field_index = change.field_index,
                    .canonical_name = change.canonical_name,
                    .kind = change.intents.front().kind,
                    .values = change.intents.front().values,
                    .item_indexes = {},
                };
                for (const auto& intent : change.intents) {
                    field.item_indexes.push_back(intent.item_index);
                }
                diverted.push_back(std::move(field));
                return true;
            });
            if (!diverted.empty()) {
                auto sidecar_position = std::ranges::find(
                    plan.sidecars, source.raw_path, &MetadataWritePlanSidecar::raw_audio_path);
                if (sidecar_position == plan.sidecars.end()) {
                    plan.sidecars.push_back(MetadataWritePlanSidecar{
                        .raw_audio_path = source.raw_path,
                        .expected_revision = source.expected_revision,
                        .observed_revision = read->source_revision,
                        .entries = {},
                        .issues = {},
                    });
                    sidecar_position = std::prev(plan.sidecars.end());
                } else if (sidecar_position->expected_revision != source.expected_revision) {
                    add_sidecar_issue(
                        *sidecar_position,
                        MetadataWritePlanIssueKind::inconsistent_baseline_revision,
                        planner_error(core::ErrorCode::conflict,
                                      "staged occurrences disagree about the source revision",
                                      source.raw_path));
                }
                for (const auto& field : diverted) {
                    if (field.kind == StagedMetadataPatchKind::replace_values &&
                        (field.values.size() != 1U ||
                         !valid_cue_replay_gain_value(field.canonical_name,
                                                      field.values.front()))) {
                        add_sidecar_issue(*sidecar_position,
                                          MetadataWritePlanIssueKind::unsupported_field_mapping,
                                          planner_error(core::ErrorCode::unsupported,
                                                        "a sidecar loudness value must be a "
                                                        "single number the writer can "
                                                        "canonicalize",
                                                        source.raw_path),
                                          field.field_index, field.item_indexes);
                    }
                }
                const auto whole_file = std::ranges::find_if(
                    sidecar_position->entries, [](const MetadataWritePlanSidecarEntry& entry) {
                        return entry.identity == StagedLogicalIdentity{};
                    });
                if (whole_file == sidecar_position->entries.end()) {
                    sidecar_position->entries.push_back(MetadataWritePlanSidecarEntry{
                        .identity = {},
                        .occurrence_indexes = source.occurrence_indexes,
                        .fields = std::move(diverted),
                        .true_peak = options.true_peak_loudness,
                    });
                } else {
                    whole_file->fields.insert(whole_file->fields.end(),
                                              std::make_move_iterator(diverted.begin()),
                                              std::make_move_iterator(diverted.end()));
                }
            }
            if (source.changes.empty()) {
                // The whole draft moved to the sidecar; the tag plan no
                // longer owns this source.
                continue;
            }
        }

        if (!read->capabilities.fields_writable) {
            add_issue(source, MetadataWritePlanIssueKind::writer_unavailable,
                      planner_error(core::ErrorCode::unsupported,
                                    "the selected metadata adapter has no proven field writer",
                                    source.raw_path));
        }
        if (!read->capabilities.unknown_data_preserved_on_write) {
            add_issue(source, MetadataWritePlanIssueKind::preservation_unproven,
                      planner_error(core::ErrorCode::unsupported,
                                    "unknown container data preservation is not proven",
                                    source.raw_path));
        }
        if (read->adapter_name == "taglib-flac-v1" || read->adapter_name == "taglib-wavpack-v1" ||
            read->adapter_name == "taglib-mpeg-v1") {
            std::unordered_set<std::string> changed_logical_fields;
            std::unordered_set<std::string> changed_native_fields;
            changed_logical_fields.reserve(source.changes.size());
            changed_native_fields.reserve(source.changes.size());
            for (const auto& change : source.changes) {
                if (change.exact_native_name) {
                    changed_native_fields.insert(*change.exact_native_name);
                } else {
                    changed_logical_fields.insert(change.canonical_name);
                }
            }
            const auto unrepresentable_untouched =
                std::ranges::find_if(effective_native, [&](const auto& field) {
                    return !changed_logical_fields.contains(field.canonical_name) &&
                           !changed_native_fields.contains(
                               canonicalize_native_field_name(field.native_name)) &&
                           (field.values.empty() ||
                            std::ranges::any_of(field.values,
                                                [](const auto& value) { return value.empty(); }));
                });
            if (unrepresentable_untouched != effective_native.end()) {
                add_issue(
                    source, MetadataWritePlanIssueKind::unsupported_field_mapping,
                    planner_error(
                        core::ErrorCode::unsupported,
                        "an untouched FLAC field contains an exact empty value that the writer "
                        "cannot preserve",
                        source.raw_path));
            }
            for (const auto& change : source.changes) {
                if (change.intents.empty() || change.conflicting_intents) {
                    continue;
                }
                const auto& intent = change.intents.front();
                if (change.exact_native_name && !change.original_present) {
                    if (intent.kind == StagedMetadataPatchKind::remove_field) {
                        add_issue(
                            source, MetadataWritePlanIssueKind::source_changed,
                            planner_error(core::ErrorCode::conflict,
                                          "the exact native metadata field is no longer present",
                                          source.raw_path),
                            change.field_index);
                        continue;
                    }
                }
                const auto mapping_native_name =
                    change.exact_native_name && change.native_name.empty()
                        ? std::string_view{change.display_name}
                        : std::string_view{change.native_name};
                auto mapping = map_flac_text_field(change.canonical_name, change.display_name,
                                                   mapping_native_name, intent.kind, intent.values);
                if (!mapping) {
                    auto mapping_error = std::move(mapping.error());
                    mapping_error.context.push_back(
                        {.key = "path", .value = core::escape_raw_path(source.raw_path)});
                    add_issue(source, MetadataWritePlanIssueKind::unsupported_field_mapping,
                              std::move(mapping_error), change.field_index, [&change] {
                                  std::vector<std::size_t> indexes;
                                  indexes.reserve(change.intents.size());
                                  for (const auto& item : change.intents) {
                                      indexes.push_back(item.item_index);
                                  }
                                  return indexes;
                              }());
                }
            }
        }
    }

    std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<std::size_t>> physical_sources;
    for (std::size_t index = 0U; index < plan.sources.size(); ++index) {
        const auto& revision = plan.sources[index].observed_revision;
        if (revision) {
            physical_sources[{revision->device, revision->inode}].push_back(index);
        }
    }
    for (const auto& [identity, aliases] : physical_sources) {
        static_cast<void>(identity);
        if (aliases.size() < 2U) {
            continue;
        }
        for (const auto source_index : aliases) {
            auto& source = plan.sources[source_index];
            add_issue(source, MetadataWritePlanIssueKind::physical_source_alias,
                      planner_error(core::ErrorCode::conflict,
                                    "another staged raw path resolves to the same physical source",
                                    source.raw_path));
        }
    }

    // ADR-0143: sources whose entire draft diverted to the sidecar no
    // longer belong to the tag plan.
    std::erase_if(plan.sources,
                  [](const MetadataWritePlanSource& source) { return source.changes.empty(); });
    return plan;
}

core::Result<MetadataWritePlan> revalidate_metadata_write_plan(
    const StagedMetadataSelection& selection, const StagedMetadataPatchSet& patches,
    const core::CancellationToken& cancellation, const MetadataWritePlanOptions& options) {
    return build_metadata_write_plan(
        selection, patches,
        [](const std::string& raw_path, const core::CancellationToken& token) {
            return read_local_metadata(raw_path, token);
        },
        cancellation, options);
}

} // namespace trackknife::metadata
