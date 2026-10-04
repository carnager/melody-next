// SPDX-License-Identifier: GPL-3.0-only

// ADR-0237: file work in the engine. What the engine measures crosses the
// protocol exactly, and a scan asked of the engine gives what the same scan
// gives run here.

#include "trackknife/engine/file_work_methods.hpp"
#include "trackknife/engine/file_work_wire.hpp"
#include "trackknife/engine/job_methods.hpp"
#include "trackknife/engine/job_registry.hpp"
#include "trackknife/engine/metadata_services.hpp"
#include "trackknife/engine/naming_methods.hpp"
#include "trackknife/engine/player.hpp"
#include "trackknife/engine/rating_tags.hpp"
#include "trackknife/engine/remote_file_work.hpp"
#include "trackknife/engine/server.hpp"
#include "trackknife/engine/workspace.hpp"
#include "trackknife/loudness/replaygain.hpp"
#include "trackknife/loudness/scan.hpp"
#include "trackknife/metadata/artwork.hpp"
#include "trackknife/metadata/artwork_write_plan.hpp"
#include "trackknife/metadata/draft_document.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/staged_patch.hpp"
#include "trackknife/metadata/staged_selection.hpp"
#include "trackknife/metadata/write_plan.hpp"
#include "trackknife/operations/artwork_apply.hpp"
#include "trackknife/operations/undo.hpp"
#include "trackknife/operations/output_path_plan.hpp"
#include "trackknife/operations/output_path_preflight.hpp"
#include "trackknife/operations/preparation_plan.hpp"
#include "trackknife/persistence/file_publication_journal.hpp"
#include "trackknife/persistence/musicbrainz_cache.hpp"
#include "trackknife/protocol/dispatch.hpp"
#include "trackknife/protocol/message.hpp"

#include <flacfile.h>
#include <id3v2tag.h>
#include <mp4file.h>
#include <mp4tag.h>
#include <mpegfile.h>
#include <popularimeterframe.h>
#include <sys/stat.h>
#include <textidentificationframe.h>
#include <xiphcomment.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace protocol = trackknife::protocol;
namespace engine = trackknife::engine;
namespace wire = trackknife::engine::wire;
namespace loudness = trackknife::loudness;
namespace core = trackknife::core;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

// Through text and back, as the wire carries it.
// For jobs that move nothing.
const engine::MoveFollower no_follow = [](const auto&, const auto*) -> core::Result<void> {
    return {};
};

[[nodiscard]] protocol::Json over_the_wire(const protocol::Json& value) {
    return protocol::Json::parse(value.dump());
}

