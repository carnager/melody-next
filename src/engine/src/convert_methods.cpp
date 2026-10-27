// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/convert/preset.hpp"
#include "trackknife/convert/scan.hpp"
#include "trackknife/engine/job_methods.hpp"
#include "trackknife/engine/workspace.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/operations/output_path_plan.hpp"
#include "trackknife/protocol/message.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <map>
#include <thread>

namespace trackknife::engine {
namespace {

using protocol::Json;

[[nodiscard]] core::Error bad_params(std::string message, std::string member) {
    return core::Error{.code = core::ErrorCode::invalid_argument,
                       .message = std::move(message),
                       .context = {{.key = "param", .value = std::move(member)}}};
}

// What a folder holds for music: the formats the library takes.
[[nodiscard]] bool audio_file(const std::filesystem::path& path) {
    static const std::vector<std::string> extensions{
        ".flac", ".mp3", ".m4a",  ".mp4", ".aac", ".ogg", ".oga", ".opus", ".wv",
        ".wav",  ".aif", ".aiff", ".ape", ".mpc", ".tta", ".wma", ".dsf",  ".dff"};
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return std::ranges::contains(extensions, extension);
}

struct ConvertRequest {
    std::vector<std::string> paths;
    std::string to;
    // ADR-0339: the naming layout itself, as the asker holds it -- an engine
    // keeps none; none keeps the files' places below the music folder.
    std::optional<operations::OutputLayoutProfile> layout;
    convert::EncoderPreset preset;
    convert::ConversionGainMode gain{convert::ConversionGainMode::none};
    bool artwork{true};
    std::uint32_t artwork_edge{1000U};
    bool dry_run{false};
};

[[nodiscard]] core::Result<ConvertRequest> request_of(const Json& params) {
    ConvertRequest request;
    const auto paths = params.find("paths");
    if (paths == params.end() || !paths->is_array() || paths->empty() || paths->size() > 100'000) {
        return std::unexpected(bad_params("between 1 and 100000 paths are required", "paths"));
    }
    for (const auto& encoded : *paths) {
        auto raw = encoded.is_string() ? protocol::decode_raw_path(encoded.get<std::string>())
                                       : core::Result<std::string>{std::unexpected(core::Error{})};
        if (!raw) {
            return std::unexpected(bad_params("a path is not an encoded path", "paths"));
        }
        request.paths.push_back(std::move(*raw));
    }
    auto to = protocol::decode_raw_path(params.value("to", std::string{}));
    if (!to || !std::filesystem::path{*to}.is_absolute()) {
        return std::unexpected(bad_params("to is an absolute folder, an encoded path", "to"));
    }
    request.to = std::filesystem::path{*to}.lexically_normal().native();
    while (request.to.size() > 1U && request.to.ends_with('/')) {
        request.to.pop_back();
    }
    if (const auto layout = params.find("layout"); layout != params.end() && !layout->is_null()) {
        if (!layout->is_object() || !layout->contains("basename") ||
            !(*layout)["basename"].is_string() ||
            (layout->contains("directory") && !(*layout)["directory"].is_string())) {
            return std::unexpected(
                bad_params("layout is {directory, basename}, two expressions", "layout"));
        }
        operations::OutputLayoutProfile profile;
        profile.name = layout->value("name", std::string{"layout"});
        profile.relative_directory_expression = layout->value("directory", std::string{});
        profile.basename_expression = (*layout)["basename"].get<std::string>();
        if (profile.basename_expression.empty()) {
            return std::unexpected(bad_params("a layout names its files", "layout"));
        }
        request.layout = std::move(profile);
    }
    const auto preset =
        convert::find_encoder_preset(params.value("preset", std::string{"opus-192"}));
    if (!preset) {
        return std::unexpected(bad_params("there is no such preset (convert.presets)", "preset"));
    }
    if (const auto here = convert::probe_encoder_preset(*preset); !here.available) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::unsupported,
                        .message = preset->display_name +
                                   " cannot be made by this engine's FFmpeg: " + here.detail,
                        .context = {{.key = "param", .value = "preset"}}});
    }
    request.preset = *preset;
    const auto gain = params.value("gain", std::string{"none"});
    if (gain == "track") {
        request.gain = convert::ConversionGainMode::track;
    } else if (gain == "album") {
        request.gain = convert::ConversionGainMode::album;
    } else if (gain != "none") {
        return std::unexpected(bad_params("gain is none, track or album", "gain"));
    }
    request.artwork = params.value("artwork", true);
    // ADR-0335: the embedded-cover limit of the asker's Cover settings; 0
    // carries covers as they are.
    request.artwork_edge = params.value("artwork_edge", std::uint32_t{1000U});
    request.dry_run = params.value("dry_run", false);
    return request;
}

} // namespace

