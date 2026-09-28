// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/loudness/replaygain.hpp"

#include "trackknife/core/local_sources.hpp"
#include "trackknife/formats/cue_sheet.hpp"
#include "trackknife/metadata/draft_document.hpp"
#include "trackknife/metadata/flac_mapping.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <map>
#include <thread>
#include <utility>

namespace trackknife::loudness {

core::Result<ReplayGainMeasurement>
measure_replaygain(const metadata::StagedMetadataSelection& selection,
                   const metadata::StagedMetadataPatchSet& draft,
                   const std::vector<std::size_t>& items,
                   const std::span<const ReplayGainAudio> audio, const ReplayGainSettings& settings,
                   const LoudnessScanProgressCallback& progress,
                   const core::CancellationToken& cancellation, const ReplayGainScanner& scanner) {
    auto documents = metadata::materialize_metadata_draft(selection, draft, items, cancellation);
    if (!documents) {
        return std::unexpected(std::move(documents.error()));
    }
    std::vector<const metadata::MetadataDocument*> document_views;
    document_views.reserve(documents->size());
    for (const auto& document : *documents) {
        document_views.push_back(&document);
    }
    auto keys = assign_loudness_groups(settings.grouping, document_views, cancellation);
    if (!keys) {
        return std::unexpected(std::move(keys.error()));
    }
    std::vector<LoudnessScanItem> scan_items;
    scan_items.reserve(items.size());
    for (std::size_t position = 0U; position < items.size(); ++position) {
        scan_items.push_back(LoudnessScanItem{
            .item_index = items[position],
            .raw_path = selection.source(items[position]).raw_path,
            .selection = audio[items[position]].selection,
            .range = audio[items[position]].range,
            .album_key = (*keys)[position],
        });
    }
    const auto hardware = std::thread::hardware_concurrency();
    const auto parallelism = std::min<std::size_t>(
        maximum_scan_parallelism, std::max<std::size_t>(1U, hardware == 0U ? 2U : hardware / 2U));
    const LoudnessScanOptions options{.measure_true_peak = true,
                                      .maximum_parallelism = parallelism};
    auto scan = scanner ? scanner(scan_items, options, progress, cancellation)
                        : scan_loudness(scan_items, options, progress, cancellation);
    if (!scan) {
        return std::unexpected(std::move(scan.error()));
    }

    ReplayGainMeasurement measured;
    // ReplayGain values are machine-readable: always the C locale's
    // decimal point, never the user locale's comma.
    const auto fixed_text = [](const double value, const int precision) {
        std::array<char, 32> buffer{};
        const auto ends = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                        std::chars_format::fixed, precision);
        return std::string{buffer.data(), ends.ptr};
    };
    const auto decibel_text = [fixed_text](const double value) {
        return fixed_text(value, 2) + " dB";
    };
    const auto peak_text = [fixed_text](const double value) { return fixed_text(value, 6); };
    // ADR-0148: the policy decides which measured peak becomes the
    // proposal; a missing true peak falls back to the sample peak.
    const auto track_peak_value = [&settings](const TrackLoudness& loudness) {
        return settings.true_peak ? loudness.true_peak.value_or(loudness.sample_peak)
                                  : loudness.sample_peak;
    };
    const auto album_peak_value = [&settings](const LoudnessAlbumScan& album) {
        return settings.true_peak ? album.true_peak.value_or(album.sample_peak) : album.sample_peak;
    };
    const auto lufs_text = [fixed_text](const double value) { return fixed_text(value, 2); };
    std::map<std::string, const LoudnessAlbumScan*> albums;
    for (const auto& album : scan->albums) {
        albums.emplace(album.album_key, &album);
        if (album.issue) {
            measured.problems.push_back(ReplayGainProblem{
                .raw_path = std::nullopt,
                .message = "Incomplete programme · " + album.issue->message,
            });
        }
    }
    measured.proposals = metadata::MetadataProposalSet{
        .provider_name = "ReplayGain",
        .provider_detail = "EBU R128 scan",
        .items = {},
    };
    const auto propose = [](metadata::MetadataProposalItem& item, std::string field,
                            std::string value, std::string rationale) {
        item.fields.push_back(metadata::ProposedFieldValues{
            .canonical_field = metadata::canonicalize_field_name(field),
            .display_field = std::move(field),
            .match_mode = metadata::MetadataFieldMatchMode::logical,
            .values = {std::move(value)},
            .confidence = 1.0,
            .rationale = std::move(rationale),
        });
    };
    for (std::size_t position = 0U; position < scan->tracks.size(); ++position) {
        const auto& track = scan->tracks[position];
        const auto analyzed = track.state == LoudnessScanState::analyzed && track.loudness &&
                              track.loudness->measurable();
        const auto* album_scan = [&]() -> const LoudnessAlbumScan* {
            const auto& album_key = scan_items[position].album_key;
            if (!album_key) {
                return nullptr;
            }
            const auto found = albums.find(*album_key);
            return found == albums.end() || !found->second->integrated_lufs ? nullptr
                                                                            : found->second;
        }();
        // ADR-0147: the measurement, independent of later draft edits.
        measured.rows.push_back(ReplayGainRow{
            .title =
                position < documents->size()
                    ? (*documents)[position].first_effective_value("title").value_or(std::string{})
                    : std::string{},
            .raw_path = track.raw_path,
            .integrated_lufs =
                analyzed ? lufs_text(track.loudness->integrated_lufs) : std::string{},
            .track_gain = analyzed ? decibel_text(track.loudness->track_gain_db()) : std::string{},
            .track_peak = analyzed ? peak_text(track_peak_value(*track.loudness)) : std::string{},
            .album_key = scan_items[position].album_key.value_or(std::string{}),
            .album_gain = album_scan ? decibel_text(*album_scan->album_gain_db()) : std::string{},
            .album_peak = album_scan ? peak_text(album_peak_value(*album_scan)) : std::string{},
            .status = analyzed                                      ? "analyzed"
                      : track.state == LoudnessScanState::failed    ? "failed"
                      : track.state == LoudnessScanState::cancelled ? "cancelled"
                      : track.state == LoudnessScanState::analyzed  ? "unmeasurable"
                                                                    : "pending",
            .peak_kind = settings.true_peak ? "true_peak" : "sample",
        });
        if (track.state != LoudnessScanState::analyzed || !track.loudness) {
            if (track.issue) {
                measured.problems.push_back(
                    ReplayGainProblem{.raw_path = track.raw_path, .message = track.issue->message});
            }
            // ADR-0146: measurement failures and cancellations can be
            // retried; structurally unmeasurable tracks cannot.
            if (track.state == LoudnessScanState::failed ||
                track.state == LoudnessScanState::cancelled) {
                measured.retry_items.push_back(track.item_index);
            }
            continue;
        }
        if (!track.loudness->measurable()) {
            measured.problems.push_back(ReplayGainProblem{
                .raw_path = track.raw_path,
                .message = "Too short for gated loudness (under 400 ms); no gain staged",
            });
            continue;
        }
        metadata::MetadataProposalItem item{
            .item_index = track.item_index,
            .fields = {},
            .artwork = {},
        };
        // ADR-0149: Opus tag writes speak RFC 7845 — Q7.8 R128 comments
        // at the -23 LUFS reference, no peaks. The sidecar-only policy
        // outranks this: the sidecar is Trackbench-owned and keeps the
        // conventional -18 LUFS fields regardless of format.
        const auto use_r128 = track.opus && !settings.sidecar_only;
        const auto r128_text = [](const double replaygain_db) {
            return formats::r128_gain_text(replaygain_db - formats::opus_r128_reference_shift_db);
        };
        std::string rationale = "Measured ";
        rationale += lufs_text(track.loudness->integrated_lufs);
        rationale += " LUFS integrated (EBU R128)";
        if (use_r128) {
            rationale += "; Q7.8 at -23 LUFS (RFC 7845)";
        } else if (settings.true_peak) {
            rationale += "; true peak";
        }
        if (use_r128) {
            propose(item, "R128_TRACK_GAIN", r128_text(track.loudness->track_gain_db()), rationale);
        } else {
            propose(item, "REPLAYGAIN_TRACK_GAIN", decibel_text(track.loudness->track_gain_db()),
                    rationale);
            propose(item, "REPLAYGAIN_TRACK_PEAK", peak_text(track_peak_value(*track.loudness)),
                    rationale);
        }
        const auto& key = scan_items[position].album_key;
        if (key) {
            const auto album = albums.find(*key);
            if (album != albums.end() && album->second->integrated_lufs) {
                std::string album_rationale = "Album programme measured ";
                album_rationale += lufs_text(*album->second->integrated_lufs);
                album_rationale += " LUFS integrated (EBU R128)";
                if (use_r128) {
                    album_rationale += "; Q7.8 at -23 LUFS (RFC 7845)";
                } else if (settings.true_peak) {
                    album_rationale += "; true peak";
                }
                if (use_r128) {
                    propose(item, "R128_ALBUM_GAIN", r128_text(*album->second->album_gain_db()),
                            album_rationale);
                } else {
                    propose(item, "REPLAYGAIN_ALBUM_GAIN",
                            decibel_text(*album->second->album_gain_db()), album_rationale);
                    propose(item, "REPLAYGAIN_ALBUM_PEAK",
                            peak_text(album_peak_value(*album->second)), album_rationale);
                }
            }
        }
        measured.proposals.items.push_back(std::move(item));
    }
    return measured;
}

