// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "trackknife/formats/cue_sheet.hpp"
#include "trackknife/formats/probe.hpp"
#include "trackknife/metadata/flac_mapping.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/loudness_sidecar.hpp"
#include "uicommon/local_artwork.hpp"
#include "uicommon/local_folder_tree_model.hpp"

#include <QFileDialog>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QTreeView>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(TRACKKNIFE_THREAD_SANITIZER)
extern "C" void __tsan_acquire(void* address);
extern "C" void __tsan_release(void* address);
#endif

namespace trackknife::bench {
namespace {

constexpr std::size_t probe_batch_size = 8U;
constexpr std::size_t discovery_row_limit = 100'000U;

// Case-insensitive first-value lookup over one ordered metadata scope.
[[nodiscard]] std::string probed_tag(const std::span<const formats::ProbedTag> tags,
                                     const std::string_view name) {
    for (const auto& tag : tags) {
        if (lowercased_ascii(tag.name) == name) {
            return tag.value;
        }
    }
    return {};
}

[[nodiscard]] std::string probed_tag(const formats::MediaProbe& probe,
                                     const std::string_view name) {
    return probed_tag(probe.tags, name);
}

void append_metadata_value(metadata::MetadataDocument& document, std::string native_name,
                           std::string value, const metadata::FieldProvenance provenance) {
    if (value.empty()) {
        return;
    }
    const auto canonical_name =
        metadata::resolve_text_property_identity(native_name).canonical_name;
    if (canonical_name.empty()) {
        return;
    }
    document.fields.push_back(metadata::MetadataField{
        .canonical_name = canonical_name,
        .native_name = std::move(native_name),
        .values = {std::move(value)},
        .qualifier = {},
        .provenance = provenance,
    });
}

void prepend_metadata_value(metadata::MetadataDocument& document, std::string native_name,
                            std::string value, const metadata::FieldProvenance provenance) {
    if (value.empty()) {
        return;
    }
    const auto canonical_name =
        metadata::resolve_text_property_identity(native_name).canonical_name;
    if (canonical_name.empty()) {
        return;
    }
    document.fields.insert(document.fields.begin(), metadata::MetadataField{
                                                        .canonical_name = canonical_name,
                                                        .native_name = std::move(native_name),
                                                        .values = {std::move(value)},
                                                        .qualifier = {},
                                                        .provenance = provenance,
                                                    });
}

void append_probed_metadata(metadata::MetadataDocument& document,
                            const std::span<const formats::ProbedTag> tags,
                            const metadata::FieldProvenance provenance) {
    for (const auto& tag : tags) {
        append_metadata_value(document, tag.name, tag.value, provenance);
    }
}

// TagLib is the primary generic property adapter. FFmpeg's container/stream
// projection fills only names that TagLib did not expose, retaining repeated
// values in demuxer order without duplicating the primary representation.
void append_missing_probed_metadata(metadata::MetadataDocument& document,
                                    const std::span<const formats::ProbedTag> tags) {
    std::unordered_set<std::string> primary_names;
    primary_names.reserve(document.fields.size());
    for (const auto& field : document.fields) {
        primary_names.insert(field.canonical_name);
    }
    for (const auto& tag : tags) {
        const auto canonical_name =
            metadata::resolve_text_property_identity(tag.name).canonical_name;
        if (primary_names.contains(canonical_name)) {
            continue;
        }
        const auto semantic = probed_semantic_alias(tag.name);
        if (semantic && primary_names.contains(std::string{*semantic})) {
            continue;
        }
        append_metadata_value(document, tag.name, tag.value, metadata::FieldProvenance::stream);
    }
    remove_shadowed_probed_metadata(document);
}

[[nodiscard]] int best_audio_sample_rate(const formats::MediaProbe& probe) {
    if (!probe.best_audio_stream) {
        return 0;
    }
    const auto found = std::ranges::find(probe.audio_streams, *probe.best_audio_stream,
                                         &formats::AudioStreamInfo::stream_index);
    return found == probe.audio_streams.end() ? 0 : found->sample_rate;
}

[[nodiscard]] std::optional<std::int64_t> sample_duration_ms(const std::int64_t frames,
                                                             const int sample_rate) {
    if (frames < 0 || sample_rate <= 0) {
        return std::nullopt;
    }
    const auto seconds = frames / sample_rate;
    if (seconds > std::numeric_limits<std::int64_t>::max() / 1'000) {
        return std::nullopt;
    }
    return (seconds * 1'000) + (((frames % sample_rate) * 1'000) / sample_rate);
}

// ADR-0153: the ingest probe already knows the stream facts; keep them
// on the row so tab searches and the Find bar can answer technical
// questions without re-probing.
[[nodiscard]] std::optional<LocalTrackTechnicals>
probe_technicals(const formats::MediaProbe& probe) {
    if (!probe.best_audio_stream) {
        return std::nullopt;
    }
    const auto found = std::ranges::find(probe.audio_streams, *probe.best_audio_stream,
                                         &formats::AudioStreamInfo::stream_index);
    if (found == probe.audio_streams.end()) {
        return std::nullopt;
    }
    LocalTrackTechnicals technicals;
    technicals.codec = found->codec_name;
    technicals.sample_rate = found->sample_rate;
    technicals.bits = formats::bits_per_sample_hint(found->sample_format);
    technicals.channels = found->channels;
    technicals.bit_rate = found->bit_rate > 0 ? found->bit_rate : probe.bit_rate;
    return technicals;
}

[[nodiscard]] LocalTrackRow
whole_file_row(const formats::MediaProbe& probe, metadata::MetadataDocument document,
               std::optional<core::LocalSourceRevision> source_revision) {
    append_missing_probed_metadata(document, probe.tags);
    LocalTrackRow row;
    row.raw_path = probe.raw_path;
    row.duration_ms = probe.duration_ms;
    row.metadata = std::move(document);
    row.source_revision = source_revision;
    row.technicals = probe_technicals(probe);
    project_display_metadata(row);
    row.probed = true;
    return row;
}

[[nodiscard]] std::string chapter_logical_reference(const formats::MediaProbe& probe,
                                                    const formats::ProbedChapter& chapter) {
    std::string reference{"container-chapter-v1"};
    reference.push_back('\0');
    reference += probe.raw_path;
    reference.push_back('\0');
    reference += std::to_string(probe.best_audio_stream.value_or(-1));
    reference.push_back('\0');
    reference += std::to_string(chapter.id);
    reference.push_back('\0');
    reference += std::to_string(chapter.source_index);
    return reference;
}

[[nodiscard]] std::vector<LocalTrackRow>
chapter_rows(const formats::MediaProbe& probe, const metadata::MetadataDocument& document,
             const std::optional<core::LocalSourceRevision>& source_revision) {
    const auto sample_rate = best_audio_sample_rate(probe);
    if (sample_rate <= 0 || probe.chapters.empty()) {
        return {};
    }
    std::vector<LocalTrackRow> rows;
    rows.reserve(probe.chapters.size());
    for (std::size_t index = 0U; index < probe.chapters.size(); ++index) {
        const auto& chapter = probe.chapters[index];
        LocalTrackRow row;
        row.raw_path = probe.raw_path;
        row.logical_reference = chapter_logical_reference(probe, chapter);
        row.segment = formats::SampleRange{.start_sample = chapter.start_sample,
                                           .end_sample = chapter.end_sample};
        row.metadata = document;
        row.source_revision = source_revision;
        append_probed_metadata(row.metadata, chapter.tags, metadata::FieldProvenance::segment);
        if (probed_tag(chapter.tags, "title").empty()) {
            append_metadata_value(row.metadata, "TITLE", "Chapter " + std::to_string(index + 1U),
                                  metadata::FieldProvenance::segment);
        }
        if (metadata_value(row.metadata, {"album"}).empty()) {
            append_metadata_value(row.metadata, "ALBUM", metadata_value(document, {"title"}),
                                  metadata::FieldProvenance::segment);
        }
        if (metadata_value(row.metadata, {"albumartist"}).empty()) {
            append_metadata_value(row.metadata, "ALBUMARTIST", metadata_value(document, {"artist"}),
                                  metadata::FieldProvenance::segment);
        }
        if (probed_tag(chapter.tags, "tracknumber").empty() &&
            probed_tag(chapter.tags, "track").empty()) {
            append_metadata_value(row.metadata, "TRACKNUMBER", std::to_string(index + 1U),
                                  metadata::FieldProvenance::segment);
        }
        row.duration_ms =
            sample_duration_ms(chapter.end_sample - chapter.start_sample, sample_rate);
        row.technicals = probe_technicals(probe);
        project_display_metadata(row);
        row.probed = true;
        rows.push_back(std::move(row));
    }
    return rows;
}

[[nodiscard]] std::string subsong_logical_reference(const formats::MediaProbe& probe,
                                                    const formats::ProbedSubsong& subsong) {
    std::string reference{"codec-subsong-v1"};
    reference.push_back('\0');
    reference += probe.raw_path;
    reference.push_back('\0');
    reference += std::to_string(subsong.selection.stream_index.value_or(-1));
    reference.push_back('\0');
    reference += std::to_string(subsong.selection.subsong_index.value_or(-1));
    reference.push_back('\0');
    reference += std::to_string(subsong.source_index);
    return reference;
}

[[nodiscard]] std::vector<LocalTrackRow>
subsong_rows(const formats::MediaProbe& probe, const metadata::MetadataDocument& document,
             const std::optional<core::LocalSourceRevision>& source_revision) {
    if (probe.subsongs.empty()) {
        return {};
    }
    const auto container_title = probed_tag(probe, "title");
    std::vector<LocalTrackRow> rows;
    rows.reserve(probe.subsongs.size());
    for (std::size_t index = 0U; index < probe.subsongs.size(); ++index) {
        const auto& subsong = probe.subsongs[index];
        LocalTrackRow row;
        row.raw_path = probe.raw_path;
        row.logical_reference = subsong_logical_reference(probe, subsong);
        row.selection = subsong.selection;
        if (subsong.duration_samples && *subsong.duration_samples > 0) {
            row.segment =
                formats::SampleRange{.start_sample = 0, .end_sample = *subsong.duration_samples};
        }
        row.metadata = document;
        row.source_revision = source_revision;
        if (!subsong.name.empty()) {
            append_metadata_value(row.metadata, "TITLE", subsong.name,
                                  metadata::FieldProvenance::segment);
        }
        append_probed_metadata(row.metadata, subsong.tags, metadata::FieldProvenance::segment);
        auto title = metadata_value(row.metadata, {"title"});
        if (title.empty() || title == container_title) {
            prepend_metadata_value(row.metadata, "TITLE", "Subsong " + std::to_string(index + 1U),
                                   metadata::FieldProvenance::segment);
        }
        if (metadata_value(row.metadata, {"album"}).empty()) {
            append_metadata_value(row.metadata, "ALBUM", container_title,
                                  metadata::FieldProvenance::segment);
        }
        if (metadata_value(row.metadata, {"albumartist"}).empty()) {
            append_metadata_value(row.metadata, "ALBUMARTIST",
                                  metadata_value(row.metadata, {"artist"}),
                                  metadata::FieldProvenance::segment);
        }
        append_metadata_value(row.metadata, "TRACKNUMBER", std::to_string(index + 1U),
                              metadata::FieldProvenance::segment);
        row.duration_ms = subsong.duration_ms;
        row.technicals = probe_technicals(probe);
        project_display_metadata(row);
        row.probed = true;
        rows.push_back(std::move(row));
    }
    return rows;
}

// Folder expansion only ingests plausible audio and external cue sheets;
// explicitly opened files always pass regardless (core discovery contract).
// The probe/resolver still gates everything this list lets through.
constexpr std::array<std::string_view, 28> audio_extensions{
    "flac", "mp3", "ogg",  "oga", "opus", "m4a", "mp4", "aac", "wv",  "wav",
    "rf64", "w64", "aiff", "aif", "aifc", "ape", "mpc", "tta", "spx", "mka",
    "wma",  "dsf", "dff",  "cue", "mod",  "xm",  "s3m", "it"};

[[nodiscard]] bool is_cue_path(const std::string& raw_path) {
    const auto slash = raw_path.find_last_of('/');
    const auto dot = raw_path.find_last_of('.');
    return dot != std::string::npos && (slash == std::string::npos || dot > slash) &&
           lowercased_ascii(raw_path.substr(dot + 1U)) == "cue";
}

[[nodiscard]] std::string cue_remark(const formats::CueLogicalTrack& track,
                                     const std::string_view name) {
    const auto found = std::ranges::find_if(
        track.remarks.rbegin(), track.remarks.rend(),
        [name](const formats::CueMetadataField& field) { return field.name == name; });
    return found == track.remarks.rend() ? std::string{} : found->value;
}

[[nodiscard]] std::string cue_logical_reference(const std::string& raw_cue_path,
                                                const formats::CueLogicalTrack& track) {
    return cue_track_logical_reference(raw_cue_path, track.file_index, track.track_index);
}

[[nodiscard]] LocalTrackRow
cue_row(const formats::ResolvedCueSheet& sheet, const formats::ResolvedCueTrack& resolved,
        const metadata::MetadataDocument& embedded_document,
        const std::optional<core::LocalSourceRevision>& source_revision) {
    const auto& cue = resolved.cue;
    LocalTrackRow row;
    row.raw_path = resolved.raw_source_path;
    row.logical_reference = cue_logical_reference(sheet.raw_cue_path, cue);
    row.segment = resolved.sample_range;
    row.metadata = embedded_document;
    row.source_revision = source_revision;
    for (const auto& remark : cue.remarks) {
        append_metadata_value(row.metadata, remark.name, remark.value,
                              metadata::FieldProvenance::segment);
    }
    append_metadata_value(row.metadata, "TITLE", cue.title.value_or(std::string{}),
                          metadata::FieldProvenance::sidecar);
    append_metadata_value(row.metadata, "ARTIST", cue.performer.value_or(std::string{}),
                          metadata::FieldProvenance::sidecar);
    append_metadata_value(row.metadata, "ALBUM", cue.album_title.value_or(std::string{}),
                          metadata::FieldProvenance::sidecar);
    append_metadata_value(row.metadata, "ALBUMARTIST", cue.album_performer.value_or(std::string{}),
                          metadata::FieldProvenance::sidecar);
    append_metadata_value(row.metadata, "SONGWRITER", cue.songwriter.value_or(std::string{}),
                          metadata::FieldProvenance::sidecar);
    append_metadata_value(row.metadata, "ISRC", cue.isrc.value_or(std::string{}),
                          metadata::FieldProvenance::sidecar);
    // REM values retain source order at segment scope. The established CUE
    // policy selects the last DATE as the effective sidecar projection.
    append_metadata_value(row.metadata, "DATE", cue_remark(cue, "DATE"),
                          metadata::FieldProvenance::sidecar);
    append_metadata_value(row.metadata, "TRACKNUMBER", std::to_string(cue.track_number),
                          metadata::FieldProvenance::sidecar);
    row.duration_ms = resolved.duration_ms;
    project_display_metadata(row);
    row.probed = true;
    return row;
}

// ADR-0141: a fresh loudness sidecar projects onto its rows at sidecar
// provenance — the highest effective precedence — so Properties and
// playback see the sidecar values without any I/O at play time. Stale
// or unreadable sidecars project nothing; the write path surfaces
// corruption, probing never fails on it.
void apply_loudness_sidecar_projection(
    LocalTrackRow& fallback, std::vector<LocalTrackRow>& rows, const std::string& raw_path,
    const std::optional<core::LocalSourceRevision>& source_revision) {
    if (!source_revision) {
        return;
    }
    const auto sidecar = metadata::read_loudness_sidecar(raw_path);
    if (!sidecar || !*sidecar || !(*sidecar)->matches(*source_revision)) {
        return;
    }
    const auto project = [&](LocalTrackRow& row) {
        const auto start = row.segment ? std::optional{row.segment->start_sample} : std::nullopt;
        const auto end = row.segment ? row.segment->end_sample : std::nullopt;
        for (const auto& entry : (*sidecar)->entries) {
            if (entry.stream_index != row.selection.stream_index ||
                entry.subsong_index != row.selection.subsong_index || entry.start_sample != start ||
                entry.end_sample != end) {
                continue;
            }
            const auto add = [&row](const char* name, const std::optional<double>& value,
                                    const bool gain) {
                if (value) {
                    append_metadata_value(row.metadata, name,
                                          gain ? formats::replay_gain_decibel_text(*value)
                                               : formats::replay_gain_peak_text(*value),
                                          metadata::FieldProvenance::sidecar);
                }
            };
            add("REPLAYGAIN_TRACK_GAIN", entry.track_gain_db, true);
            add("REPLAYGAIN_TRACK_PEAK", entry.track_peak, false);
            add("REPLAYGAIN_ALBUM_GAIN", entry.album_gain_db, true);
            add("REPLAYGAIN_ALBUM_PEAK", entry.album_peak, false);
            return;
        }
    };
    project(fallback);
    for (auto& row : rows) {
        project(row);
    }
}

} // namespace

