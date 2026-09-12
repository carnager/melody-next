// SPDX-License-Identifier: GPL-3.0-only

#include "bench/replaygain_scan.hpp"

#include "bench/metadata_dialog_helpers.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/loudness/scan.hpp"
#include "trackknife/metadata/draft_document.hpp"

#include <QString>

#include <array>
#include <charconv>
#include <cstddef>
#include <map>
#include <string>
#include <thread>
#include <utility>

namespace trackknife::bench {

std::shared_ptr<ReplayGainScanOutcome> run_replaygain_scan(
    const std::shared_ptr<const metadata::StagedMetadataSelection> selection,
    const metadata::StagedMetadataPatchSet& draft, const std::vector<std::size_t>& items,
    const std::shared_ptr<const std::vector<MetadataPropertiesAudioSource>>& audio_sources,
    const ReplayGainScanSettings& settings, const std::shared_ptr<std::atomic_size_t>& completed,
    const core::CancellationToken& cancellation) {
    auto outcome = std::make_shared<ReplayGainScanOutcome>();
    auto documents = metadata::materialize_metadata_draft(*selection, draft, items, cancellation);
    if (!documents) {
        outcome->proposals = std::unexpected(std::move(documents.error()));
        return outcome;
    }
    std::vector<const metadata::MetadataDocument*> document_views;
    document_views.reserve(documents->size());
    for (const auto& document : *documents) {
        document_views.push_back(&document);
    }
    auto keys = loudness::assign_loudness_groups(settings.grouping, document_views, cancellation);
    if (!keys) {
        outcome->proposals = std::unexpected(std::move(keys.error()));
        return outcome;
    }
    std::vector<loudness::LoudnessScanItem> scan_items;
    scan_items.reserve(items.size());
    for (std::size_t position = 0U; position < items.size(); ++position) {
        scan_items.push_back(loudness::LoudnessScanItem{
            .item_index = items[position],
            .raw_path = selection->source(items[position]).raw_path,
            .selection = (*audio_sources)[items[position]].selection,
            .range = (*audio_sources)[items[position]].range,
            .album_key = (*keys)[position],
        });
    }
    const auto hardware = std::thread::hardware_concurrency();
    const auto parallelism =
        std::min<std::size_t>(loudness::maximum_scan_parallelism,
                              std::max<std::size_t>(1U, hardware == 0U ? 2U : hardware / 2U));
    auto scan = loudness::scan_loudness(
        scan_items, {.measure_true_peak = true, .maximum_parallelism = parallelism},
        [completed](const loudness::LoudnessScanProgress& update) {
            completed->store(update.completed_items);
        },
        cancellation);
    if (!scan) {
        outcome->proposals = std::unexpected(std::move(scan.error()));
        return outcome;
    }

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
    const auto track_peak_value = [&settings](const loudness::TrackLoudness& loudness) {
        return settings.true_peak ? loudness.true_peak.value_or(loudness.sample_peak)
                                  : loudness.sample_peak;
    };
    const auto album_peak_value = [&settings](const loudness::LoudnessAlbumScan& album) {
        return settings.true_peak ? album.true_peak.value_or(album.sample_peak) : album.sample_peak;
    };
    const auto lufs_text = [fixed_text](const double value) { return fixed_text(value, 2); };
    std::map<std::string, const loudness::LoudnessAlbumScan*> albums;
    for (const auto& album : scan->albums) {
        albums.emplace(album.album_key, &album);
        if (album.issue) {
            outcome->problems.push_back(PreparationFeedbackRow{
                .file = QStringLiteral("Album group"),
                .detail = QStringLiteral("Incomplete programme · %1")
                              .arg(display_utf8(album.issue->message)),
            });
        }
    }
    metadata::MetadataProposalSet proposals{
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
    // ADR-0147: one CSV data row per measured item, snapshotting the
    // measurement independent of later draft edits.
    const auto csv_field = [](QString value) {
        if (value.contains(QLatin1Char(',')) || value.contains(QLatin1Char('"')) ||
            value.contains(QLatin1Char('\n'))) {
            value.replace(QLatin1Char('"'), QStringLiteral("\"\""));
            value = QLatin1Char('"') + value + QLatin1Char('"');
        }
        return value;
    };
    for (std::size_t position = 0U; position < scan->tracks.size(); ++position) {
        const auto& track = scan->tracks[position];
        const auto analyzed = track.state == loudness::LoudnessScanState::analyzed &&
                              track.loudness && track.loudness->measurable();
        const auto* album_scan = [&]() -> const loudness::LoudnessAlbumScan* {
            const auto& album_key = scan_items[position].album_key;
            if (!album_key) {
                return nullptr;
            }
            const auto found = albums.find(*album_key);
            return found == albums.end() || !found->second->integrated_lufs ? nullptr
                                                                            : found->second;
        }();
        const auto status_text =
            analyzed                                                ? QStringLiteral("analyzed")
            : track.state == loudness::LoudnessScanState::failed    ? QStringLiteral("failed")
            : track.state == loudness::LoudnessScanState::cancelled ? QStringLiteral("cancelled")
            : track.state == loudness::LoudnessScanState::analyzed  ? QStringLiteral("unmeasurable")
                                                                    : QStringLiteral("pending");
        const auto document_title =
            position < documents->size()
                ? (*documents)[position].first_effective_value("title").value_or(std::string{})
                : std::string{};
        outcome->export_rows
                << QStringList{
                       csv_field(display_utf8(document_title)),
                       csv_field(QString::fromStdString(core::escape_raw_path(track.raw_path))),
                       analyzed ? display_utf8(lufs_text(track.loudness->integrated_lufs))
                                : QString{},
                       analyzed ? display_utf8(decibel_text(track.loudness->track_gain_db()))
                                : QString{},
                       analyzed ? display_utf8(peak_text(track_peak_value(*track.loudness)))
                                : QString{},
                       csv_field(display_utf8(
                           scan_items[position].album_key.value_or(std::string{}))),
                       album_scan ? display_utf8(decibel_text(*album_scan->album_gain_db()))
                                  : QString{},
                       album_scan ? display_utf8(peak_text(album_peak_value(*album_scan)))
                                  : QString{},
                       status_text,
                       settings.true_peak ? QStringLiteral("true_peak") : QStringLiteral("sample"),
                   }
                       .join(QLatin1Char(','));
        if (track.state != loudness::LoudnessScanState::analyzed || !track.loudness) {
            if (track.issue) {
                outcome->problems.push_back(PreparationFeedbackRow{
                    .file = QString::fromStdString(core::escape_raw_path(track.raw_path)),
                    .detail = display_utf8(track.issue->message),
                });
            }
            // ADR-0146: measurement failures and cancellations can be
            // retried; structurally unmeasurable tracks cannot.
            if (track.state == loudness::LoudnessScanState::failed ||
                track.state == loudness::LoudnessScanState::cancelled) {
                outcome->retry_items.push_back(track.item_index);
            }
            continue;
        }
        if (!track.loudness->measurable()) {
            outcome->problems.push_back(PreparationFeedbackRow{
                .file = QString::fromStdString(core::escape_raw_path(track.raw_path)),
                .detail =
                    QStringLiteral("Too short for gated loudness (under 400 ms); no gain staged"),
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
        proposals.items.push_back(std::move(item));
    }
    outcome->proposals = std::move(proposals);
    return outcome;
}

} // namespace trackknife::bench
