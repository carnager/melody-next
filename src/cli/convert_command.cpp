// SPDX-License-Identifier: GPL-3.0-only

#include "convert_command.hpp"

#include "trackknife/convert/preset.hpp"
#include "trackknife/convert/scan.hpp"
#include "trackknife/engine/file_work_wire.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/operations/output_path_plan.hpp"
#include "trackknife/protocol/message.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>

namespace trackknife::cli {
namespace {

using Json = nlohmann::json;

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "melody-cli: " << message << "\n";
    // On the main thread, the one that ends the process either way.
    std::exit(EXIT_FAILURE); // NOLINT(concurrency-mt-unsafe)
}

[[nodiscard]] Json call(protocol::Client& client, const std::string& method,
                        const Json& params = Json::object()) {
    auto answer = client.call(method, params);
    if (!answer) {
        fail(method + ": " + answer.error().message);
    }
    return std::move(*answer);
}

[[nodiscard]] std::string text_of(const Json& value, const char* key) {
    const auto found = value.find(key);
    return found != value.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

// What a scan of a folder takes for music: what Melody's library would.
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

void usage() {
    std::cerr
        << "usage: melody-cli convert --to FOLDER [--layout NAME] [--preset ID] [--jobs N]\n"
           "                          [--gain none|track|album] [--no-replaygain] [--no-artwork]\n"
           "                          [--dry-run]\n"
           "                          (--since WHEN | PATH...)\n"
           "       melody-cli convert --presets | --layouts\n"
           "  Converts with Melody's converter on this computer: tags and cover carried\n"
           "  and checked, nothing left half-written. Files are named by the engine's\n"
           "  naming layout NAME (melody-cli convert --layouts), else keep their place\n"
           "  below the music folder. ReplayGain is measured afresh on what was converted\n"
           "  -- by the engine, so it must reach FOLDER at the same path; --no-replaygain\n"
           "  where it cannot. One already converted is passed over, so the same\n"
           "  command can run again, from cron, say:\n"
           "    melody-cli convert --to ~/Opus --since 1d\n";
}

} // namespace

std::optional<std::int64_t> since_seconds(const std::string& when) {
    if (when.empty()) {
        return std::nullopt;
    }
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    std::int64_t number = 0;
    const auto* end = when.data() + when.size();
    const auto [rest, error] = std::from_chars(when.data(), end, number);
    if (error == std::errc{} && rest == end) {
        return number;
    }
    if (error == std::errc{} && rest + 1 == end && number >= 0) {
        switch (*rest) {
        case 's':
            return now - number;
        case 'm':
            return now - (number * 60);
        case 'h':
            return now - (number * 3'600);
        case 'd':
            return now - (number * 86'400);
        case 'w':
            return now - (number * 604'800);
        default:
            return std::nullopt;
        }
    }
    for (const auto* shape :
         {"%Y-%m-%dT%H:%M:%S", "%Y-%m-%dT%H:%M", "%Y-%m-%d %H:%M", "%Y-%m-%d"}) {
        std::istringstream in{when};
        std::tm local{};
        local.tm_isdst = -1;
        in >> std::get_time(&local, shape);
        if (!in.fail() && in.peek() == std::char_traits<char>::eof()) {
            return static_cast<std::int64_t>(std::mktime(&local));
        }
    }
    return std::nullopt;
}

std::vector<std::string> album_track_paths(protocol::Client& client, const std::string& album_key) {
    const auto tracks = call(client, "catalogue.query",
                             Json{{"kind", 2},
                                  {"text", ""},
                                  {"album_key", album_key},
                                  {"offset", 0},
                                  {"limit", 5'000},
                                  {"fields", Json::array({"title"})}})
                            .value("entries", std::vector<Json>{});
    std::vector<std::string> paths;
    for (const auto& track : tracks) {
        if (auto raw = protocol::decode_raw_path(text_of(track, "key"))) {
            paths.push_back(std::move(*raw));
        }
    }
    return paths;
}

int convert_command(protocol::Client& client, const bool json,
                    const std::vector<std::string>& words) {
    std::string to;
    std::string layout_name;
    std::string preset_id{"opus-192"};
    std::optional<std::int64_t> since;
    std::size_t jobs = std::max(1U, std::thread::hardware_concurrency() / 2U);
    auto gain = convert::ConversionGainMode::none;
    bool artwork = true;
    bool replaygain = true;
    bool dry_run = false;
    std::vector<std::string> named;
    for (std::size_t at = 1; at < words.size(); ++at) {
        const auto& word = words[at];
        const auto value = [&]() -> const std::string& {
            if (at + 1 >= words.size()) {
                usage();
                fail(word + " wants a value");
            }
            return words[++at];
        };
        if (word == "--help" || word == "-h") {
            usage();
            return EXIT_SUCCESS;
        }
        if (word == "--presets") {
            for (const auto& preset : convert::builtin_encoder_presets()) {
                const auto here = convert::probe_encoder_preset(preset);
                std::cout << preset.id << "\t" << preset.display_name
                          << (here.available ? "" : "\t(not in this FFmpeg: " + here.detail + ")")
                          << "\n";
            }
            return EXIT_SUCCESS;
        }
        if (word == "--layouts") {
            for (const auto& layout :
                 call(client, "layouts.list").value("layouts", Json::array())) {
                if (auto saved = engine::wire::decode_saved_layout(layout)) {
                    std::cout << saved->profile.name << "\n";
                }
            }
            return EXIT_SUCCESS;
        }
        if (word == "--to") {
            to = value();
        } else if (word == "--layout") {
            layout_name = value();
        } else if (word == "--preset") {
            preset_id = value();
        } else if (word == "--jobs") {
            jobs = static_cast<std::size_t>(std::clamp(std::atoi(value().c_str()), 1, 64));
        } else if (word == "--since") {
            since = since_seconds(value());
            if (!since) {
                fail("--since wants Unix seconds, a date (2026-10-17, 2026-10-17T18:30) or a "
                     "time ago (90m, 12h, 3d, 2w), not " +
                     words[at]);
            }
        } else if (word == "--gain") {
            const auto& mode = value();
            gain = mode == "track"   ? convert::ConversionGainMode::track
                   : mode == "album" ? convert::ConversionGainMode::album
                   : mode == "none"  ? convert::ConversionGainMode::none
                                     : (fail("--gain is none, track or album"),
                                        convert::ConversionGainMode::none);
        } else if (word == "--no-artwork") {
            artwork = false;
        } else if (word == "--no-replaygain") {
            replaygain = false;
        } else if (word == "--dry-run") {
            dry_run = true;
        } else if (word.starts_with("--")) {
            usage();
            fail("convert does not know " + word);
        } else {
            named.push_back(word);
        }
    }
    if (to.empty() || (!since && named.empty())) {
        usage();
        fail("convert wants --to FOLDER, and --since WHEN or the paths to convert");
    }
    const auto preset = convert::find_encoder_preset(preset_id);
    if (!preset) {
        fail("there is no preset " + preset_id + " (melody-cli convert --presets)");
    }
    if (const auto here = convert::probe_encoder_preset(*preset); !here.available) {
        fail(preset->display_name + " cannot be made by this computer's FFmpeg: " + here.detail);
    }
    std::error_code made;
    std::filesystem::create_directories(to, made);
    const auto root = std::filesystem::weakly_canonical(to).lexically_normal().native();

    // What to convert: the tracks of the albums added since, and the files
    // named or in the folders named.
    std::vector<std::string> sources;
    if (since) {
        for (std::size_t offset = 0;;) {
            auto answer = call(client, "catalogue.query",
                               Json{{"kind", 1},
                                    {"text", ""},
                                    {"offset", offset},
                                    {"limit", 200},
                                    {"newest_first", true},
                                    {"fields", Json::array({"added"})}});
            const auto albums = answer.value("entries", std::vector<Json>{});
            bool older = false;
            for (const auto& album : albums) {
                if (album.value("added", std::int64_t{0}) <= *since) {
                    older = true;
                    break;
                }
                for (auto& path : album_track_paths(client, text_of(album, "key"))) {
                    sources.push_back(std::move(path));
                }
            }
            offset += albums.size();
            if (older || albums.empty() || !answer.value("more", false)) {
                break;
            }
        }
    }
    for (const auto& name : named) {
        std::error_code looked;
        if (std::filesystem::is_directory(name, looked)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(name, looked)) {
                if (entry.is_regular_file() && audio_file(entry.path())) {
                    sources.push_back(std::filesystem::absolute(entry.path()).native());
                }
            }
        } else {
            sources.push_back(std::filesystem::absolute(name).native());
        }
    }
    std::ranges::sort(sources);
    sources.erase(std::ranges::unique(sources).begin(), sources.end());
    if (sources.empty()) {
        return EXIT_SUCCESS;
    }

    // Named by a layout of the engine's, or in their place below the music
    // folder they are in.
    std::optional<operations::OutputLayoutProfile> layout;
    if (!layout_name.empty()) {
        for (const auto& listed : call(client, "layouts.list").value("layouts", Json::array())) {
            if (auto saved = engine::wire::decode_saved_layout(listed);
                saved && saved->profile.name == layout_name) {
                layout = saved->profile;
            }
        }
        if (!layout) {
            fail("the engine has no naming layout " + layout_name +
                 " (melody-cli convert --layouts)");
        }
    }
    std::vector<std::string> roots;
    if (!layout) {
        for (const auto& listed : call(client, "catalogue.roots").value("roots", Json::array())) {
            if (auto raw = protocol::decode_raw_path(text_of(listed, "path"))) {
                roots.push_back(std::filesystem::path{*raw}.lexically_normal().native());
            }
        }
    }
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

    // Read here, planned as the converter window plans them, a plan for
    // each music folder when mirroring.
    std::map<std::string, std::vector<operations::OutputPathPlanningItem>> groups;
    std::vector<metadata::MetadataDocument> documents;
    int problems = 0;
    for (const auto& path : sources) {
        auto read = metadata::read_local_metadata(path);
        if (!read) {
            std::cerr << "melody-cli: " << path << ": " << read.error().message << "\n";
            ++problems;
            continue;
        }
        const auto group = layout ? std::string{} : root_of(path);
        if (!layout && group.empty()) {
            std::cerr << "melody-cli: " << path
                      << ": not in a music folder of the engine's; name a --layout\n";
            ++problems;
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
        operations::OutputLayoutProfile used =
            layout ? *layout
                   : operations::OutputLayoutProfile{.schema_version = 1U,
                                                     .name = "mirror",
                                                     .dialect = {},
                                                     .relative_directory_expression = "%directory%",
                                                     .basename_expression = "%filename%",
                                                     .sanitization_policy = {"linux", 1U}};
        operations::ConvertedPublicationPolicy converted{
            .target_extension = preset->file_extension,
            .mirror_source_root_raw_path = layout ? std::nullopt : std::optional{group}};
        auto planned = operations::plan_output_paths(
            items, {.rename_files = true, .move_files = true}, std::move(used),
            operations::DestinationProfile{.schema_version = 1U,
                                           .name = "melody-cli",
                                           .root_raw_path = root,
                                           .containment_policy = {"lexical-beneath-root", 1U}},
            {}, {}, {}, converted);
        if (!planned) {
            fail(planned.error().message);
        }
        for (const auto& issue : planned->issues) {
            std::cerr << "melody-cli: " << issue.message << "\n";
            ++problems;
        }
        for (const auto& source : planned->sources) {
            std::error_code looked;
            if (std::filesystem::exists(source.target_raw_path, looked)) {
                ++passed;
                continue;
            }
            for (const auto item : source.item_indexes) {
                work.push_back(
                    convert::ConversionScanItem{.item_index = item,
                                                .source_raw_path = source.source_raw_path,
                                                .selection = {},
                                                .range = std::nullopt,
                                                .destination_raw_path = source.target_raw_path,
                                                .metadata = documents[item]});
            }
        }
    }
    if (dry_run) {
        for (const auto& item : work) {
            std::cout << item.source_raw_path << "\t" << item.destination_raw_path << "\n";
        }
        return problems == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    for (const auto& item : work) {
        std::error_code created;
        std::filesystem::create_directories(
            std::filesystem::path{item.destination_raw_path}.parent_path(), created);
    }
    auto result =
        convert::scan_conversion(work, {.preset = *preset,
                                        .maximum_parallelism = jobs,
                                        .target_sample_rate = std::nullopt,
                                        .sample_rate_cap = std::nullopt,
                                        .target_bit_depth = std::nullopt,
                                        .keep_source_bit_depth = false,
                                        .channel_policy = convert::ConversionChannelPolicy::keep,
                                        .gain_mode = gain,
                                        .gain_preamp_db = 0.0F,
                                        .carry_artwork = artwork});
    if (!result) {
        fail(result.error().message);
    }
    Json listed = Json::array();
    Json converted_paths = Json::array();
    for (const auto& item : result->items) {
        const bool converted = item.state == convert::ConversionScanState::converted;
        if (converted) {
            converted_paths.push_back(protocol::encode_raw_path(item.destination_raw_path));
        }
        if (json) {
            listed.push_back(
                Json{{"source", item.source_raw_path},
                     {"converted", converted ? Json(item.destination_raw_path) : Json()},
                     {"error", item.issue ? Json(item.issue->message) : Json()}});
        } else if (converted) {
            std::cout << item.destination_raw_path << "\n";
        } else {
            std::cerr << "melody-cli: " << item.source_raw_path << ": "
                      << (item.issue ? item.issue->message : std::string{"not converted"}) << "\n";
        }
        if (!converted) {
            ++problems;
        }
    }
    // The source's gain does not fit what a lossy encoder made of it, and the
    // converter carries none: measured again on the files made, by album, as
    // Melody's ReplayGain scan does -- Opus's in its R128 tags.
    Json gained;
    if (replaygain && gain == convert::ConversionGainMode::none && !converted_paths.empty()) {
        const auto count = converted_paths.size();
        auto outcome = client.run_job("replaygain.apply",
                                      Json{{"paths", std::move(converted_paths)},
                                           {"grouping", "album"},
                                           {"true_peak", false},
                                           {"sidecar", false}},
                                      [](const Json&) {});
        const auto* why = !outcome                     ? "the engine did not measure them"
                          : outcome->contains("error") ? "the engine could not measure them"
                                                       : nullptr;
        if (why != nullptr) {
            std::cerr << "melody-cli: ReplayGain: " << why << ": "
                      << (!outcome ? outcome.error().message
                                   : outcome->at("error").value("message", std::string{}))
                      << " -- does the engine reach " << root
                      << " at that path? Else --no-replaygain\n";
            ++problems;
        } else {
            gained = outcome->value("result", Json::object());
            const auto written = gained.value("written", std::size_t{0});
            if (gained.value("refused", false) || written < count) {
                std::cerr << "melody-cli: ReplayGain written to " << written << " of " << count
                          << " files";
                for (const auto& problem : gained.value("problems", Json::array())) {
                    if (problem.is_object()) {
                        std::cerr << "; " << problem.value("message", std::string{});
                    } else if (problem.is_string()) {
                        std::cerr << "; " << problem.get<std::string>();
                    }
                }
                std::cerr << "\n";
                ++problems;
            } else if (!json) {
                std::cerr << "melody-cli: ReplayGain measured and written for " << written
                          << " files\n";
            }
        }
    }
    if (json) {
        std::cout << Json{{"files", listed}, {"replaygain", gained}}.dump() << "\n";
    } else if (passed > 0U) {
        std::cerr << "melody-cli: " << passed << " converted before, passed over\n";
    }
    return problems == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace trackknife::cli