void BenchMainWindow::enqueueUnprobedRows(ListTab& tab) {
    // ADR-0227: a remote tab's files are on the remote's machine, and need
    // not be reachable from this one. What a row is missing is asked of that
    // engine's index instead of read from a file here.
    if (tab.document.remote) {
        enrichRemoteRows(tab);
        return;
    }
    const auto id = QString::fromStdString(tab.document.id.to_string());
    const auto& rows = tab.model->rows();
    for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
        if (!rows[static_cast<std::size_t>(row)].probed) {
            probe_queue_.push_back(ProbeJob{
                .document_id = id,
                .raw_path = rows[static_cast<std::size_t>(row)].raw_path,
                .hint_row = row,
            });
        }
    }
    pumpProbeQueue();
}

// A remote tab's counterpart to probing: rows that are bare paths -- dragged
// from the remote's library, or taken over from its queue, which holds paths
// and not tags -- are filled in from the remote engine's index, keeping each
// row's identity. A path the index does not know keeps its file name and is
// not asked about again: left unanswered, it would be asked on every look at
// the tab, ahead of everything else on the connection.
void BenchMainWindow::enrichRemoteRows(ListTab& tab) {
    if (!remote_catalogue_source_) {
        return;
    }
    std::vector<std::string> paths;
    std::unordered_set<std::string> asked;
    for (const auto& row : tab.model->rows()) {
        // A CUE track's title is its own, not the file's.
        if (!row.probed && !row.segment && asked.insert(row.raw_path).second) {
            paths.push_back(row.raw_path);
        }
    }
    if (paths.empty()) {
        return;
    }
    // Opened here, used there: it holds its own reference to the connection.
    std::shared_ptr<engine::Catalogue> catalogue{remote_catalogue_source_->open()};
    using Found = std::vector<persistence::LibraryTrackSnapshot>;
    auto* watcher = new QFutureWatcher<Found>(this);
    const auto id = QString::fromStdString(tab.document.id.to_string());
    connect(watcher, &QFutureWatcher<Found>::finished, this,
            [this, watcher, id, asked = std::move(asked)] {
                watcher->deleteLater();
                auto found = watcher->result();
                auto* target = tabForDocument(id);
                if (target == nullptr) {
                    return;
                }
                std::unordered_map<std::string, LocalTrackRow> by_path;
                for (auto& snapshot : found) {
                    auto row = cached_library_row(std::move(snapshot));
                    auto path = row.raw_path;
                    by_path.emplace(std::move(path), std::move(row));
                }
                bool applied = false;
                for (int row = 0; row < target->model->rowCount(); ++row) {
                    const auto& current = target->model->rows()[static_cast<std::size_t>(row)];
                    if (current.probed || current.segment || !asked.contains(current.raw_path)) {
                        continue;
                    }
                    const auto match = by_path.find(current.raw_path);
                    auto filled = match != by_path.end() ? match->second : current;
                    filled.selection = current.selection;
                    std::vector<LocalTrackRow> one;
                    one.push_back(std::move(filled));
                    applied =
                        target->model->applyProbeRows(current.raw_path, row, std::move(one)) ||
                        applied;
                }
                if (applied) {
                    schedulePersist();
                    syncArtwork(*target);
                }
            });
    watcher->setFuture(QtConcurrent::run([catalogue, paths = std::move(paths)] {
        // All at once; a batch holding one path the index does not know is
        // refused whole, and then each is asked alone.
        if (auto all = catalogue->cached_tracks(paths, {})) {
            return std::move(*all);
        }
        Found found;
        for (const auto& path : paths) {
            if (auto one = catalogue->cached_tracks({path}, {}); one && !one->empty()) {
                found.push_back(std::move(one->front()));
            }
        }
        return found;
    }));
}