void register_convert_methods(protocol::Dispatcher& dispatcher) {
    dispatcher.on("convert.presets", [](const Json&) -> core::Result<Json> {
        auto presets = Json::array();
        for (const auto& preset : convert::builtin_encoder_presets()) {
            const auto here = convert::probe_encoder_preset(preset);
            presets.push_back(Json{{"id", preset.id},
                                   {"name", preset.display_name},
                                   {"extension", preset.file_extension},
                                   {"available", here.available},
                                   {"detail", here.detail}});
        }
        return Json{{"presets", std::move(presets)}};
    });
}

void register_convert_jobs(JobCatalog& jobs, LocalCatalogue& catalogue) {
    jobs.on("convert.apply", [&catalogue](const Json& params) -> core::Result<JobRegistry::Work> {
        auto request = request_of(params);
        if (!request) {
            return std::unexpected(std::move(request.error()));
        }
        auto layout = request->layout;
        std::vector<std::string> roots;
        if (!layout) {
            if (auto known = catalogue.roots()) {
                for (const auto& root : *known) {
                    roots.push_back(
                        std::filesystem::path{root.raw_path}.lexically_normal().native());
                }
            }
        }
        return [request = std::move(*request), layout = std::move(layout),
                roots = std::move(roots)](const core::CancellationToken& token,
                                          const JobRegistry::Reporter& report) {
            Json problems = Json::array();
            // The files, and the audio files in the folders.
            std::vector<std::string> sources;
            for (const auto& path : request.paths) {
                std::error_code looked;
                if (std::filesystem::is_directory(path, looked)) {
                    for (auto entry = std::filesystem::recursive_directory_iterator(
                             path, std::filesystem::directory_options::skip_permission_denied,
                             looked);
                         !looked && entry != std::filesystem::recursive_directory_iterator{};
                         entry.increment(looked)) {
                        if (entry->is_regular_file() && audio_file(entry->path())) {
                            sources.push_back(entry->path().native());
                        }
                    }
                } else {
                    sources.push_back(path);
                }
            }
            std::ranges::sort(sources);
            sources.erase(std::ranges::unique(sources).begin(), sources.end());
            const auto root_of = [&roots](const std::string& path) {
                std::string found;
                for (const auto& candidate : roots) {
                    if ((path.starts_with(candidate + "/") || candidate == "/") &&
                        candidate.size() > found.size()) {
                        found = candidate;
                    }
                }
                return found;
            };
            // Read here, and planned as the converter window plans
            // them: a plan for each music folder when mirroring.
            std::map<std::string, std::vector<operations::OutputPathPlanningItem>> groups;
            std::vector<metadata::MetadataDocument> documents;
            for (const auto& path : sources) {
                if (token.is_cancellation_requested()) {
                    return Json{{"error", Json{{"message", "stopped"}}}};
                }
                auto read = metadata::read_local_metadata(path, token);
                if (!read) {
                    problems.push_back(protocol::displayable_text(path) + ": " +
                                       read.error().message);
                    continue;
                }
                const auto group = layout ? std::string{} : root_of(path);
                if (!layout && group.empty()) {
                    problems.push_back(protocol::displayable_text(path) +
                                       ": not in a music folder; name a layout");
                    continue;
                }
                groups[group].push_back(
                    operations::OutputPathPlanningItem{.item_index = documents.size(),
                                                       .source_raw_path = path,
                                                       .source_revision = read->source_revision,
                                                       .final_metadata = read->document});
                documents.push_back(std::move(read->document));
            }
            std::vector<convert::ConversionScanItem> work;
            std::size_t passed = 0;
            for (auto& [group, items] : groups) {
                auto used = layout.value_or(
                    operations::OutputLayoutProfile{.schema_version = 1U,
                                                    .name = "mirror",
                                                    .dialect = {},
                                                    .relative_directory_expression = "%directory%",
                                                    .basename_expression = "%filename%",
                                                    .sanitization_policy = {"linux", 1U}});
                operations::ConvertedPublicationPolicy converted{
                    .target_extension = request.preset.file_extension,
                    .mirror_source_root_raw_path = layout ? std::nullopt : std::optional{group}};
                auto planned = operations::plan_output_paths(
                    items, {.rename_files = true, .move_files = true}, std::move(used),
                    operations::DestinationProfile{
                        .schema_version = 1U,
                        .name = "convert",
                        .root_raw_path = request.to,
                        .containment_policy = {"lexical-beneath-root", 1U}},
                    {}, token, {}, converted);
                if (!planned) {
                    return Json{{"error", Json{{"message", planned.error().message}}}};
                }
                for (const auto& issue : planned->issues) {
                    problems.push_back(issue.message);
                }
                for (const auto& source : planned->sources) {
                    std::error_code looked;
                    if (std::filesystem::exists(source.target_raw_path, looked)) {
                        ++passed;
                        continue;
                    }
                    for (const auto item : source.item_indexes) {
                        work.push_back(convert::ConversionScanItem{
                            .item_index = item,
                            .source_raw_path = source.source_raw_path,
                            .selection = {},
                            .range = std::nullopt,
                            .destination_raw_path = source.target_raw_path,
                            .metadata = documents[item]});
                    }
                }
            }
            Json files = Json::array();
            if (request.dry_run) {
                for (const auto& item : work) {
                    files.push_back(
                        Json{{"source", protocol::encode_raw_path(item.source_raw_path)},
                             {"target", protocol::encode_raw_path(item.destination_raw_path)},
                             {"converted", false},
                             {"error", nullptr}});
                }
                return Json{{"result", Json{{"files", std::move(files)},
                                            {"passed", passed},
                                            {"problems", std::move(problems)}}}};
            }
            for (const auto& item : work) {
                std::error_code created;
                std::filesystem::create_directories(
                    std::filesystem::path{item.destination_raw_path}.parent_path(), created);
            }
            const auto hardware = std::thread::hardware_concurrency();
            report(Json{{"completed", 0}, {"total", work.size()}});
            auto scanned = convert::scan_conversion(
                work,
                {.preset = request.preset,
                 .maximum_parallelism =
                     std::max<std::size_t>(1U, hardware == 0U ? 2U : hardware / 2U),
                 .target_sample_rate = std::nullopt,
                 .sample_rate_cap = std::nullopt,
                 .target_bit_depth = std::nullopt,
                 .keep_source_bit_depth = false,
                 .channel_policy = convert::ConversionChannelPolicy::keep,
                 .gain_mode = request.gain,
                 .gain_preamp_db = 0.0F,
                 .carry_artwork = request.artwork,
                 .lossy_artwork_edge = request.artwork_edge},
                [&report](const convert::ConversionScanProgress& progress) {
                    report(Json{{"completed", progress.completed_items},
                                {"total", progress.total_items}});
                },
                token);
            if (!scanned) {
                return Json{{"error", Json{{"message", scanned.error().message}}}};
            }
            for (const auto& item : scanned->items) {
                const bool made = item.state == convert::ConversionScanState::converted;
                files.push_back(
                    Json{{"source", protocol::encode_raw_path(item.source_raw_path)},
                         {"target", protocol::encode_raw_path(item.destination_raw_path)},
                         {"converted", made},
                         {"error", made || !item.issue ? Json() : Json(item.issue->message)}});
            }
            return Json{{"result", Json{{"files", std::move(files)},
                                        {"passed", passed},
                                        {"problems", std::move(problems)}}}};
        };
    });
}

} // namespace trackknife::engine
