// SPDX-License-Identifier: GPL-3.0-only

// ADR-0237: file work in the engine. What the engine measures crosses the
// protocol exactly, and a scan asked of the engine gives what the same scan
// gives run here.

#include "trackknife/engine/file_work_methods.hpp"
#include "trackknife/engine/file_work_wire.hpp"
#include "trackknife/engine/job_methods.hpp"
#include "trackknife/engine/job_registry.hpp"
#include "trackknife/loudness/scan.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/protocol/dispatch.hpp"
#include "trackknife/protocol/message.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
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
    const loudness::LoudnessScanItem item{.item_index = 7,
                                          .raw_path = std::string{"/music/\xff\xfe.flac"},
                                          .selection = {.stream_index = 1, .subsong_index = 3},
                                          .range = trackknife::formats::SampleRange{
                                              .start_sample = 44100, .end_sample = 88200},
                                          .album_key = std::string{"key\x01\xff"}};
    const auto decoded_item = wire::decode_scan_item(over_the_wire(wire::encode(item)));
    require(decoded_item && *decoded_item == item, "a scan item round-trips, bytes and all");

    // A track too short for a gated loudness has one of minus infinity.
    loudness::LoudnessScanResult result;
    result.tracks.push_back(loudness::LoudnessTrackScan{
        .item_index = 0,
        .raw_path = "/music/short.flac",
        .state = loudness::LoudnessScanState::analyzed,
        .loudness = loudness::TrackLoudness{.integrated_lufs =
                                                -std::numeric_limits<double>::infinity(),
                                            .sample_peak = 0.123456789012345678,
                                            .true_peak = std::numeric_limits<double>::quiet_NaN()},
        .opus = true,
        .source_revision = revision,
        .issue = std::nullopt});
    result.tracks.push_back(loudness::LoudnessTrackScan{.item_index = 1,
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
        {.item_index = 0, .raw_path = flac.string(), .selection = {}, .range = std::nullopt,
         .album_key = std::string{"album"}},
        {.item_index = 1, .raw_path = opus.string(), .selection = {}, .range = std::nullopt,
         .album_key = std::string{"album"}},
        {.item_index = 2, .raw_path = (directory / "missing.flac").string(), .selection = {},
         .range = std::nullopt, .album_key = std::nullopt},
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
    engine::register_file_work_jobs(jobs);
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
    const protocol::Request bad{
        .id = 2,
        .method = "job.submit",
        .params = protocol::Json{{"job", "loudness.scan"},
                                 {"params", {{"items", {{{"item_index", 0}}}}}}}};
    require(dispatcher.dispatch(bad).error.has_value(), "a malformed item is refused at submit");
    const protocol::Request none{
        .id = 3, .method = "job.submit", .params = protocol::Json{{"job", "loudness.scan"}}};
    require(dispatcher.dispatch(none).error.has_value(), "so is a scan of nothing");
}

void documents_are_exact() {
    namespace metadata = trackknife::metadata;
    metadata::MetadataDocument document;
    const std::array provenances{metadata::FieldProvenance::cached_snapshot,
                                 metadata::FieldProvenance::annotation,
                                 metadata::FieldProvenance::embedded,
                                 metadata::FieldProvenance::stream,
                                 metadata::FieldProvenance::segment,
                                 metadata::FieldProvenance::sidecar};
    for (const auto provenance : provenances) {
        document.fields.push_back(metadata::MetadataField{
            .canonical_name = "comment",
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

} // namespace

int main(int argc, char** argv) {
    require(argc == 2, "usage: engine_file_work_test <fixture-dir>");
    const auto directory = std::filesystem::temp_directory_path() /
                           ("trackknife-file-work-" + core::StableId::random().to_string());
    std::filesystem::create_directories(directory);
    encodings_are_exact();
    documents_are_exact();
    the_engine_reads_as_this_process_would(directory, argv[1]);
    the_engine_measures_as_this_process_would(directory, argv[1]);
    std::filesystem::remove_all(directory);
    std::cout << "engine file work: ok\n";
    return EXIT_SUCCESS;
}