void BenchMainWindow::insertRemotePaths(ListTab& tab, std::vector<std::string> raw_paths,
                                        const int insertion_row) {
    std::vector<LocalTrackRow> rows;
    rows.reserve(raw_paths.size());
    for (auto& raw_path : raw_paths) {
        LocalTrackRow row;
        row.raw_path = std::move(raw_path);
        row.title = core::escape_raw_path(row.raw_path.substr(row.raw_path.find_last_of('/') + 1));
        rows.push_back(std::move(row));
    }
    if (rows.empty()) {
        return;
    }
    tab.model->appendRows(std::move(rows), insertion_row);
    markTabDirty(tab);
    enqueueUnprobedRows(tab);
}

void BenchMainWindow::pumpProbeQueue() {
    if (probe_running_ || probe_queue_.empty()) {
        return;
    }
    std::vector<ProbeJob> batch;
    batch.reserve(probe_batch_size);
    while (!probe_queue_.empty() && batch.size() < probe_batch_size) {
        batch.push_back(std::move(probe_queue_.front()));
        probe_queue_.pop_front();
    }
    probe_running_ = true;
    connect(&probe_watcher_, &QFutureWatcher<std::vector<ProbeOutcome>>::finished, this,
            &BenchMainWindow::finishProbeBatch, Qt::SingleShotConnection);
    probe_watcher_.setFuture(QtConcurrent::run(
        [jobs = std::move(batch), cancellation = probe_cancellation_.token()]() mutable {
            std::vector<ProbeOutcome> outcomes;
            outcomes.reserve(jobs.size());
            for (auto& job : jobs) {
                if (cancellation.is_cancellation_requested()) {
                    break;
                }
                LocalTrackRow fallback;
                std::vector<LocalTrackRow> rows;
                metadata::MetadataDocument document;
                std::optional<core::LocalSourceRevision> source_revision;
                if (auto read = metadata::read_local_metadata(job.raw_path, cancellation); read) {
                    document = std::move(read->document);
                    source_revision = read->source_revision;
                }
                if (auto probe = formats::probe_local_media(job.raw_path, cancellation); probe) {
                    append_missing_probed_metadata(document, probe->tags);
                    fallback = whole_file_row(*probe, document, source_revision);
                    rows = subsong_rows(*probe, document, source_revision);
                    if (rows.empty()) {
                        rows = chapter_rows(*probe, document, source_revision);
                    }
                    apply_loudness_sidecar_projection(fallback, rows, job.raw_path,
                                                      source_revision);
                } else {
                    fallback.raw_path = job.raw_path;
                    fallback.metadata = std::move(document);
                    fallback.source_revision = source_revision;
                    project_display_metadata(fallback);
                    fallback.probed = true;
                }
                // A failed probe still marks the row probed so it is not
                // retried in a loop; the file-name fallback stays visible.
                if (rows.empty()) {
                    rows.push_back(fallback);
                }
                outcomes.push_back(ProbeOutcome{.job = std::move(job),
                                                .rows = std::move(rows),
                                                .whole_file_fallback = std::move(fallback)});
            }
            return outcomes;
        }));
}