[[nodiscard]] std::filesystem::path materialize(const std::filesystem::path& fixtures,
                                                const std::string& name,
                                                const std::filesystem::path& target) {
    std::ifstream input{fixtures / (name + ".b64")};
    std::string base64((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::erase(base64, '\n');
    const auto decoded = protocol::decode_raw_path(base64);
    require(decoded.has_value(), "a fixture decodes");
    std::ofstream output{target, std::ios::binary};
    output.write(decoded->data(), static_cast<std::streamsize>(decoded->size()));
    return target;
}

[[nodiscard]] bool same_double(const double left, const double right) {
    return (std::isnan(left) && std::isnan(right)) || left == right;
}

[[nodiscard]] bool same_optional(const std::optional<double>& left,
                                 const std::optional<double>& right) {
    return left.has_value() == right.has_value() && (!left || same_double(*left, *right));
}

void require_same(const loudness::LoudnessScanResult& left,
                  const loudness::LoudnessScanResult& right, const std::string_view what) {
    require(left.tracks.size() == right.tracks.size(), what);
    require(left.albums.size() == right.albums.size(), what);
    require(left.cancellation_requested == right.cancellation_requested, what);
    for (std::size_t index = 0; index < left.tracks.size(); ++index) {
        const auto& a = left.tracks[index];
        const auto& b = right.tracks[index];
        require(a.item_index == b.item_index && a.raw_path == b.raw_path && a.state == b.state &&
                    a.opus == b.opus && a.source_revision == b.source_revision &&
                    a.issue == b.issue && a.loudness.has_value() == b.loudness.has_value(),
                what);
        if (a.loudness) {
            require(same_double(a.loudness->integrated_lufs, b.loudness->integrated_lufs) &&
                        same_double(a.loudness->sample_peak, b.loudness->sample_peak) &&
                        same_optional(a.loudness->true_peak, b.loudness->true_peak),
                    what);
        }
    }
    for (std::size_t index = 0; index < left.albums.size(); ++index) {
        const auto& a = left.albums[index];
        const auto& b = right.albums[index];
        require(a.album_key == b.album_key && a.item_indexes == b.item_indexes &&
                    same_optional(a.integrated_lufs, b.integrated_lufs) &&
                    same_double(a.sample_peak, b.sample_peak) &&
                    same_optional(a.true_peak, b.true_peak) && a.issue == b.issue,
                what);
    }
}

void encodings_are_exact() {
    // ADR-0263: undo requests and outcomes as they travel.
    {
        namespace operations = trackknife::operations;
        const std::array requests{
            operations::UndoRequest{.kind = operations::UndoKind::publication,
                                    .journal_id = core::StableId::random()},
            operations::UndoRequest{.kind = operations::UndoKind::metadata,
                                    .journal_id = core::StableId::random()}};
        const auto decoded = wire::decode_undo_requests(wire::encode(requests));
        require(decoded && *decoded == std::vector(requests.begin(), requests.end()),
                "undo requests travel exactly");
        const std::array outcomes{
            operations::UndoOutcome{.request = requests[0], .issue = std::nullopt,
                                    .from_raw_path = std::string{"/a/\xff.flac"},
                                    .to_raw_path = "/b/c.flac"},
            operations::UndoOutcome{.request = requests[1],
                                    .issue = core::Error{.code = core::ErrorCode::conflict,
                                                         .message = "changed",
                                                         .context = {}},
                                    .from_raw_path = {}, .to_raw_path = {}}};
        const auto answered = wire::decode_undo_outcomes(wire::encode(outcomes));
        require(answered && *answered == std::vector(outcomes.begin(), outcomes.end()),
                "undo outcomes travel exactly");
    }
    const core::Error error{.code = core::ErrorCode::conflict,
                            .message = "changed since it was read",
                            .context = {{"path", "/music/a.flac"}, {"path", "/music/b.flac"}}};
    const auto decoded_error = wire::decode_error(over_the_wire(wire::encode(error)));
    require(decoded_error && *decoded_error == error, "an error round-trips, context in order");

    const core::LocalSourceRevision revision{.device = 2049,
                                             .inode = std::numeric_limits<std::uint64_t>::max(),
                                             .size = 123456789,
                                             .modification_time_seconds = -5,
                                             .modification_time_nanoseconds = 999999999};
    const auto decoded_revision = wire::decode_revision(over_the_wire(wire::encode(revision)));
    require(decoded_revision && *decoded_revision == revision, "a revision round-trips");

    // Raw path bytes that are not text, a segment and a subsong.
    const loudness::LoudnessScanItem item{
        .item_index = 7,
        .raw_path = std::string{"/music/\xff\xfe.flac"},
        .selection = {.stream_index = 1, .subsong_index = 3},
        .range = trackknife::formats::SampleRange{.start_sample = 44100, .end_sample = 88200},
        .album_key = std::string{"key\x01\xff"}};
    const auto decoded_item = wire::decode_scan_item(over_the_wire(wire::encode(item)));
    require(decoded_item && *decoded_item == item, "a scan item round-trips, bytes and all");

    // A track too short for a gated loudness has one of minus infinity.
    loudness::LoudnessScanResult result;
    result.tracks.push_back(loudness::LoudnessTrackScan{
        .item_index = 0,
        .raw_path = "/music/short.flac",
        .state = loudness::LoudnessScanState::analyzed,
        .loudness =
            loudness::TrackLoudness{.integrated_lufs = -std::numeric_limits<double>::infinity(),
                                    .sample_peak = 0.123456789012345678,
                                    .true_peak = std::numeric_limits<double>::quiet_NaN()},
        .opus = true,
        .source_revision = revision,
        .issue = std::nullopt});
    result.tracks.push_back(
        loudness::LoudnessTrackScan{.item_index = 1,
                                    .raw_path = "/music/gone.flac",
                                    .state = loudness::LoudnessScanState::failed,
                                    .loudness = std::nullopt,
                                    .opus = false,
                                    .source_revision = std::nullopt,
                                    .issue = error});
    result.albums.push_back(loudness::LoudnessAlbumScan{.album_key = "album",
                                                        .item_indexes = {0, 1},
                                                        .integrated_lufs = std::nullopt,
                                                        .sample_peak = 0.5,
                                                        .true_peak = std::nullopt,
                                                        .issue = error});
    result.cancellation_requested = true;
    const auto decoded_result = wire::decode_scan_result(over_the_wire(wire::encode(result)));
    require(decoded_result.has_value(), "a scan result decodes");
    require_same(result, *decoded_result, "a scan result round-trips exactly");

    // Malformed documents are refused, not guessed at.
    require(!wire::decode_revision(protocol::Json::array({1, 2, 3})),
            "a short revision is refused");
    require(!wire::decode_scan_item(protocol::Json{{"item_index", -1}}),
            "an item without its path is refused");
    require(!wire::decode_scan_result(protocol::Json{{"tracks", 3}, {"albums", {}}}),
            "tracks that are not a list are refused");
}

void the_engine_measures_as_this_process_would(const std::filesystem::path& directory,
                                               const std::filesystem::path& fixtures) {
    const auto flac = materialize(fixtures, "tagged-tone-flac", directory / "one.flac");
    const auto opus = materialize(fixtures, "loudness-tone-opus", directory / "two.opus");
    const std::vector<loudness::LoudnessScanItem> items{
        {.item_index = 0,
         .raw_path = flac.string(),
         .selection = {},
         .range = std::nullopt,
         .album_key = std::string{"album"}},
        {.item_index = 1,
         .raw_path = opus.string(),
         .selection = {},
         .range = std::nullopt,
         .album_key = std::string{"album"}},
        {.item_index = 2,
         .raw_path = (directory / "missing.flac").string(),
         .selection = {},
         .range = std::nullopt,
         .album_key = std::nullopt},
    };
    const loudness::LoudnessScanOptions options{.measure_true_peak = true,
                                                .maximum_parallelism = 2};

    std::mutex mutex;
    std::vector<protocol::Event> events;
    engine::JobRegistry registry{[&](const protocol::Event& event) {
        const std::lock_guard guard{mutex};
        events.push_back(event);
    }};
    engine::JobCatalog jobs;
    engine::LocalCatalogue catalogue{directory / "library.sqlite3"};
    engine::register_file_work_jobs(jobs, directory / "library.sqlite3", catalogue, no_follow);
    protocol::Dispatcher dispatcher;
    engine::register_job_methods(dispatcher, registry, jobs);

    auto encoded_items = protocol::Json::array();
    for (const auto& item : items) {
        encoded_items.push_back(wire::encode(item));
    }
    const protocol::Request submit{
        .id = 1,
        .method = "job.submit",
        .params = over_the_wire(protocol::Json{
            {"job", "loudness.scan"},
            {"params", {{"items", encoded_items}, {"options", wire::encode(options)}}}})};
    const auto answer = dispatcher.dispatch(submit);
    require(answer.result.has_value(), "a scan is submitted");
    const auto job_id = answer.result->at("job_id").get<std::string>();
    registry.wait_all();

    protocol::Json outcome;
    std::size_t progress_reports = 0;
    {
        const std::lock_guard guard{mutex};
        for (const auto& event : events) {
            if (event.name == "job.progress") {
                ++progress_reports;
            }
            if (event.name == "job.finished" && event.data.at("job_id") == job_id) {
                outcome = event.data.at("outcome");
            }
        }
    }
    require(progress_reports >= items.size(), "each measured item is reported");
    require(outcome.contains("result"), "the scan finishes with its result");
    const auto measured = wire::decode_scan_result(over_the_wire(outcome.at("result")));
    require(measured.has_value(), "which decodes");

    const auto here = loudness::scan_loudness(items, options);
    require(here.has_value(), "the same scan runs here");
    require_same(*here, *measured, "the engine's scan is the scan this process makes");
    require(measured->tracks[0].state == loudness::LoudnessScanState::analyzed &&
                measured->tracks[1].opus &&
                measured->tracks[2].state == loudness::LoudnessScanState::failed,
            "a FLAC and an Opus are measured and a missing file fails on its own");
    require(measured->albums.size() == 1U, "and the two share one album");

    // A request the engine cannot run fails at submit, not in a job.
    const protocol::Request bad{.id = 2,
                                .method = "job.submit",
                                .params =
                                    protocol::Json{{"job", "loudness.scan"},
                                                   {"params", {{"items", {{{"item_index", 0}}}}}}}};
    require(dispatcher.dispatch(bad).error.has_value(), "a malformed item is refused at submit");
    const protocol::Request none{
        .id = 3, .method = "job.submit", .params = protocol::Json{{"job", "loudness.scan"}}};
    require(dispatcher.dispatch(none).error.has_value(), "so is a scan of nothing");
}

void documents_are_exact() {
    namespace metadata = trackknife::metadata;
    metadata::MetadataDocument document;
    const std::array provenances{
        metadata::FieldProvenance::cached_snapshot, metadata::FieldProvenance::annotation,
        metadata::FieldProvenance::embedded,        metadata::FieldProvenance::stream,
        metadata::FieldProvenance::segment,         metadata::FieldProvenance::sidecar};
    for (const auto provenance : provenances) {
        document.fields.push_back(
            metadata::MetadataField{.canonical_name = "comment",
                                    .native_name = "COMMENT",
                                    .values = {"plain", std::string{"not text \xff\xfe"}, ""},
                                    .qualifier = {.language = "eng", .description = std::nullopt},
                                    .provenance = provenance});
    }
    document.unsupported_native_objects.push_back({.identity = "APIC:0"});
    const auto decoded = wire::decode_document(over_the_wire(wire::encode(document)));
    require(decoded && *decoded == document,
            "a document round-trips: every provenance, a value that is not text, an empty one");
    require(wire::encode_text("plain").is_string(), "text travels as text");
    require(wire::encode_text(std::string{"\xff"}).is_object(), "bytes travel as bytes");
}

void the_engine_reads_as_this_process_would(const std::filesystem::path& directory,
                                            const std::filesystem::path& fixtures) {
    namespace metadata = trackknife::metadata;
    std::vector<std::string> paths;
    for (const auto* name : {"tagged-tone-flac", "tagged-tone-mp3", "tagged-tone-opus",
                             "tagged-tone-m4a", "container-chapters-mka", "rf64-tone-wav"}) {
        paths.push_back(materialize(fixtures, name, directory / name).string());
    }
    paths.push_back((directory / "missing.flac").string());

    protocol::Dispatcher dispatcher;
    engine::register_file_work_methods(dispatcher);
    auto encoded = protocol::Json::array();
    for (const auto& path : paths) {
        encoded.push_back(protocol::encode_raw_path(path));
    }
    const auto answer = dispatcher.dispatch(protocol::Request{
        .id = 1, .method = "metadata.read", .params = over_the_wire({{"paths", encoded}})});
    require(answer.result.has_value(), "the engine reads");
    const auto files = over_the_wire(answer.result->at("files"));
    require(files.size() == paths.size(), "one answer per path, in order");

    bool read_one = false;
    bool failed_one = false;
    for (std::size_t index = 0; index < paths.size(); ++index) {
        const auto here = metadata::read_local_metadata(paths[index]);
        const auto& file = files[index];
        if (here) {
            require(file.contains("read"), "a file read here is read by the engine");
            const auto there = wire::decode_metadata_read(file.at("read"));
            require(there && *there == *here, "and exactly the same");
            read_one = true;
            continue;
        }
        failed_one = true;
        require(file.contains("error"), "a file that fails here fails there");
        const auto error = wire::decode_error(file.at("error"));
        require(error && error->code == here.error().code, "with the same kind of error");
        const auto revision = core::observe_local_source_revision(paths[index]);
        if (here.error().code == core::ErrorCode::unsupported && revision) {
            const auto there = wire::decode_revision(file.at("revision"));
            require(there && *there == *revision,
                    "and a file with no tags to read comes with its revision");
        } else {
            require(file.at("revision").is_null(), "a file that is not there has none");
        }
    }
    require(read_one && failed_one, "both a read and a failure are covered");

    auto too_many = protocol::Json::array();
    for (std::size_t index = 0; index <= engine::metadata_read_limit; ++index) {
        too_many.push_back(protocol::encode_raw_path(paths[0]));
    }
    require(dispatcher
                .dispatch(protocol::Request{
                    .id = 2, .method = "metadata.read", .params = {{"paths", too_many}}})
                .error.has_value(),
            "a read of too many paths is refused");
}

struct Jobs {
    std::mutex mutex;
    std::vector<protocol::Event> events;
    engine::LocalCatalogue catalogue;
    engine::Workspace workspace;
    std::unique_ptr<engine::Player> player{engine::Player::create_without_audio()};
    engine::JobRegistry registry;
    engine::JobCatalog catalog;
    protocol::Dispatcher dispatcher;
    engine::MoveFollower follow;

    explicit Jobs(const std::filesystem::path& database)
        : catalogue(database), workspace(engine::Workspace::open(database).value()),
          registry([this](const protocol::Event& event) { record(event); }),
          follow(engine::follow_moves(workspace, catalogue, player.get(),
                                      [this](const protocol::Event& event) { record(event); })) {
        engine::register_file_work_jobs(catalog, database, catalogue, follow);
        engine::register_job_methods(dispatcher, registry, catalog);
    }

    void record(const protocol::Event& event) {
        const std::lock_guard guard{mutex};
        events.push_back(event);
    }

    // Submits and waits: the outcome, or the submit's refusal.
    [[nodiscard]] protocol::Json run(const std::string& job, const protocol::Json& params) {
        const auto answer = dispatcher.dispatch(
            protocol::Request{.id = 1,
                              .method = "job.submit",
                              .params = over_the_wire({{"job", job}, {"params", params}})});
        if (answer.error) {
            return protocol::Json{{"refused", answer.error->message}};
        }
        const auto job_id = answer.result->at("job_id").get<std::string>();
        registry.wait_all();
        const std::lock_guard guard{mutex};
        for (const auto& event : events) {
            if (event.name == "job.finished" && event.data.at("job_id") == job_id) {
                return over_the_wire(event.data.at("outcome"));
            }
        }
        return protocol::Json{{"refused", "no finish"}};
    }
};

void the_engine_writes_what_was_previewed(const std::filesystem::path& directory,
                                          const std::filesystem::path& fixtures) {
    namespace metadata = trackknife::metadata;
    const auto flac = materialize(fixtures, "tagged-tone-flac", directory / "write.flac").string();
    const auto read = metadata::read_local_metadata(flac);
    require(read.has_value(), "the file to write is read");
    const std::array<std::string_view, 1> preferred{"title"};
    auto selection = metadata::StagedMetadataSelection::create(
        {metadata::StagedMetadataSource{.raw_path = flac,
                                        .source_revision = read->source_revision,
                                        .baseline = read->document}},
        preferred);
    require(selection.has_value(), "a selection of it");
    std::optional<std::size_t> title;
    for (std::size_t index = 0; index < selection->field_count(); ++index) {
        if (selection->field(index).canonical_name == "title") {
            title = index;
        }
    }
    require(title.has_value(), "with a title field");
    metadata::StagedMetadataPatchSet patches;
    require(patches.replace_values(*selection, 0, *title, {"Written by the engine"}).has_value(),
            "a title is staged");
    const auto plan = metadata::revalidate_metadata_write_plan(*selection, patches);
    require(plan && plan->ready(), "the preview is clean");

    const auto decoded = wire::decode_write_plan(over_the_wire(wire::encode(*plan)));
    require(decoded && *decoded == *plan, "the plan round-trips exactly");

    Jobs jobs{directory / "engine.sqlite3"};
    const auto outcome = jobs.run("metadata.apply", {{"plan", wire::encode(*plan)}});
    require(outcome.contains("result"), "the engine writes the plan");
    const auto result = wire::decode_apply_result(outcome.at("result"));
    require(result && result->committed_source_count() == 1U, "and commits the file");
    const auto again = wire::decode_apply_result(over_the_wire(wire::encode(*result)));
    require(again && *again == *result, "the result round-trips exactly");
    const auto written = metadata::read_local_metadata(flac);
    require(written && written->document.first_effective_value("title") ==
                           std::optional<std::string>{"Written by the engine"},
            "the file has what was previewed");

    // The same plan again: the file has changed since it was previewed, so the
    // engine refuses to write it, and the file keeps what it has.
    const auto stale = jobs.run("metadata.apply", {{"plan", wire::encode(*plan)}});
    const auto refused = wire::decode_apply_result(stale.at("result"));
    require(refused && refused->committed_source_count() == 0U &&
                refused->sources.front().state ==
                    trackknife::operations::MetadataApplySourceState::failed,
            "a file changed since the preview is not written");
    const auto kept = metadata::read_local_metadata(flac);
    require(kept && kept->source_revision == written->source_revision, "and is left as it is");

    // A preview with a blocking issue is refused before anything runs.
    auto blocked = *plan;
    blocked.sources.front().issues.push_back(metadata::MetadataWritePlanIssue{
        .kind = metadata::MetadataWritePlanIssueKind::source_changed,
        .error =
            core::Error{.code = core::ErrorCode::conflict, .message = "changed", .context = {}},
        .field_index = std::nullopt,
        .item_indexes = {0},
        .blocking = true});
    require(jobs.run("metadata.apply", {{"plan", wire::encode(blocked)}}).contains("refused"),
            "a plan with a blocking issue is refused at submit");
    require(jobs.run("metadata.apply", protocol::Json::object()).contains("refused"),
            "so is no plan at all");
    // A clean journal: nothing to recover, nothing to show the user.
    const auto recovery =
        engine::recover_file_work(directory / "engine.sqlite3", jobs.catalogue, jobs.follow);
    require(!recovery.error && recovery.recovered == 0U,
            "finished writes leave nothing to recover");
    protocol::Dispatcher methods;
    engine::register_file_work_methods(methods, directory / "engine.sqlite3", recovery);
    const auto interrupted = methods.dispatch(protocol::Request{
        .id = 9, .method = "metadata.interrupted", .params = protocol::Json::object()});
    require(interrupted.result && interrupted.result->at("interrupted").empty() &&
                interrupted.result->at("error").is_null(),
            "and no interrupted work is reported");
}

// What a client's file tools get: the same reads, scans and writes, done by an
// engine over a real connection.
// ADR-0237 stage 5: a path plan built here, looked at and published by the
// engine, with the engine's lists, queue and library following the file.
void the_engine_moves_what_was_previewed(const std::filesystem::path& directory,
                                         const std::filesystem::path& fixtures) {
    namespace metadata = trackknife::metadata;
    namespace operations = trackknife::operations;
    const auto root = directory / "moves";
    std::filesystem::create_directories(root / "sorted");
    const auto flac = materialize(fixtures, "tagged-tone-flac", root / "old.flac").string();
    const auto read = metadata::read_local_metadata(flac);
    require(read.has_value(), "the file to move is read");

    Jobs jobs{directory / "moves.sqlite3"};
    // A list and a queue that name it.
    const auto list_id = core::StableId::random();
    trackknife::persistence::EngineListItem item;
    item.raw_path = flac;
    require(jobs.workspace
                .save_engine_list(list_id, "Moves", trackknife::persistence::EngineListKind::saved,
                                  {item}, std::nullopt, 0)
                .has_value(),
            "a list names the file");
    engine::QueueEntry queued;
    queued.source.raw_path = flac;
    jobs.player->replace_queue({queued});

    const auto plan_for = [&](const std::string& source, const core::LocalSourceRevision& revision,
                              metadata::MetadataDocument document,
                              const operations::OutputPathOperationSelection selection,
                              std::string directory_expression) {
        const std::array items{
            operations::OutputPathPlanningItem{.item_index = 0U,
                                               .source_raw_path = source,
                                               .source_revision = revision,
                                               .final_metadata = std::move(document)}};
        auto planned = operations::plan_output_paths(
            items, selection,
            operations::OutputLayoutProfile{.schema_version = 1U,
                                            .name = "Title",
                                            .dialect = {},
                                            .relative_directory_expression =
                                                std::move(directory_expression),
                                            .basename_expression = "%title%",
                                            .sanitization_policy = {"linux", 1U}},
            selection.move_files ? std::optional{operations::DestinationProfile{
                                       .schema_version = 1U,
                                       .name = "Sorted",
                                       .root_raw_path = (root / "sorted").string(),
                                       .containment_policy = {"lexical-beneath-root", 1U}}}
                                 : std::nullopt);
        require(planned && planned->ready(), "the path plan is clean");
        return *planned;
    };

    // A move into a folder that is not there yet.
    const auto plan = plan_for(flac, read->source_revision, read->document,
                               {.rename_files = true, .move_files = true}, "New folder");
    const auto round = wire::decode_path_plan(over_the_wire(wire::encode(plan)));
    require(round && *round == plan, "the path plan round-trips exactly");
    const auto checked = jobs.run("paths.preflight", {{"plan", wire::encode(plan)}});
    require(checked.contains("result"), "the engine looks at its filesystem for the plan");
    const auto preflight = wire::decode_path_preflight(checked.at("result"));
    const auto here = operations::preflight_output_paths(plan);
    require(preflight && here && *preflight == *here, "and sees what this process sees");
    require(preflight->ready() && !preflight->sources.front().missing_directory_raw_paths.empty(),
            "a move into a folder still to be made");
    const auto target = preflight->sources.front().planned.target_raw_path;

    auto preparation = operations::assemble_preparation_plan(
        {.save_tags = false, .rename_files = true, .move_files = true, .replaygain = false}, 0U,
        std::nullopt, plan, *preflight);
    require(preparation && preparation->ready(), "the preparation is ready");
    const auto decoded = wire::decode_preparation_plan(over_the_wire(wire::encode(*preparation)));
    require(decoded && decoded->operations == preparation->operations &&
                decoded->output_paths == preparation->output_paths &&
                decoded->path_preflight == preparation->path_preflight &&
                decoded->issues == preparation->issues && !decoded->metadata,
            "and round-trips exactly");

    const auto moved = jobs.run("preparation.apply", {{"plan", wire::encode(*preparation)}});
    require(moved.contains("result"), "the engine moves the file");
    const auto result = wire::decode_publication_apply_result(moved.at("result"));
    require(result && result->committed_source_count() == 1U, "and commits it");
    const auto again = wire::decode_publication_apply_result(over_the_wire(wire::encode(*result)));
    require(again && *again == *result, "the result round-trips exactly");
    require(!std::filesystem::exists(flac) && std::filesystem::is_regular_file(target),
            "the file is where the preview said");

    const auto list = jobs.workspace.load_engine_list(list_id);
    require(list && *list && (*list)->items.front().raw_path == target,
            "the engine's list follows it");
    require(jobs.player->queue().front().source.raw_path == target, "and so does its queue");
    {
        const std::lock_guard guard{jobs.mutex};
        require(std::ranges::any_of(jobs.events,
                                    [&](const protocol::Event& event) {
                                        return event.name == "list.changed" &&
                                               event.data.at("id") == list_id.to_string();
                                    }),
                "and every client hears the list changed");
    }

    // The same preparation again: the file is not where it was previewed, so
    // nothing moves.
    const auto stale = jobs.run("preparation.apply", {{"plan", wire::encode(*preparation)}});
    const auto refused = wire::decode_publication_apply_result(stale.at("result"));
    require((refused && refused->committed_source_count() == 0U) || stale.contains("error"),
            "a file gone since the preview is not moved");
    require(std::filesystem::is_regular_file(target), "and the moved file stays");

    // Tags and a rename in one: the new name comes from the new title, and the
    // file is written once, at its new place.
    const auto current = metadata::read_local_metadata(target);
    require(current.has_value(), "the moved file is read");
    const std::array<std::string_view, 1> preferred{"title"};
    auto selection = metadata::StagedMetadataSelection::create(
        {metadata::StagedMetadataSource{.raw_path = target,
                                        .source_revision = current->source_revision,
                                        .baseline = current->document}},
        preferred);
    require(selection.has_value(), "a selection of it");
    std::optional<std::size_t> title;
    for (std::size_t index = 0; index < selection->field_count(); ++index) {
        if (selection->field(index).canonical_name == "title") {
            title = index;
        }
    }
    require(title.has_value(), "with a title field");
    metadata::StagedMetadataPatchSet patches;
    require(patches.replace_values(*selection, 0, *title, {"Tagged and renamed"}).has_value(),
            "a title is staged");
    auto tags = metadata::revalidate_metadata_write_plan(*selection, patches);
    require(tags && tags->ready(), "the tag preview is clean");
    const std::array<std::size_t, 1> first{0U};
    auto documents = metadata::materialize_metadata_draft(*selection, patches, first);
    require(documents.has_value(), "the new tags name the file");
    const auto renaming = plan_for(target, current->source_revision, documents->front(),
                                   {.rename_files = true, .move_files = false}, "");
    const auto renaming_checked = jobs.run("paths.preflight", {{"plan", wire::encode(renaming)}});
    const auto renaming_preflight = wire::decode_path_preflight(renaming_checked.at("result"));
    require(renaming_preflight && renaming_preflight->ready(), "the rename is clean");
    auto combined = operations::assemble_preparation_plan(
        {.save_tags = true, .rename_files = true, .move_files = false, .replaygain = false}, 0U,
        std::move(*tags), renaming, *renaming_preflight);
    require(combined && combined->ready(), "tags and rename make one preparation");
    const auto both = jobs.run("preparation.apply", {{"plan", wire::encode(*combined)}});
    const auto both_result = wire::decode_publication_apply_result(both.at("result"));
    require(both_result && both_result->committed_source_count() == 1U &&
                both_result->sources.front().published_metadata.has_value(),
            "the engine writes and renames it");
    const auto renamed = renaming_preflight->sources.front().planned.target_raw_path;
    require(renamed.ends_with("Tagged and renamed.flac") && !std::filesystem::exists(target),
            "under its new title");
    const auto written = metadata::read_local_metadata(renamed);
    require(written && written->document.first_effective_value("title") ==
                           std::optional<std::string>{"Tagged and renamed"},
            "with the tags that were previewed");
    const auto relisted = jobs.workspace.load_engine_list(list_id);
    require(relisted && *relisted && (*relisted)->items.front().raw_path == renamed,
            "the list follows again");
    require(jobs.player->queue().front().source.raw_path == renamed, "as does the queue");

    // Finished moves leave nothing to recover.
    const auto recovery =
        engine::recover_file_work(directory / "moves.sqlite3", jobs.catalogue, jobs.follow);
    require(!recovery.error && recovery.recovered == 0U, "finished moves leave nothing to recover");

    // One recovery could not settle is shown with where it was going.
    {
        auto journal = trackknife::persistence::SqliteFilePublicationJournal::open(directory /
                                                                                   "moves.sqlite3");
        require(journal.has_value(), "the engine's move journal opens");
        const auto id = core::StableId::random();
        const core::LocalSourceRevision revision{.device = 1U,
                                                 .inode = 2U,
                                                 .size = 3U,
                                                 .modification_time_seconds = 4,
                                                 .modification_time_nanoseconds = 5};
        require(
            journal
                ->create(operations::FilePublicationJournalRecord{
                    .id = id,
                    .state = operations::FilePublicationJournalState::planned,
                    .publication = operations::OutputPathPublicationKind::same_filesystem_rename,
                    .source_raw_path = "/music/original.flac",
                    .target_raw_path = "/music/renamed.flac",
                    .prepared_raw_path = {},
                    .expected_source_revision = revision,
                    .prepared_revision = std::nullopt,
                    .target_revision = std::nullopt,
                    .occurrence_indexes = {0U},
                    .planned_missing_directory_raw_paths = {},
                    .reverses_journal_id = std::nullopt,
                    .failure = std::nullopt,
                })
                .has_value(),
            "an interrupted move is journaled");
        require(journal
                    ->transition(
                        id,
                        operations::FilePublicationJournalTransition{
                            .expected_state = operations::FilePublicationJournalState::planned,
                            .state = operations::FilePublicationJournalState::needs_reconciliation,
                            .prepared_revision = std::nullopt,
                            .target_revision = std::nullopt,
                            .failure = core::Error{.code = core::ErrorCode::conflict,
                                                   .message = "Ambiguous rename topology",
                                                   .context = {}},
                        })
                    .has_value(),
                "and left for the user");
    }
    protocol::Dispatcher methods;
    engine::register_file_work_methods(methods, directory / "moves.sqlite3", {});
    const auto answer = methods.dispatch(
        protocol::Request{.id = 1, .method = "metadata.interrupted", .params = {}});
    require(answer.result.has_value(), "the engine says what it could not settle");
    const auto listed = answer.result->at("interrupted");
    require(listed.size() == 1U &&
                protocol::decode_raw_path(listed.front().at("target").get<std::string>()) ==
                    core::Result<std::string>{"/music/renamed.flac"} &&
                listed.front().at("message") == "Ambiguous rename topology",
            "an interrupted move, with where it was going");
}

// ADR-0237 stage 2: ratings copied into the files they rate, only when asked.
void ratings_go_into_tags_when_asked(const std::filesystem::path& directory,
                                     const std::filesystem::path& fixtures) {
    namespace metadata = trackknife::metadata;
    const auto music = directory / "rated";
    std::filesystem::create_directories(music);
    const auto flac = materialize(fixtures, "tagged-tone-flac", music / "rated.flac").string();
    const auto database = directory / "ratings.sqlite3";
    engine::LocalCatalogue catalogue{database};
    require(catalogue.prepare().has_value() && catalogue.add_root(music.string()).has_value(),
            "a library");
    trackknife::persistence::LibraryScanProgress progress;
    require(catalogue.scan({}, progress).has_value(), "scanned");
    trackknife::persistence::LibraryQuery lookup;
    lookup.kind = trackknife::persistence::LibraryEntryKind::track;
    lookup.raw_path = flac;
    lookup.limit = 1;
    const auto found = catalogue.query(lookup);
    require(found && found->entries.size() == 1U, "the track is in it");
    const auto hash = found->entries.front().rating_hash;
    require(!hash.empty(), "with a rating key");
    auto workspace = engine::Workspace::open(database);
    require(workspace.has_value(), "a workspace");
    const auto in_file = [&flac] {
        const auto read = metadata::read_local_metadata(flac);
        return read ? read->document.effective_values("FMPS_RATING")
                    : std::vector<std::string>{"unreadable"};
    };
    const auto rate = [&catalogue](engine::RatingTags& tags, const std::string& key,
                                   const unsigned rating) {
        require(catalogue.set_rating(key, false, rating).has_value(), "rated");
        tags.rated(key, false, rating);
        tags.wait_idle();
    };

    {
        engine::RatingTags tags{database, catalogue, *workspace};
        require(!tags.enabled(), "off until turned on");
        rate(tags, hash, 6);
        require(in_file().empty(), "so a rating stays in the engine");
        // Turned on: what is already rated is written.
        require(tags.set_enabled(true).has_value(), "turned on");
        tags.wait_idle();
        require(in_file() == std::vector<std::string>{"0.6"}, "the rating reaches the file");
        rate(tags, hash, 10);
        require(in_file() == std::vector<std::string>{"1.0"}, "and follows a change");
        // Album ratings stay in the engine.
        tags.rated(found->entries.front().album_rating_hash, true, 4);
        tags.wait_idle();
        require(in_file() == std::vector<std::string>{"1.0"}, "an album rating is not written");
    }
    {
        // Kept across restarts.
        engine::RatingTags tags{database, catalogue, *workspace};
        require(tags.enabled(), "still on after a restart");
        rate(tags, hash, 0);
        require(in_file().empty(), "unrated takes it out");
        // The library still knows the file under the same key.
        const auto again = catalogue.query(lookup);
        require(again && again->entries.size() == 1U && again->entries.front().rating_hash == hash,
                "and the file keeps its rating key");
        protocol::Dispatcher methods;
        engine::register_rating_tag_methods(methods, tags);
        const auto off = methods.dispatch(protocol::Request{
            .id = 1, .method = "ratings.set_tags", .params = {{"write_tags", false}}});
        require(off.result && off.result->at("write_tags") == false, "turned off by a client");
        rate(tags, hash, 3);
        require(in_file().empty(), "and nothing more is written");
        const auto asked =
            methods.dispatch(protocol::Request{.id = 2, .method = "ratings.tags", .params = {}});
        require(asked.result && asked.result->at("write_tags") == false &&
                    asked.result->at("pending") == 0,
                "as the engine says");
    }
}

// A rating another player put in a file is taken into the library.
void ratings_in_files_are_imported(const std::filesystem::path& directory,
                                   const std::filesystem::path& fixtures) {
    const auto music = directory / "imported";
    std::filesystem::create_directories(music);
    const auto flac = materialize(fixtures, "tagged-tone-flac", music / "imported.flac").string();
    const auto database = directory / "imports.sqlite3";
    engine::LocalCatalogue catalogue{database};
    require(catalogue.prepare().has_value(), "a library");
    // Tagged before the library knows the file, as by another player.
    require(engine::RatingTags::write(database, catalogue, flac, 8).value_or(false),
            "a rating is put in the file");
    require(catalogue.add_root(music.string()).has_value(), "its folder is added");
    trackknife::persistence::LibraryScanProgress progress;
    require(catalogue.scan({}, progress).has_value(), "scanned");
    trackknife::persistence::LibraryQuery lookup;
    lookup.kind = trackknife::persistence::LibraryEntryKind::track;
    lookup.raw_path = flac;
    lookup.limit = 1;
    const auto rating = [&catalogue, &lookup] {
        const auto found = catalogue.query(lookup);
        return found && found->entries.size() == 1U ? found->entries.front().rating : 99U;
    };
    const auto hash = [&catalogue, &lookup] {
        return catalogue.query(lookup)->entries.front().rating_hash;
    }();
    require(rating() == 8U, "the file's rating is the library's");

    // Cleared here: the same tag read again does not bring it back.
    require(catalogue.set_rating(hash, false, 0).has_value(), "cleared");
    require(catalogue.refresh({flac}).has_value(), "the file is read again");
    require(rating() == 0U, "and it stays cleared");

    // A library read before ratings were imported is caught up once, when
    // the engine starts: an unrated track takes the rating in its file.
    auto workspace = engine::Workspace::open(database);
    require(workspace.has_value(), "a workspace");
    {
        engine::RatingTags caught_up{database, catalogue, *workspace};
    }
    require(rating() == 8U, "caught up from what the library read");
    require(catalogue.set_rating(hash, false, 0).has_value(), "cleared again");
    {
        engine::RatingTags again{database, catalogue, *workspace};
    }
    require(rating() == 0U, "and only once");

    // Changed in the file by another player: that is taken.
    require(engine::RatingTags::write(database, catalogue, flac, 4).value_or(false),
            "the tag changes");
    require(rating() == 4U, "and the library follows it");
    // The library's own rating over a tag it already saw stays.
    require(catalogue.set_rating(hash, false, 9).has_value(), "rated here");
    require(catalogue.refresh({flac}).has_value(), "read again");
    require(rating() == 9U, "a rating made here is kept");
}

// Each format's rating where its players read it: FMPS_RATING, spelled as
// the specification has it in ID3v2 and MP4, and on MP3 Windows Media
// Player's POPM as well.
void ratings_are_written_where_players_read_them(const std::filesystem::path& directory,
                                                 const std::filesystem::path& fixtures) {
    namespace metadata = trackknife::metadata;
    const auto database = directory / "formats.sqlite3";
    engine::LocalCatalogue catalogue{database};
    require(catalogue.prepare().has_value(), "a library");

    const auto mp3 = materialize(fixtures, "tagged-tone-mp3", directory / "rated.mp3").string();
    require(engine::RatingTags::write(database, catalogue, mp3, 7).value_or(false),
            "an MP3 is rated");
    const auto read = metadata::read_local_metadata(mp3);
    require(read && read->document.first_effective_value("FMPS_RATING") ==
                        std::optional<std::string>{"0.7"},
            "with FMPS_RATING");
    require(read->popularimeter == std::optional<std::uint8_t>{186U}, "and POPM, 3.5 stars");
    {
        TagLib::MPEG::File file{mp3.c_str(), false};
        require(file.isValid() && file.hasID3v2Tag(), "the MP3 has ID3v2");
        const auto frames = file.ID3v2Tag()->frameList("TXXX");
        require(std::ranges::any_of(
                    frames,
                    [](auto* frame) {
                        const auto* user =
                            dynamic_cast<TagLib::ID3v2::UserTextIdentificationFrame*>(frame);
                        return user != nullptr &&
                               user->description() == TagLib::String{"FMPS_Rating"};
                    }),
                "the TXXX frame is named as the specification spells it");
        const auto popm = file.ID3v2Tag()->frameList("POPM");
        require(popm.size() == 1U &&
                    dynamic_cast<TagLib::ID3v2::PopularimeterFrame*>(popm.front())->email() ==
                        TagLib::String{"Windows Media Player 9 Series"},
                "and POPM is Windows Media Player's");
    }
    require(engine::RatingTags::write(database, catalogue, mp3, 0).value_or(false),
            "the MP3's rating is taken away");
    const auto cleared = metadata::read_local_metadata(mp3);
    require(cleared && cleared->document.effective_values("FMPS_RATING").empty() &&
                !cleared->popularimeter,
            "from both");

    const auto m4a = materialize(fixtures, "tagged-tone-m4a", directory / "rated.m4a").string();
    require(engine::RatingTags::write(database, catalogue, m4a, 4).value_or(false),
            "an M4A is rated");
    const auto m4a_read = metadata::read_local_metadata(m4a);
    require(m4a_read && m4a_read->document.first_effective_value("FMPS_RATING") ==
                            std::optional<std::string>{"0.4"},
            "with FMPS_RATING");
    TagLib::MP4::File m4a_file{m4a.c_str(), false};
    require(m4a_file.isValid() && m4a_file.tag() != nullptr &&
                m4a_file.tag()->contains("----:com.apple.iTunes:FMPS_Rating"),
            "in an atom named as the specification spells it");
}

// ADR-0245: a copy of the rating as its plain 0-10 number, in a tag the user
// names -- one of its own spelled exactly, an official one as each format
// writes it -- and gone with the rating.
void ratings_are_copied_into_a_backup_tag(const std::filesystem::path& directory,
                                          const std::filesystem::path& fixtures) {
    namespace metadata = trackknife::metadata;
    const auto database = directory / "backups.sqlite3";
    engine::LocalCatalogue catalogue{database};
    require(catalogue.prepare().has_value(), "a library");

    const auto flac = materialize(fixtures, "tagged-tone-flac", directory / "copied.flac").string();
    require(engine::RatingTags::write(database, catalogue, flac, 8, {}, "TRACKKNIFE_RATING")
                .value_or(false),
            "a FLAC is rated, with a copy");
    const auto read = metadata::read_local_metadata(flac);
    require(read && read->document.first_effective_value("FMPS_RATING") ==
                        std::optional<std::string>{"0.8"} &&
                read->document.first_effective_value("TRACKKNIFE_RATING") ==
                    std::optional<std::string>{"8"},
            "the rating, and its copy as 0-10");
    require(!engine::RatingTags::write(database, catalogue, flac, 8, {}, "TRACKKNIFE_RATING")
                 .value_or(true),
            "nothing to write when both are there");
    // Named after the rating was already written: only the copy is added.
    require(engine::RatingTags::write(database, catalogue, flac, 8, {}, "RATING_COPY")
                .value_or(false),
            "a new name gets the copy");
    require(metadata::read_local_metadata(flac)->document.first_effective_value("RATING_COPY") ==
                std::optional<std::string>{"8"},
            "under it");

    // An official tag, as MP3 writes it: COMMENT is its comment frame.
    const auto mp3 = materialize(fixtures, "tagged-tone-mp3", directory / "copied.mp3").string();
    require(engine::RatingTags::write(database, catalogue, mp3, 6, {}, "COMMENT").value_or(false),
            "an MP3 is rated, with a copy in its comment");
    {
        TagLib::MPEG::File file{mp3.c_str(), false};
        require(file.isValid() && file.hasID3v2Tag() &&
                    file.ID3v2Tag()->comment() == TagLib::String{"6"},
                "in the COMM frame players show");
    }
    require(engine::RatingTags::write(database, catalogue, mp3, 0, {}, "COMMENT").value_or(false),
            "unrated");
    const auto cleared = metadata::read_local_metadata(mp3);
    require(cleared && cleared->document.effective_values("FMPS_RATING").empty() &&
                cleared->document.effective_values("COMMENT").empty(),
            "the copy goes with the rating");

    // What cannot hold the copy is said, not tried.
    require(metadata::rating_backup_tag_problem("").has_value() &&
                metadata::rating_backup_tag_problem("FMPS_RATING").has_value() &&
                metadata::rating_backup_tag_problem("A=B").has_value() &&
                !metadata::rating_backup_tag_problem("TRACKKNIFE_RATING").has_value(),
            "names are checked");
    require(metadata::official_tag_name("COMMENT") && metadata::official_tag_name("Title") &&
                !metadata::official_tag_name("TRACKKNIFE_RATING"),
            "official names are known");
}

// POPM and plain RATING tags other players wrote come in too.
void other_players_ratings_are_imported(const std::filesystem::path& directory,
                                        const std::filesystem::path& fixtures) {
    const auto music = directory / "others";
    std::filesystem::create_directories(music);
    const auto mp3 = materialize(fixtures, "tagged-tone-mp3", music / "popm.mp3").string();
    const auto flac = materialize(fixtures, "tagged-tone-flac", music / "plain.flac").string();
    const auto set_popm = [&mp3](const int byte) {
        TagLib::MPEG::File file{mp3.c_str(), false};
        auto* tag = file.ID3v2Tag(true);
        const auto existing = TagLib::ID3v2::FrameList{tag->frameList("POPM")};
        for (auto* frame : existing) {
            tag->removeFrame(frame);
        }
        auto* popm = new TagLib::ID3v2::PopularimeterFrame;
        popm->setEmail("someone@elsewhere");
        popm->setRating(byte);
        tag->addFrame(popm);
        require(file.save(), "another player rates the MP3");
    };
    set_popm(196);
    {
        TagLib::FLAC::File file{flac.c_str(), false};
        file.xiphComment(true)->addField("RATING", "4");
        require(file.save(), "another player rates the FLAC, in stars");
    }
    const auto database = directory / "others.sqlite3";
    engine::LocalCatalogue catalogue{database};
    require(catalogue.prepare().has_value() && catalogue.add_root(music.string()).has_value(),
            "a library");
    trackknife::persistence::LibraryScanProgress progress;
    require(catalogue.scan({}, progress).has_value(), "scanned");
    const auto rating = [&catalogue](const std::string& path) {
        trackknife::persistence::LibraryQuery lookup;
        lookup.kind = trackknife::persistence::LibraryEntryKind::track;
        lookup.raw_path = path;
        lookup.limit = 1;
        const auto found = catalogue.query(lookup);
        return found && found->entries.size() == 1U ? found->entries.front().rating : 99U;
    };
    require(rating(mp3) == 8U, "a POPM of four stars is an 8");
    set_popm(54);
    require(catalogue.refresh({mp3}).has_value(), "the MP3 is read again");
    require(rating(mp3) == 3U, "and a change there is followed");
    require(rating(flac) == 0U, "a plain RATING is not read until its scale is known");
    auto workspace = engine::Workspace::open(database);
    require(workspace.has_value(), "a workspace");
    engine::RatingTags tags{database, catalogue, *workspace};
    require(tags.set_plain_scale(trackknife::metadata::PlainRatingScale::five).has_value(),
            "RATING is said to be stars");
    require(rating(flac) == 8U, "and four stars are taken as an 8");
}

// Naming layouts copied in whole; this engine's own move destinations; and
// its folders, to choose one.
void naming_and_destinations_are_the_engines(const std::filesystem::path& directory) {
    namespace operations = trackknife::operations;
    auto workspace = engine::Workspace::open(directory / "naming.sqlite3");
    require(workspace.has_value(), "a workspace");
    std::vector<protocol::Event> events;
    protocol::Dispatcher dispatcher;
    engine::register_naming_methods(
        dispatcher, *workspace,
        [&events](const protocol::Event& event) { events.push_back(event); });
    const auto call = [&dispatcher](const std::string& method, protocol::Json params) {
        return dispatcher.dispatch(
            protocol::Request{.id = 1, .method = method, .params = over_the_wire(params)});
    };
    const auto layout = [](std::string name, std::string basename) {
        return trackknife::persistence::SavedOutputLayoutProfile{
            .id = core::StableId::random(),
            .profile = operations::OutputLayoutProfile{.schema_version = 1U,
                                                       .name = std::move(name),
                                                       .dialect = {},
                                                       .relative_directory_expression = "%artist%",
                                                       .basename_expression = std::move(basename),
                                                       .sanitization_policy = {"linux", 1U}}};
    };
    auto first = layout("By title", "%title%");
    auto second = layout("Numbered", "$num(%tracknumber%,2) %title%");
    auto put = call("layouts.put", {{"layouts", {wire::encode(first), wire::encode(second)}}});
    require(put.result && put.result->at("layouts").size() == 2U, "the layouts are copied in");
    // A smaller set -- another client's, or one sent late -- takes nothing away.
    put = call("layouts.put", {{"layouts", {wire::encode(first)}}});
    require(put.result && put.result->at("layouts").size() == 2U, "a smaller set removes nothing");
    put = call("layouts.put", {{"layouts", protocol::Json::array()}});
    require(put.result && put.result->at("layouts").size() == 2U, "nor does an empty one");
    // A layout renamed to another's name is that layout now: the other gives way.
    second.profile.name = "By title";
    put = call("layouts.put", {{"layouts", {wire::encode(second)}}});
    require(put.result && put.result->at("layouts").size() == 1U, "a name is one layout's");
    const auto held = wire::decode_saved_layout(put.result->at("layouts").front());
    require(held && *held == second, "the renamed one, exactly");
    // Only what it is told to remove goes.
    put = call("layouts.put",
               {{"layouts", protocol::Json::array()}, {"removed", {second.id.to_string()}}});
    require(put.result && put.result->at("layouts").empty(), "a removed layout goes");
    require(call("layouts.set", {{"layouts", protocol::Json::array()}}).error.has_value(),
            "and the old replace-everything call is no more");

    const trackknife::persistence::SavedDestinationProfile destination{
        .id = core::StableId::random(),
        .profile =
            operations::DestinationProfile{.schema_version = 1U,
                                           .name = "Sorted",
                                           .root_raw_path = "/srv/music/sorted",
                                           .containment_policy = {"lexical-beneath-root", 1U}}};
    auto saved = call("destinations.save", {{"destination", wire::encode(destination)}});
    require(saved.result && saved.result->at("destinations").size() == 1U, "a destination is kept");
    require(!events.empty() && events.back().name == "destinations.changed",
            "and every client hears of it");
    auto removed = call("destinations.remove", {{"id", destination.id.to_string()}});
    require(removed.result && removed.result->at("destinations").empty(), "and forgotten");

    const auto root = directory / "browse";
    std::filesystem::create_directories(root / "b");
    std::filesystem::create_directories(root / "a");
    std::ofstream{root / "file.flac"} << "not a folder";
    std::filesystem::create_directory_symlink(root / "a", root / "link");
    const auto listed = call("folders.list", {{"path", protocol::encode_raw_path(root.string())}});
    require(listed.result.has_value(), "a folder is listed");
    std::vector<std::string> names;
    for (const auto& name : listed.result->at("folders")) {
        names.push_back(*protocol::decode_raw_path(name.get<std::string>()));
    }
    require(names == std::vector<std::string>{"a", "b"}, "its folders, in order, without links");
    require(protocol::decode_raw_path(listed.result->at("parent").get<std::string>()) ==
                core::Result<std::string>{directory.string()},
            "with its parent");
    require(
        call("folders.list", {{"path", protocol::encode_raw_path("relative")}}).error.has_value(),
        "only absolute paths");
    const auto top = call("folders.list", {{"path", protocol::encode_raw_path("/")}});
    require(top.result && top.result->at("parent").is_null(), "the root has no parent");
}

// ADR-0237: the ReplayGain window's round as one engine job, for a client
// with no tag library -- and the same gains the window would write.
void the_engine_writes_replaygain_in_one_job(const std::filesystem::path& directory,
                                             const std::filesystem::path& fixtures) {
    namespace metadata = trackknife::metadata;
    const auto folder = directory / "replaygain";
    std::filesystem::create_directories(folder);
    // Long enough for gated loudness (400 ms and more).
    const auto first =
        materialize(fixtures, "rich-metadata-long-flac", folder / "one.flac").string();
    const auto second =
        materialize(fixtures, "rich-metadata-long-flac", folder / "two.flac").string();

    // What the shared pipeline proposes for them here, before anything is written.
    std::vector<metadata::StagedMetadataSource> sources;
    for (const auto& path : {first, second}) {
        const auto read = metadata::read_local_metadata(path);
        require(read.has_value(), "a file to measure is read");
        sources.push_back(metadata::StagedMetadataSource{.raw_path = path,
                                                         .source_revision = read->source_revision,
                                                         .baseline = read->document});
    }
    const auto selection = metadata::StagedMetadataSelection::create(sources, {});
    require(selection.has_value(), "a selection of them");
    const std::vector<loudness::ReplayGainAudio> audio(2U);
    const auto expected = loudness::measure_replaygain(
        *selection, metadata::StagedMetadataPatchSet{}, {0U, 1U}, audio,
        loudness::ReplayGainSettings{
            .grouping = {.mode = loudness::LoudnessGroupingMode::selection_album, .expression = {}},
            .true_peak = false,
            .sidecar_only = false},
        {}, {});
    require(expected && expected->rows.size() == 2U, "measured here");

    Jobs jobs{directory / "replaygain.sqlite3"};
    require(jobs.run("replaygain.apply", protocol::Json::object()).contains("refused"),
            "paths are required");
    require(jobs.run("replaygain.apply",
                     {{"paths", {protocol::encode_raw_path(first)}}, {"grouping", "loudest"}})
                .contains("refused"),
            "and a grouping it knows");
    const auto outcome = jobs.run(
        "replaygain.apply",
        {{"paths", {protocol::encode_raw_path(first), protocol::encode_raw_path(second)}}});
    require(outcome.contains("result"), "the engine measures and writes");
    const auto& result = outcome.at("result");
    require(result.at("written") == 2 && result.at("failed") == 0 && !result.contains("refused"),
            "both files are written");
    require(result.at("tracks").size() == 2U &&
                result.at("tracks")[0].at("track_gain") == expected->rows[0].track_gain &&
                result.at("tracks")[0].at("album_gain") == expected->rows[0].album_gain,
            "with the gains measured here");
    for (std::size_t index = 0U; index < 2U; ++index) {
        const auto written = metadata::read_local_metadata(index == 0U ? first : second);
        require(written &&
                    written->document.first_effective_value("REPLAYGAIN_TRACK_GAIN") ==
                        std::optional{expected->rows[index].track_gain} &&
                    written->document.first_effective_value("REPLAYGAIN_ALBUM_GAIN") ==
                        std::optional{expected->rows[index].album_gain},
                "the files carry them, the album's the same for both");
    }
    {
        const std::lock_guard guard{jobs.mutex};
        const auto phases = std::ranges::count_if(jobs.events, [](const protocol::Event& event) {
            return event.name == "job.progress" &&
                   event.data.value("phase", std::string{}) == "writing";
        });
        require(phases > 0, "and says how far it got");
    }
    // Measured again, nothing has changed: nothing is written.
    const auto again = jobs.run(
        "replaygain.apply",
        {{"paths", {protocol::encode_raw_path(first), protocol::encode_raw_path(second)}}});
    require(again.contains("result") && again.at("result").at("written") == 0,
            "the same gains again write nothing");
}

void a_client_does_file_work_through_the_engine(const std::filesystem::path& directory,
                                                const std::filesystem::path& fixtures) {
    namespace metadata = trackknife::metadata;
    const auto flac = materialize(fixtures, "tagged-tone-flac", directory / "remote.flac").string();
    const auto socket = directory / "engine.sock";
    const auto database = directory / "remote.sqlite3";

    struct Relay {
        std::mutex mutex;
        engine::EventSink sink;
    } relay;
    engine::LocalCatalogue catalogue{database};
    engine::JobRegistry registry{[&relay](const protocol::Event& event) {
        const std::lock_guard guard{relay.mutex};
        if (relay.sink) {
            relay.sink(event);
        }
    }};
    engine::JobCatalog jobs;
    engine::register_file_work_jobs(jobs, database, catalogue, no_follow);
    protocol::Dispatcher dispatcher;
    engine::register_job_methods(dispatcher, registry, jobs);
    engine::register_file_work_methods(dispatcher, database, {});
    engine::MetadataServices services{database, directory};
    engine::register_metadata_service_jobs(jobs, services);
    engine::register_metadata_service_methods(dispatcher, services);
    engine::register_artwork_methods(dispatcher, directory / "staging");
    auto server = engine::Server::listen(socket, dispatcher);
    require(server.has_value(), "an engine listens");
    {
        const std::lock_guard guard{relay.mutex};
        relay.sink = (*server)->sink();
    }
    (*server)->start();

    engine::RemoteFileWork remote{
        protocol::Endpoint{.socket = socket, .host = {}, .port = 0, .token = {}}};
    require(remote.supported(), "an engine with file work says so");
    const auto access = remote.access();
    const auto here = metadata::read_local_metadata(flac);
    const auto there = access.read(flac, {});
    require(here && there && *here == *there,
            "a file read through the engine is the file read here");
    const auto revision = access.revision(flac);
    require(revision && *revision == here->source_revision, "and so is its revision");
    require(!access.read((directory / "absent.flac").string(), {}),
            "a file that is not there fails through the engine too");
    // A batch in one request, read at once on the engine: answered in order,
    // each file for itself, the missing one failing alone.
    const auto absent = (directory / "absent.flac").string();
    const auto many = access.read_many({flac, absent, flac}, {});
    require(many && many->size() == 3U && (*many)[0] && *(*many)[0] == *here &&
                !(*many)[1] && (*many)[2] && *(*many)[2] == *here,
            "a batch read through the engine is each file read here, in order");
    const auto probed_there = remote.probe(flac, {});
    const auto probed_here = engine::probe_local_technicals(flac, {});
    require(probed_there && probed_here && *probed_there == *probed_here &&
                probed_there->sample_rate > 0,
            "a file's technicals through the engine are its technicals here");
    require(!remote.probe((directory / "absent.flac").string(), {}), "and a missing file has none");

    const std::vector<loudness::LoudnessScanItem> items{{.item_index = 0,
                                                         .raw_path = flac,
                                                         .selection = {},
                                                         .range = std::nullopt,
                                                         .album_key = std::nullopt}};
    std::size_t reported = 0;
    const auto measured =
        remote.scan(items, {.measure_true_peak = false, .maximum_parallelism = 1},
                    [&reported](const loudness::LoudnessScanProgress&) { ++reported; }, {});
    const auto local =
        loudness::scan_loudness(items, {.measure_true_peak = false, .maximum_parallelism = 1});
    require(measured && local, "a scan runs through the engine and here");
    require_same(*local, *measured, "and measures the same");
    require(reported >= 1U, "reporting its progress");

    // A scan bigger than one job: several, each album whole in one, the
    // result back in the order given and the same as one scan here.
    {
        std::vector<loudness::LoudnessScanItem> several;
        for (std::size_t index = 0; index < 7U; ++index) {
            several.push_back({.item_index = 100U + index,
                               .raw_path = flac,
                               .selection = {},
                               .range = std::nullopt,
                               .album_key = index == 6U ? std::nullopt
                                                        : std::optional<std::string>{
                                                              index < 3U ? "album:a" : "album:b"}});
        }
        remote.limitJobs(900U);
        std::size_t last_completed = 0;
        const auto split = remote.scan(
            several, {.measure_true_peak = false, .maximum_parallelism = 1},
            [&last_completed](const loudness::LoudnessScanProgress& step) {
                last_completed = std::max(last_completed, step.completed_items);
            },
            {});
        const auto whole = loudness::scan_loudness(
            several, {.measure_true_peak = false, .maximum_parallelism = 1});
        require(split && whole, "a scan in several jobs runs");
        require_same(*whole, *split, "and measures what one scan does, albums included");
        require(last_completed == several.size(), "counting its progress across the jobs");
        remote.limitJobs(100U);
        const auto too_big = remote.scan(several, {.measure_true_peak = false,
                                                   .maximum_parallelism = 1},
                                         {}, {});
        require(!too_big && too_big.error().code == core::ErrorCode::limit_exceeded,
                "an album too large for one job is refused, not cut in two");
        remote.limitJobs(engine::remote_job_bytes);
    }

    // A plan built from what the engine read, written by the engine.
    const std::array<std::string_view, 1> preferred{"title"};
    auto selection = metadata::StagedMetadataSelection::create(
        {metadata::StagedMetadataSource{.raw_path = flac,
                                        .source_revision = there->source_revision,
                                        .baseline = there->document}},
        preferred);
    require(selection.has_value(), "a selection of what the engine read");
    std::size_t title = 0;
    for (std::size_t index = 0; index < selection->field_count(); ++index) {
        if (selection->field(index).canonical_name == "title") {
            title = index;
        }
    }
    metadata::StagedMetadataPatchSet patches;
    require(patches.replace_values(*selection, 0, title, {"Through the engine"}).has_value(),
            "a title is staged");
    const auto plan = metadata::build_metadata_write_plan(*selection, patches, access);
    require(plan && plan->ready(), "the plan is built through the engine");
    const auto bytes_of = [](const std::string& path) {
        std::ifstream input{path, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    };
    const auto original_bytes = bytes_of(flac);
    const auto applied = remote.apply(*plan, {}, {});
    require(applied && applied->committed_source_count() == 1U, "and written by it");
    const auto after = metadata::read_local_metadata(flac);
    require(after && after->document.first_effective_value("title") ==
                         std::optional<std::string>{"Through the engine"},
            "the file has it");

    // ADR-0263: undone through the engine, the file as it was.
    {
        const std::array undo{trackknife::operations::UndoRequest{
            .kind = trackknife::operations::UndoKind::metadata,
            .journal_id = applied->sources.front().commit->journal_id}};
        const auto undone = remote.undo(undo, {}, {});
        require(undone && undone->size() == 1U && !undone->front().issue &&
                    undone->front().to_raw_path == flac,
                "a write is undone through the engine");
        require(bytes_of(flac) == original_bytes, "the file is the original, byte for byte");
        const auto again = remote.undo(undo, {}, {});
        require(again && again->size() == 1U && again->front().issue.has_value(),
                "a write undone is not undone twice");
        // Written once more, for what follows.
        const auto rewritten = metadata::build_metadata_write_plan(
            *metadata::StagedMetadataSelection::create(
                {metadata::StagedMetadataSource{
                    .raw_path = flac,
                    .source_revision = metadata::read_local_metadata(flac)->source_revision,
                    .baseline = metadata::read_local_metadata(flac)->document}},
                preferred),
            patches, access);
        require(rewritten && rewritten->ready() && remote.apply(*rewritten, {}, {}),
                "and written again");
    }

    // A plan bigger than one job goes in parts, each file written once and
    // the results in the plan's order.
    {
        std::vector<std::string> copies;
        std::vector<metadata::StagedMetadataSource> sources;
        for (int index = 0; index < 3; ++index) {
            const auto copy = (directory / ("part-" + std::to_string(index) + ".flac")).string();
            std::filesystem::copy_file(flac, copy, std::filesystem::copy_options::overwrite_existing);
            const auto read = access.read(copy, {});
            require(read.has_value(), "a copy is read through the engine");
            copies.push_back(copy);
            sources.push_back({.raw_path = copy,
                               .source_revision = read->source_revision,
                               .baseline = read->document});
        }
        auto parts_selection = metadata::StagedMetadataSelection::create(sources, preferred);
        require(parts_selection.has_value(), "a selection of three files");
        std::size_t part_title = 0;
        for (std::size_t index = 0; index < parts_selection->field_count(); ++index) {
            if (parts_selection->field(index).canonical_name == "title") {
                part_title = index;
            }
        }
        metadata::StagedMetadataPatchSet part_patches;
        for (std::size_t item = 0; item < 3U; ++item) {
            require(part_patches
                        .replace_values(*parts_selection, item, part_title,
                                        {"Part " + std::to_string(item)})
                        .has_value(),
                    "a title for each");
        }
        const auto parts_plan =
            metadata::build_metadata_write_plan(*parts_selection, part_patches, access);
        require(parts_plan && parts_plan->ready() && parts_plan->sources.size() == 3U,
                "the plan reads all three through the engine in one batch");
        remote.limitJobs(1U);
        std::size_t last_completed = 0;
        const auto written = remote.apply(
            *parts_plan,
            [&last_completed](const trackknife::operations::MetadataApplyProgress& step) {
                last_completed = std::max(last_completed, step.completed_sources);
            },
            {});
        remote.limitJobs(engine::remote_job_bytes);
        require(written && written->committed_source_count() == 3U &&
                    written->sources.size() == 3U && written->sources[2].source_index == 2U &&
                    written->sources[2].raw_path == copies[2],
                "written in parts, the results in the plan's order");
        require(last_completed == 3U, "progress counted across the parts");
        for (std::size_t item = 0; item < 3U; ++item) {
            const auto now = metadata::read_local_metadata(copies[item]);
            require(now && now->document.first_effective_value("title") ==
                               std::optional<std::string>{"Part " + std::to_string(item)},
                    "each file has its own title");
        }
    }

    // The tagger's lookups through the engine: a cached answer comes back as
    // it was, a URL elsewhere is refused, the key is handed over.
    const std::string url = "https://musicbrainz.org/ws/2/release/remote?fmt=json";
    {
        auto cache = trackknife::persistence::SqliteMusicBrainzResponseCache::open(database);
        require(cache && cache->store(url, "{\"from\":\"engine\"}",
                                      static_cast<std::int64_t>(std::time(nullptr)),
                                      14LL * 24 * 60 * 60, 100U),
                "the engine has an answer cached");
    }
    const auto fetched = remote.fetch(url, {});
    require(fetched && *fetched == "{\"from\":\"engine\"}", "fetched through the engine");
    require(!remote.fetch("https://example.org/", {}), "a URL elsewhere is refused");
    require(remote.set_acoustid_key("handed-over").has_value() && services.has_acoustid_key(),
            "the AcoustID key is handed to the engine");
    // Artwork: what a file holds, an image handed over, a plan built from the
    // engine's reading and written by it.
    {
        namespace operations = trackknife::operations;
        const auto art = materialize(fixtures, "art-tone-flac", directory / "art.flac").string();
        const auto cover_path =
            materialize(fixtures, "external-blue-jpeg", directory / "cover.jpg");
        const auto remote_access = remote.artwork_access();
        const auto local_access = operations::local_artwork_file_access();
        const auto policy = metadata::default_artwork_inventory_policy();
        const auto art_there = remote_access.inventory(art, policy, {});
        const auto art_here = local_access.inventory(art, policy, {});
        require(art_there && art_here && *art_there == *art_here,
                "a file's pictures through the engine are its pictures");
        // An image over one part -- an original cover -- goes in parts and
        // arrives whole. Bytes after a JPEG's end are no part of the picture,
        // so a small one padded out stands for a large one.
        {
            std::ifstream input{cover_path, std::ios::binary};
            std::string large{std::istreambuf_iterator<char>{input},
                              std::istreambuf_iterator<char>{}};
            large.append(1'200'000U, '\0');
            const auto staged = remote.stage(std::span{
                reinterpret_cast<const unsigned char*>(large.data()), large.size()});
            require(staged && staged->byte_size == large.size() && staged->mime_type == "image/jpeg",
                    "a large image is staged in parts, whole");
            std::ifstream kept{staged->raw_path, std::ios::binary};
            const std::string back{std::istreambuf_iterator<char>{kept},
                                   std::istreambuf_iterator<char>{}};
            require(back == large, "byte for byte");
        }
        require(!art_here->items.empty(), "and it has one to replace");

        std::ifstream input{cover_path, std::ios::binary};
        const std::vector<unsigned char> cover((std::istreambuf_iterator<char>(input)),
                                               std::istreambuf_iterator<char>());
        const auto staged = remote.stage(cover);
        require(staged && staged->raw_path.starts_with((directory / "staging").string()),
                "an image handed over is kept where the engine writes from");
        const auto again = remote.stage(cover);
        require(again && *again == *staged,
                "the same image is kept once, as it was -- a plan naming it stays good");
        const auto bytes = remote_access.image_bytes(*staged, 1U << 20U, {});
        require(bytes && *bytes == cover, "and reads back as it was");
        require(remote_access.destination((directory / "none.jpg").string(), {}) &&
                    !*remote_access.destination((directory / "none.jpg").string(), {}),
                "an empty folder-image destination is empty");

        const auto& front = art_here->items.front();
        const std::vector<metadata::ArtworkWritePlanIntent> intents{
            metadata::ArtworkWritePlanIntent{.occurrence_index = 0,
                                             .raw_media_path = art,
                                             .expected_media_revision = art_here->media_revision,
                                             .target_ordinal = front.source_ordinal,
                                             .expected_target_fingerprint =
                                                 front.content_fingerprint,
                                             .kind = metadata::ArtworkWritePlanIntentKind::replace,
                                             .replacement_raw_path = staged->raw_path,
                                             .added_role = metadata::ArtworkRole::front,
                                             .added_description = {},
                                             .replacement_embedded_source = std::nullopt}};
        const metadata::ArtworkStoragePolicy storage{};
        const auto plan_there =
            operations::plan_artwork_storage(intents, storage, {}, {}, remote_access);
        const auto plan_here =
            operations::plan_artwork_storage(intents, storage, {}, {}, local_access);
        require(plan_there && plan_here && *plan_there == *plan_here && plan_there->ready(),
                "an artwork plan built through the engine is the plan built here");
        const auto round = wire::decode_artwork_plan(over_the_wire(wire::encode(*plan_there)));
        require(round && *round == *plan_there, "and round-trips exactly");

        // Inside a tag plan, as a combined Apply sends it.
        metadata::MetadataWritePlan combined;
        combined.sources.push_back(metadata::MetadataWritePlanSource{
            .raw_path = art,
            .occurrence_indexes = {0},
            .expected_revision = art_here->media_revision,
            .observed_revision = art_here->media_revision,
            .adapter_name = "taglib-flac-v1",
            .changes = {},
            .issues = {},
            .artwork = std::make_shared<const metadata::ArtworkWritePlanSource>(
                plan_there->sources.front())});
        const auto carried = wire::decode_write_plan(over_the_wire(wire::encode(combined)));
        require(carried && carried->sources.front().artwork &&
                    *carried->sources.front().artwork == plan_there->sources.front(),
                "a tag plan carries its artwork");

        const auto art_applied = remote.artwork_apply(*plan_there, {}, {});
        require(art_applied && art_applied->committed_source_count() == 1U,
                "the engine writes the picture");
        const auto art_after = local_access.inventory(art, policy, {});
        require(art_after && !art_after->items.empty() &&
                    art_after->items.front().content_fingerprint == staged->content_fingerprint,
                "and the file now holds the image handed over");
        const auto result_round =
            wire::decode_artwork_apply_result(over_the_wire(wire::encode(*art_applied)));
        require(result_round && *result_round == *art_applied, "its result round-trips exactly");
    }

    const auto printed_there = remote.fingerprint(flac, {});
    const auto printed_here = services.fingerprint(flac, {});
    require(printed_there.has_value() == printed_here.has_value() &&
                (!printed_there || printed_there->fingerprint == printed_here->fingerprint),
            "a fingerprint through the engine is the engine's own");
    (*server)->stop();

    // An engine older than file work: the tools do it themselves, as before.
    protocol::Dispatcher bare;
    auto old_socket = directory / "old.sock";
    auto old = engine::Server::listen(old_socket, bare);
    require(old.has_value(), "an old engine listens");
    (*old)->start();
    engine::RemoteFileWork older{
        protocol::Endpoint{.socket = old_socket, .host = {}, .port = 0, .token = {}}};
    require(!older.supported(), "an engine without file work says it has none");
    (*old)->stop();
}

// The tagger's lookups, made by the engine: nothing here goes to the
// network -- a cached answer is served as it is, and the rest is refused or
// local.
void the_engine_makes_the_taggers_lookups(const std::filesystem::path& parent) {
    const auto directory = parent / "services";
    std::filesystem::create_directories(directory);
    const auto database = directory / "services.sqlite3";
    engine::MetadataServices services{database, directory};

    // ADR-0261: a throttled answer is asked again, twice at most, never
    // after more than 30 s.
    {
        using Throttle = engine::MetadataServices::Throttle;
        const auto decide = engine::MetadataServices::throttle;
        require(decide(200, 0, 0) == Throttle{} && decide(404, 5, 0) == Throttle{},
                "an answer is taken, whatever it says");
        require(decide(429, 0, 0) == Throttle{.kind = Throttle::Kind::wait, .seconds = 2} &&
                    decide(503, 0, 1) == Throttle{.kind = Throttle::Kind::wait, .seconds = 4},
                "throttled without a word: 2 s, then 4");
        require(decide(429, 7, 0) == Throttle{.kind = Throttle::Kind::wait, .seconds = 7},
                "or as long as it asks");
        require(decide(429, 7, 2).kind == Throttle::Kind::give_up,
                "and not a third time");
        require(decide(503, 120, 0) == Throttle{.kind = Throttle::Kind::give_up, .seconds = 120},
                "nor after a wait too long to sit through");
    }
    require(
        engine::MetadataServices::fetchable("https://musicbrainz.org/ws/2/release/x?fmt=json") &&
            engine::MetadataServices::fetchable("https://coverartarchive.org/release/x"),
        "MusicBrainz and the Cover Art Archive are fetched");
    for (const auto* url : {"http://musicbrainz.org/ws/2/release/x", "https://example.org/",
                            "file:///etc/passwd", "https://musicbrainz.org.evil.example/ws/2/",
                            "https://coverartarchive.org.evil.example/"}) {
        require(!engine::MetadataServices::fetchable(url), url);
        require(!services.fetch(url, {}), "and fetch() refuses it too");
    }

    // An answer already cached is served without touching the network.
    const std::string url = "https://musicbrainz.org/ws/2/release/cached?fmt=json";
    const std::string cached = R"({"id":"cached","title":"From the cache"})";
    {
        auto cache = trackknife::persistence::SqliteMusicBrainzResponseCache::open(database);
        require(cache.has_value(), "the engine's cache opens");
        require(cache
                    ->store(url, cached, static_cast<std::int64_t>(std::time(nullptr)),
                            14LL * 24 * 60 * 60, 100U)
                    .has_value(),
                "and takes an answer");
    }
    const auto started = std::chrono::steady_clock::now();
    const auto served = services.fetch(url, {});
    require(served && *served == cached, "a cached answer is served as it was");
    require(std::chrono::steady_clock::now() - started < std::chrono::milliseconds{500},
            "at once, without waiting its turn");

    // Only answers are served from the cache: an archive's 404 page an older
    // engine kept for a release with no covers is not served as its listing
    // -- it is asked again (here, cancelled before reaching the network). An
    // image is bytes, and is served as it was kept.
    {
        const std::string listing = "https://coverartarchive.org/release/"
                                    "00000000-0000-0000-0000-000000000000";
        const std::string image = listing + "/1-250.jpg";
        auto cache = trackknife::persistence::SqliteMusicBrainzResponseCache::open(database);
        require(cache.has_value(), "the cache opens again");
        const auto now = static_cast<std::int64_t>(std::time(nullptr));
        require(cache->store(listing, "<!doctype html><title>404 Not Found</title>", now,
                             14LL * 24 * 60 * 60, 100U)
                        .has_value() &&
                    cache->store(image, "\xff\xd8\xff", now, 14LL * 24 * 60 * 60, 100U)
                        .has_value(),
                "a kept error page and a kept image");
        core::CancellationSource stop;
        stop.request_cancellation();
        const auto page = services.fetch(listing, stop.token());
        require(!page || !page->starts_with("<!doctype"), "an error page is not an answer");
        const auto bytes = services.fetch(image, stop.token());
        require(bytes && *bytes == "\xff\xd8\xff", "an image is served as it was kept");
    }

    // The AcoustID key is the engine's, readable by its owner only.
    require(!services.has_acoustid_key(), "no key to begin with");
    const auto keyless =
        services.acoustid_lookup({.duration_seconds = 10, .fingerprint = "AQAD"}, {});
    require(!keyless && keyless.error().code == core::ErrorCode::unsupported,
            "a lookup without a key says what to set");
    require(services.set_acoustid_key("client-key").has_value() && services.has_acoustid_key(),
            "a key is kept");
    struct stat kept{};
    require(::stat((directory / "acoustid.key").c_str(), &kept) == 0 && (kept.st_mode & 0077) == 0,
            "readable by its owner only");
    require(services.set_acoustid_key("").has_value() && !services.has_acoustid_key(),
            "and forgotten when emptied");

    // Its own files, fingerprinted here -- where fpcalc is installed.
    // Twelve seconds of changing tones: the fixtures are too short for
    // Chromaprint, which needs a few seconds of something to hear.
    const auto flac = (directory / "print.wav").string();
    {
        constexpr std::uint32_t rate = 22050;
        constexpr std::uint32_t seconds = 12;
        std::vector<std::int16_t> samples;
        samples.reserve(rate * seconds);
        std::uint32_t noise = 1;
        for (std::uint32_t index = 0; index < rate * seconds; ++index) {
            const double time = static_cast<double>(index) / rate;
            const double pitch = 220.0 * (1 + static_cast<int>(time * 2) % 5);
            noise = noise * 1664525U + 1013904223U;
            const double value = 0.4 * std::sin(2 * 3.14159265358979 * pitch * time) +
                                 0.05 * (static_cast<double>(noise >> 8) / 16777216.0 - 0.5);
            samples.push_back(static_cast<std::int16_t>(value * 32767));
        }
        const auto bytes = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
        std::ofstream wav{flac, std::ios::binary};
        const auto put32 = [&wav](const std::uint32_t value) {
            wav.write(reinterpret_cast<const char*>(&value), 4);
        };
        const auto put16 = [&wav](const std::uint16_t value) {
            wav.write(reinterpret_cast<const char*>(&value), 2);
        };
        wav.write("RIFF", 4);
        put32(36 + bytes);
        wav.write("WAVEfmt ", 8);
        put32(16);
        put16(1);
        put16(1);
        put32(rate);
        put32(rate * 2);
        put16(2);
        put16(16);
        wav.write("data", 4);
        put32(bytes);
        wav.write(reinterpret_cast<const char*>(samples.data()), bytes);
    }
    const auto printed = services.fingerprint(flac, {});
    if (printed) {
        require(printed->duration_seconds == 12U && !printed->fingerprint.empty(),
                "a file is fingerprinted by the engine");
    } else {
        require(printed.error().code == core::ErrorCode::unsupported,
                "or, with no fpcalc, says it is not installed");
    }
    trackknife::core::CancellationSource stop;
    stop.request_cancellation();
    require(!services.fingerprint(flac, stop.token()), "a cancelled fingerprint stops");

    // Over the protocol: a URL elsewhere is refused at submit.
    engine::JobCatalog jobs;
    engine::register_metadata_service_jobs(jobs, services);
    require(!jobs.build("musicbrainz.fetch", {{"url", "https://example.org/"}}),
            "musicbrainz.fetch refuses a URL elsewhere");
    require(jobs.build("musicbrainz.fetch", {{"url", url}}).has_value(), "and takes MusicBrainz");
    require(!jobs.build("acoustid.lookup", protocol::Json::object()),
            "acoustid.lookup needs a fingerprint");
}

} // namespace

int main(int argc, char** argv) {
    require(argc == 2, "usage: engine_file_work_test <fixture-dir>");
    const auto directory = std::filesystem::temp_directory_path() /
                           ("trackknife-file-work-" + core::StableId::random().to_string());
    std::filesystem::create_directories(directory);
    encodings_are_exact();
    documents_are_exact();
    the_engine_reads_as_this_process_would(directory, argv[1]);
    the_engine_writes_what_was_previewed(directory, argv[1]);
    the_engine_moves_what_was_previewed(directory, argv[1]);
    ratings_go_into_tags_when_asked(directory, argv[1]);
    ratings_in_files_are_imported(directory, argv[1]);
    ratings_are_written_where_players_read_them(directory, argv[1]);
    ratings_are_copied_into_a_backup_tag(directory, argv[1]);
    other_players_ratings_are_imported(directory, argv[1]);
    naming_and_destinations_are_the_engines(directory);
    the_engine_writes_replaygain_in_one_job(directory, argv[1]);
    a_client_does_file_work_through_the_engine(directory, argv[1]);
    the_engine_makes_the_taggers_lookups(directory);
    the_engine_measures_as_this_process_would(directory, argv[1]);
    std::filesystem::remove_all(directory);
    std::cout << "engine file work: ok\n";
    return EXIT_SUCCESS;
}
