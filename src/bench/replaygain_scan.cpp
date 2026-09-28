// SPDX-License-Identifier: GPL-3.0-only

#include "bench/replaygain_scan.hpp"

#include "bench/metadata_dialog_helpers.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/loudness/replaygain.hpp"
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
    const core::CancellationToken& cancellation, const LoudnessScanner& scanner) {
    auto outcome = std::make_shared<ReplayGainScanOutcome>();
    // ADR-0156: the pipeline itself is shared with the engine
    // (loudness/replaygain.hpp); this is its face in the windows.
    std::vector<loudness::ReplayGainAudio> audio;
    audio.reserve(audio_sources->size());
    for (const auto& source : *audio_sources) {
        audio.push_back(
            loudness::ReplayGainAudio{.selection = source.selection, .range = source.range});
    }
    auto measured = loudness::measure_replaygain(
        *selection, draft, items, audio,
        loudness::ReplayGainSettings{.grouping = settings.grouping,
                                     .true_peak = settings.true_peak,
                                     .sidecar_only = settings.sidecar_only},
        [completed](const loudness::LoudnessScanProgress& update) {
            completed->store(update.completed_items);
        },
        cancellation, scanner);
    if (!measured) {
        outcome->proposals = std::unexpected(std::move(measured.error()));
        return outcome;
    }
    for (const auto& problem : measured->problems) {
        outcome->problems.push_back(PreparationFeedbackRow{
            .file = problem.raw_path
                        ? QString::fromStdString(core::display_raw_path(*problem.raw_path))
                        : QStringLiteral("Album group"),
            .detail = display_utf8(problem.message),
        });
    }
    // ADR-0147: one CSV data row per measured item.
    const auto csv_field = [](QString value) {
        if (value.contains(QLatin1Char(',')) || value.contains(QLatin1Char('"')) ||
            value.contains(QLatin1Char('\n'))) {
            value.replace(QLatin1Char('"'), QStringLiteral("\"\""));
            value = QLatin1Char('"') + value + QLatin1Char('"');
        }
        return value;
    };
    for (const auto& row : measured->rows) {
        outcome->export_rows
            << QStringList{
                   csv_field(display_utf8(row.title)),
                   csv_field(QString::fromStdString(core::display_raw_path(row.raw_path))),
                   display_utf8(row.integrated_lufs),
                   display_utf8(row.track_gain),
                   display_utf8(row.track_peak),
                   csv_field(display_utf8(row.album_key)),
                   display_utf8(row.album_gain),
                   display_utf8(row.album_peak),
                   display_utf8(row.status),
                   display_utf8(row.peak_kind),
               }
                   .join(QLatin1Char(','));
    }
    outcome->retry_items = std::move(measured->retry_items);
    outcome->proposals = std::move(measured->proposals);
    return outcome;
}

} // namespace trackknife::bench