void BenchMainWindow::finishProbeBatch() {
    probe_running_ = false;
    auto outcomes = probe_watcher_.result();
    bool applied = false;
    bool logical_track_limit_hit = false;
    QSet<QString> touched_documents;
    for (auto& outcome : outcomes) {
        auto* tab = tabForDocument(outcome.job.document_id);
        if (tab == nullptr) {
            continue;
        }
        const auto current_count = static_cast<std::size_t>(tab->model->rowCount());
        const auto projected_count =
            current_count == 0U ? 0U : current_count - 1U + outcome.rows.size();
        if (outcome.rows.size() > 1U && current_count > 0U &&
            projected_count > discovery_row_limit) {
            outcome.rows.clear();
            outcome.rows.push_back(std::move(outcome.whole_file_fallback));
            logical_track_limit_hit = true;
        }
        if (tab->model->applyProbeRows(outcome.job.raw_path, outcome.job.hint_row,
                                       std::move(outcome.rows))) {
            applied = true;
            touched_documents.insert(outcome.job.document_id);
        }
    }
    if (applied) {
        schedulePersist();
    }
    if (logical_track_limit_hit) {
        statusBar()->showMessage(QStringLiteral("Logical-track expansion hit the local row limit"),
                                 5'000);
    }
    for (const auto& document_id : touched_documents) {
        if (auto* tab = tabForDocument(document_id); tab != nullptr) {
            syncArtwork(*tab);
        }
    }
    pumpProbeQueue();
}

void BenchMainWindow::syncArtwork(ListTab& tab) {
    // A remote tab's files are on the remote's machine (ADR-0227), and
    // nothing of it need be mounted here: its covers come from its engine.
    std::shared_ptr<engine::Catalogue> engine;
    if (tab.document.remote) {
        if (!remote_catalogue_source_) {
            return;
        }
        engine = remote_catalogue_source_->open();
    }
    const auto& rows = tab.model->rows();
    for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
        const auto& track = rows[static_cast<std::size_t>(row)];
        if (track.album.empty() && track.artist.empty() && track.album_artist.empty()) {
            continue;
        }
        const auto key = tab.model->groupKey(row);
        if (const auto cached = artwork_cache_.constFind(key);
            cached != artwork_cache_.constEnd()) {
            if (!cached->isNull() && !tab.model->hasArtwork(key)) {
                tab.model->setArtwork(key, *cached);
            }
            continue;
        }
        if (!artwork_pending_.contains(key)) {
            artwork_pending_.insert(key);
            artwork_queue_.push_back(
                ArtworkJob{.key = key, .raw_path = track.raw_path, .engine = engine});
        }
    }
    pumpArtworkQueue();
}