void project_loudness_sidecar(metadata::MetadataDocument& document,
                              const metadata::LoudnessSidecar& sidecar,
                              const formats::AudioSourceSelection& selection,
                              const std::optional<formats::SampleRange>& range) {
    const auto start = range ? std::optional{range->start_sample} : std::nullopt;
    const auto end = range ? range->end_sample : std::nullopt;
    for (const auto& entry : sidecar.entries) {
        if (entry.stream_index != selection.stream_index ||
            entry.subsong_index != selection.subsong_index || entry.start_sample != start ||
            entry.end_sample != end) {
            continue;
        }
        const auto add = [&document](const char* name, const std::optional<double>& value,
                                     const bool gain) {
            if (!value) {
                return;
            }
            auto canonical = metadata::resolve_text_property_identity(name).canonical_name;
            if (canonical.empty()) {
                return;
            }
            document.fields.push_back(metadata::MetadataField{
                .canonical_name = std::move(canonical),
                .native_name = name,
                .values = {gain ? formats::replay_gain_decibel_text(*value)
                                : formats::replay_gain_peak_text(*value)},
                .qualifier = {},
                .provenance = metadata::FieldProvenance::sidecar,
            });
        };
        add("REPLAYGAIN_TRACK_GAIN", entry.track_gain_db, true);
        add("REPLAYGAIN_TRACK_PEAK", entry.track_peak, false);
        add("REPLAYGAIN_ALBUM_GAIN", entry.album_gain_db, true);
        add("REPLAYGAIN_ALBUM_PEAK", entry.album_peak, false);
        return;
    }
}