void BenchMainWindow::invalidateArtwork(const std::string& raw_path) {
    QSet<QString> keys;
    for (const auto& tab : list_tabs_) {
        const auto& rows = tab->model->rows();
        for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
            if (rows[static_cast<std::size_t>(row)].raw_path == raw_path) {
                keys.insert(tab->model->groupKey(row));
            }
        }
    }
    for (const auto& key : keys) {
        artwork_cache_.remove(key);
        artwork_pending_.remove(key);
        std::erase_if(artwork_queue_, [&key](const ArtworkJob& job) { return job.key == key; });
        if (artwork_running_ && artwork_outcome_ && artwork_outcome_->key == key) {
            artwork_invalidated_while_loading_.insert(key);
        }
        for (const auto& tab : list_tabs_) {
            tab->model->setArtwork(key, {});
        }
    }
    for (const auto& tab : list_tabs_) {
        syncArtwork(*tab);
    }
}

void BenchMainWindow::pumpArtworkQueue() {
    if (artwork_running_ || artwork_queue_.empty()) {
        return;
    }
    auto job = std::move(artwork_queue_.front());
    artwork_queue_.pop_front();
    artwork_running_ = true;
    connect(&artwork_watcher_, &QFutureWatcher<void>::finished, this,
            &BenchMainWindow::finishArtworkLoad, Qt::SingleShotConnection);
    artwork_outcome_ =
        std::make_shared<ArtworkOutcome>(ArtworkOutcome{.key = std::move(job.key), .image = {}});
    artwork_watcher_.setFuture(
        QtConcurrent::run([raw_path = std::move(job.raw_path), engine = std::move(job.engine),
                           outcome = artwork_outcome_,
                           cancellation = probe_cancellation_.token()] {
            if (engine) {
                const auto bytes = engine->artwork(raw_path, cancellation);
                outcome->image = bytes ? ui::artworkThumbnail(*bytes) : QImage{};
                outcome->failed = !bytes;
            } else {
                outcome->image = ui::loadLocalArtwork(raw_path, cancellation);
            }
#if defined(TRACKKNIFE_THREAD_SANITIZER)
            __tsan_release(outcome.get());
#endif
        }));
}

void BenchMainWindow::finishArtworkLoad() {
    artwork_running_ = false;
    auto outcome = artwork_outcome_;
    if (!outcome) {
        pumpArtworkQueue();
        return;
    }
#if defined(TRACKKNIFE_THREAD_SANITIZER)
    __tsan_acquire(outcome.get());
#endif
    if (artwork_invalidated_while_loading_.remove(outcome->key)) {
        // Invalidation already queued a fresh read and its pending marker now
        // owns this key. Discard only the stale in-flight result.
        pumpArtworkQueue();
        return;
    }
    if (outcome->failed) {
        // Not known to have no cover: asked again when the tab next looks.
        artwork_pending_.remove(outcome->key);
        pumpArtworkQueue();
        return;
    }
    // Failed lookups are cached as null so a missing cover is asked once, not
    // on every metadata refresh.
    artwork_cache_.insert(outcome->key, outcome->image);
    if (!outcome->image.isNull()) {
        for (int index = 0; index < tabs_->count(); ++index) {
            auto* view = qobject_cast<QTableView*>(tabs_->widget(index));
            if (view == nullptr) {
                continue;
            }
            auto* tab = static_cast<ListTab*>(view->property("bench-tab-pointer").value<void*>());
            if (tab != nullptr && !tab->model->hasArtwork(outcome->key)) {
                tab->model->setArtwork(outcome->key, outcome->image);
            }
        }
    }
    pumpArtworkQueue();
}