core::Result<StagedReplayGain> stage_replaygain(const metadata::StagedMetadataSelection& selection,
                                                const metadata::MetadataProposalSet& proposals,
                                                const core::CancellationToken& cancellation) {
    auto preview = metadata::metadata_proposal_preview(
        selection, metadata::StagedMetadataPatchSet{}, proposals, 0.0, cancellation);
    if (!preview) {
        return std::unexpected(std::move(preview.error()));
    }
    StagedReplayGain staged{
        .selection = selection, .patches = metadata::StagedMetadataPatchSet{}, .staged_fields = 0U};
    for (const auto& cell : preview->cells) {
        auto field_index = staged.selection.field_index(cell.canonical_field);
        if (!field_index) {
            auto ensured =
                staged.selection.ensure_missing_field(cell.canonical_field, cell.display_field);
            if (!ensured) {
                return std::unexpected(std::move(ensured.error()));
            }
            field_index = *ensured;
        }
        const auto patched =
            cell.after
                ? staged.patches.replace_values(staged.selection, cell.item_index, *field_index,
                                                *cell.after)
                : staged.patches.remove_field(staged.selection, cell.item_index, *field_index);
        if (!patched) {
            return std::unexpected(core::Error{patched.error()});
        }
        // What the file already says is no change to write.
        if (*patched) {
            ++staged.staged_fields;
        }
    }
    return staged;
}

} // namespace trackknife::loudness