void BenchMainWindow::openFilesDialog() {
    const auto files = QFileDialog::getOpenFileNames(this, QStringLiteral("Open files"));
    std::vector<std::string> raw_paths;
    raw_paths.reserve(static_cast<std::size_t>(files.size()));
    for (const auto& file : files) {
        const auto encoded = QFile::encodeName(file);
        raw_paths.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
    }
    openLocalPaths(std::move(raw_paths));
}

void BenchMainWindow::openFolderDialog() {
    const auto folder = QFileDialog::getExistingDirectory(this, QStringLiteral("Open folder"));
    if (folder.isEmpty()) {
        return;
    }
    const auto encoded = QFile::encodeName(folder);
    openLocalPaths({{encoded.constData(), static_cast<std::size_t>(encoded.size())}});
}

void BenchMainWindow::addFolderRoot() {
    const auto folder = QFileDialog::getExistingDirectory(this, QStringLiteral("Bookmark folder"));
    if (folder.isEmpty()) {
        return;
    }
    const auto encoded = QFile::encodeName(folder);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    addFolderBookmark(raw_path);
    revealFolderPath(raw_path);
}

void BenchMainWindow::openLocalPaths(std::vector<std::string> raw_paths) {
    if (raw_paths.empty()) {
        return;
    }
    if (!lists_restored_) {
        pending_open_paths_.insert(pending_open_paths_.end(),
                                   std::make_move_iterator(raw_paths.begin()),
                                   std::make_move_iterator(raw_paths.end()));
        return;
    }
    // Files on this computer go into a local list (ADR-0227): the one on
    // screen when it is one, else the first there is, else a new one. Into a
    // remote tab they would be the remote's paths, which they are not.
    auto* tab = currentListTab();
    if (tab == nullptr || tab->document.remote) {
        const auto local = std::ranges::find_if(
            list_tabs_, [](const auto& candidate) { return !candidate->document.remote; });
        tab = local != list_tabs_.end()
                  ? local->get()
                  : addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                         .kind = persistence::ListKind::scratch,
                                                         .name = "Local Queue",
                                                         .pinned = false,
                                                         .dirty = false,
                                                         .items = {},
                                                         .remote = false},
                               true);
        tabs_->setCurrentWidget(tab->view);
    }
    startDiscovery(std::move(raw_paths), QString::fromStdString(tab->document.id.to_string()), -1);
}

void BenchMainWindow::startDiscovery(std::vector<std::string> raw_paths, QString target_document_id,
                                     const int insertion_row, const bool replace_and_play) {
    if (discovery_running_) {
        statusBar()->showMessage(QStringLiteral("A folder scan is already running"), 3'000);
        return;
    }
    discovery_running_ = true;
    discovery_target_document_ = std::move(target_document_id);
    discovery_insertion_row_ = insertion_row;
    const auto* target = tabForDocument(discovery_target_document_);
    discovery_insertion_anchor_ = target ? target->model->index(insertion_row, 0) : QModelIndex{};
    discovery_anchored_ = discovery_insertion_anchor_.isValid();
    discovery_replace_and_play_ = replace_and_play;
    connect(&discovery_watcher_, &QFutureWatcher<DiscoveryOutcome>::finished, this,
            &BenchMainWindow::finishDiscovery, Qt::SingleShotConnection);
    discovery_watcher_.setFuture(QtConcurrent::run([paths = std::move(raw_paths),
                                                    cancellation = probe_cancellation_.token()] {
        auto discovered =
            core::discover_local_sources(std::span{paths.data(), paths.size()}, cancellation,
                                         discovery_row_limit, std::span{audio_extensions});
        DiscoveryOutcome outcome{.rows = {},
                                 .issues = std::move(discovered.issues),
                                 .cancelled = discovered.cancelled,
                                 .truncated = discovered.truncated};
        std::vector<std::optional<formats::ResolvedCueSheet>> resolved_cues(
            discovered.raw_files.size());
        std::unordered_set<std::string> cue_sources;
        std::unordered_map<std::string, metadata::LocalMetadataRead> cue_metadata;

        // Resolve cues first so an audio file referenced by a successfully
        // expanded sheet is not also inserted as one duplicate whole-file
        // row when both came from the same intake batch.
        for (std::size_t index = 0U; index < discovered.raw_files.size(); ++index) {
            if (cancellation.is_cancellation_requested()) {
                outcome.cancelled = true;
                break;
            }
            const auto& raw_path = discovered.raw_files[index];
            if (!is_cue_path(raw_path)) {
                continue;
            }
            auto resolved = formats::resolve_external_cue_sheet(raw_path, cancellation);
            if (!resolved) {
                outcome.issues.push_back(core::LocalSourceIssue{
                    .raw_path = raw_path, .error = std::move(resolved.error())});
                continue;
            }
            for (const auto& physical : resolved->physical_sources) {
                cue_sources.insert(physical);
                if (!cue_metadata.contains(physical)) {
                    if (auto read = metadata::read_local_metadata(physical, cancellation); read) {
                        cue_metadata.emplace(physical, std::move(*read));
                    }
                }
            }
            resolved_cues[index] = std::move(*resolved);
        }

        bool row_limit_reached = false;
        for (std::size_t index = 0U;
             index < discovered.raw_files.size() && !outcome.cancelled && !row_limit_reached;
             ++index) {
            const auto& raw_path = discovered.raw_files[index];
            if (is_cue_path(raw_path)) {
                if (!resolved_cues[index]) {
                    continue;
                }
                for (const auto& resolved_track : resolved_cues[index]->tracks) {
                    if (outcome.rows.size() == discovery_row_limit) {
                        outcome.truncated = true;
                        row_limit_reached = true;
                        outcome.issues.push_back(core::LocalSourceIssue{
                            .raw_path = raw_path,
                            .error =
                                core::Error{
                                    .code = core::ErrorCode::limit_exceeded,
                                    .message = "CUE expansion reached the local row limit",
                                    .context = {},
                                },
                        });
                        break;
                    }
                    const auto embedded = cue_metadata.find(resolved_track.raw_source_path);
                    if (embedded == cue_metadata.end()) {
                        outcome.rows.push_back(
                            cue_row(*resolved_cues[index], resolved_track, {}, std::nullopt));
                    } else {
                        outcome.rows.push_back(cue_row(*resolved_cues[index], resolved_track,
                                                       embedded->second.document,
                                                       embedded->second.source_revision));
                    }
                }
                continue;
            }

            std::error_code canonical_error;
            const auto canonical =
                std::filesystem::canonical(std::filesystem::path{raw_path}, canonical_error);
            if (!canonical_error && cue_sources.contains(canonical.native())) {
                continue;
            }
            if (outcome.rows.size() == discovery_row_limit) {
                outcome.truncated = true;
                row_limit_reached = true;
                outcome.issues.push_back(core::LocalSourceIssue{
                    .raw_path = raw_path,
                    .error =
                        core::Error{
                            .code = core::ErrorCode::limit_exceeded,
                            .message = "Local intake reached the row limit",
                            .context = {},
                        },
                });
                continue;
            }
            LocalTrackRow row;
            row.raw_path = raw_path;
            outcome.rows.push_back(std::move(row));
        }
        return outcome;
    }));
}

void BenchMainWindow::finishDiscovery() {
    discovery_running_ = false;
    auto result = discovery_watcher_.result();
    auto* tab = tabForDocument(discovery_target_document_);
    if (tab == nullptr || (discovery_anchored_ && !discovery_insertion_anchor_.isValid())) {
        return;
    }
    if (!result.rows.empty() && (!discovery_replace_and_play_ || !result.cancelled)) {
        if (discovery_replace_and_play_) {
            tab->model->replaceRows(std::move(result.rows), true);
            playRow(*tab, 0);
        } else {
            tab->model->appendRows(std::move(result.rows), discovery_anchored_
                                                               ? discovery_insertion_anchor_.row()
                                                               : discovery_insertion_row_);
        }
        markTabDirty(*tab);
        enqueueUnprobedRows(*tab);
        syncArtwork(*tab);
    }
    if (!result.issues.empty()) {
        statusBar()->showMessage(
            QStringLiteral("%1 entr%2 could not be opened")
                .arg(result.issues.size())
                .arg(result.issues.size() == 1U ? QStringLiteral("y") : QStringLiteral("ies")),
            5'000);
    }
    if (result.truncated) {
        statusBar()->showMessage(QStringLiteral("Folder scan hit the file limit"), 5'000);
    }
}

} // namespace trackknife::bench
