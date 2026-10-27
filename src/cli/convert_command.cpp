// SPDX-License-Identifier: GPL-3.0-only

#include "convert_command.hpp"

#include "trackknife/core/engine_state.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "trackknife/protocol/message.hpp"

#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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

void usage() {
    std::cerr
        << "usage: melody-cli convert --to FOLDER [--layout NAME | --layout-expression EXPR]\n"
           "                          [--preset ID]\n"
           "                          [--gain none|track|album] [--no-replaygain] [--no-artwork]\n"
           "                          [--cover-size PIXELS] [--dry-run]\n"
           "                          (--since WHEN | PATH...)\n"
           "       melody-cli convert --presets | --layouts\n"
           "  The engine converts, on its machine, with Melody's converter: tags and\n"
           "  cover carried and checked, nothing left half-written. FOLDER and PATHs\n"
           "  are the engine's. Files are named by Melody's naming layout NAME (melody-cli\n"
           "  convert --layouts) or the path expression EXPR (folders/name), else keep\n"
           "  their place below the music folder; then\n"
           "  ReplayGain is measured afresh on what was made (--no-replaygain not to).\n"
           "  One already converted is passed over, so the same\n"
           "  command can run again, from cron, say:\n"
           "    melody-cli convert --to ~/Opus --since 1d\n";
}

// ADR-0339: Melody's naming layouts, as the window keeps them on this
// computer -- the one place they are.
[[nodiscard]] std::vector<persistence::SavedOutputLayoutProfile> melody_layouts() {
    const auto path = core::presets_database_path();
    std::error_code missing;
    if (!std::filesystem::exists(path, missing)) {
        fail("Melody keeps no naming layouts on this computer yet (" + path.string() +
             "); make one in Melody, or give --layout-expression");
    }
    auto presets = persistence::ListRepository::open(path);
    if (!presets) {
        fail("Melody's presets could not be read: " + presets.error().message);
    }
    auto layouts = presets->load_output_layout_profiles();
    if (!layouts) {
        fail("Melody's naming layouts could not be read: " + layouts.error().message);
    }
    return std::move(*layouts);
}

// A whole path expression -- `%albumartist%/%album%/%tracknumber% %title%` --
// as a layout: its folders, and the name after the last slash outside any
// function's brackets.
[[nodiscard]] Json layout_of_expression(const std::string& expression) {
    std::size_t depth = 0U;
    std::optional<std::size_t> last;
    for (std::size_t at = 0U; at < expression.size(); ++at) {
        const auto character = expression[at];
        if (character == '\\') {
            ++at;
        } else if (character == '(') {
            ++depth;
        } else if (character == ')' && depth > 0U) {
            --depth;
        } else if (character == '/' && depth == 0U) {
            last = at;
        }
    }
    if (!last) {
        return Json{{"name", "expression"}, {"directory", ""}, {"basename", expression}};
    }
    return Json{{"name", "expression"},
                {"directory", expression.substr(0U, *last)},
                {"basename", expression.substr(*last + 1U)}};
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

void print_layout_names(std::ostream& out) {
    for (const auto& saved : melody_layouts()) {
        out << saved.profile.name << "\n";
    }
}

int convert_command(protocol::Client& client, const bool json,
                    const std::vector<std::string>& words) {
    std::string to;
    std::string layout;
    std::string layout_expression;
    std::string preset{"opus-192"};
    std::string gain{"none"};
    std::optional<std::int64_t> since;
    bool artwork = true;
    // ADR-0335: a lossy copy's cover at most this on its longer side; 0 as it is.
    std::uint32_t cover_size = 1000U;
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
            for (const auto& listed :
                 call(client, "convert.presets").value("presets", Json::array())) {
                std::cout << text_of(listed, "id") << "\t" << text_of(listed, "name")
                          << (listed.value("available", false)
                                  ? std::string{}
                                  : "\t(not in the engine's FFmpeg: " + text_of(listed, "detail") +
                                        ")")
                          << "\n";
            }
            return EXIT_SUCCESS;
        }
        if (word == "--layouts") {
            print_layout_names(std::cout);
            return EXIT_SUCCESS;
        }
        if (word == "--to") {
            to = value();
        } else if (word == "--layout") {
            layout = value();
        } else if (word == "--layout-expression") {
            layout_expression = value();
        } else if (word == "--preset") {
            preset = value();
        } else if (word == "--gain") {
            gain = value();
        } else if (word == "--since") {
            since = since_seconds(value());
            if (!since) {
                fail("--since wants Unix seconds, a date (2026-10-17, 2026-10-17T18:30) or a "
                     "time ago (90m, 12h, 3d, 2w), not " +
                     words[at]);
            }
        } else if (word == "--no-artwork") {
            artwork = false;
        } else if (word == "--cover-size") {
            const auto& text = value();
            std::uint32_t parsed = 0U;
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (error != std::errc{} || end != text.data() + text.size()) {
                fail("--cover-size wants pixels, 0 for covers as they are, not " + text);
            }
            cover_size = parsed;
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
    // The engine's paths; relative ones are taken as from here, which is
    // the engine's machine when it is this one.
    const auto absolute = [](const std::string& path) {
        return std::filesystem::path{path}.is_absolute() ? path
                                                         : std::filesystem::absolute(path).native();
    };
    Json paths = Json::array();
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
                for (const auto& path : album_track_paths(client, text_of(album, "key"))) {
                    paths.push_back(protocol::encode_raw_path(path));
                }
            }
            offset += albums.size();
            if (older || albums.empty() || !answer.value("more", false)) {
                break;
            }
        }
    }
    for (const auto& path : named) {
        paths.push_back(protocol::encode_raw_path(absolute(path)));
    }
    if (paths.empty()) {
        return EXIT_SUCCESS;
    }
    Json params{{"paths", std::move(paths)}, {"to", protocol::encode_raw_path(absolute(to))},
                {"preset", preset},          {"gain", gain},
                {"artwork", artwork},        {"artwork_edge", cover_size},
                {"dry_run", dry_run}};
    if (!layout.empty() && !layout_expression.empty()) {
        fail("--layout or --layout-expression, not both");
    }
    if (!layout_expression.empty()) {
        params["layout"] = layout_of_expression(layout_expression);
    } else if (!layout.empty()) {
        const auto layouts = melody_layouts();
        const auto found = std::ranges::find_if(
            layouts, [&layout](const auto& saved) { return saved.profile.name == layout; });
        if (found == layouts.end()) {
            fail("Melody has no naming layout " + layout + " (melody-cli convert --layouts)");
        }
        params["layout"] = Json{{"name", found->profile.name},
                                {"directory", found->profile.relative_directory_expression},
                                {"basename", found->profile.basename_expression}};
    }
    const bool interactive = !json && ::isatty(STDERR_FILENO) == 1;
    auto outcome = client.run_job("convert.apply", params, [interactive](const Json& progress) {
        if (interactive) {
            std::cerr << "\r\033[Kconverting " << progress.value("completed", 0) << "/"
                      << progress.value("total", 0) << std::flush;
        }
    });
    if (interactive) {
        std::cerr << "\r\033[K";
    }
    if (!outcome) {
        fail(outcome.error().message);
    }
    if (const auto error = outcome->find("error"); error != outcome->end()) {
        fail(error->value("message", std::string{"the engine could not convert"}));
    }
    const auto result = outcome->value("result", Json::object());
    int problems = 0;
    for (const auto& problem : result.value("problems", Json::array())) {
        std::cerr << "melody-cli: " << (problem.is_string() ? problem.get<std::string>() : "")
                  << "\n";
        ++problems;
    }
    const auto decoded = [](const Json& encoded) {
        auto raw = protocol::decode_raw_path(encoded.is_string() ? encoded.get<std::string>()
                                                                 : std::string{});
        return raw ? *raw : std::string{};
    };
    Json made = Json::array();
    for (const auto& file : result.value("files", Json::array())) {
        const auto source = decoded(file.value("source", Json()));
        const auto target = decoded(file.value("target", Json()));
        if (dry_run) {
            if (!json) {
                std::cout << source << "\t" << target << "\n";
            }
        } else if (file.value("converted", false)) {
            made.push_back(file.value("target", Json()));
            if (!json) {
                std::cout << target << "\n";
            }
        } else {
            std::cerr << "melody-cli: " << source << ": "
                      << (file.value("error", Json()).is_string() ? file["error"].get<std::string>()
                                                                  : std::string{"not converted"})
                      << "\n";
            ++problems;
        }
    }
    // The converter carries no ReplayGain -- the source's does not fit what a
    // lossy encoder made of it -- so the engine measures what it made, by
    // album, as Melody's ReplayGain scan does: Opus's in its R128 tags.
    Json gained;
    if (replaygain && gain == "none" && !made.empty()) {
        const auto count = made.size();
        auto measured = client.run_job(
            "replaygain.apply",
            Json{{"paths", made}, {"grouping", "album"}, {"true_peak", false}, {"sidecar", false}},
            [](const Json&) {});
        if (!measured || measured->contains("error")) {
            std::cerr << "melody-cli: ReplayGain not measured: "
                      << (!measured ? measured.error().message
                                    : measured->at("error").value("message", std::string{}))
                      << "\n";
            ++problems;
        } else {
            gained = measured->value("result", Json::object());
            const auto written = gained.value("written", std::size_t{0});
            if (gained.value("refused", false) || written < count) {
                std::cerr << "melody-cli: ReplayGain written to " << written << " of " << count
                          << " files";
                for (const auto& problem : gained.value("problems", Json::array())) {
                    if (problem.is_object()) {
                        std::cerr << "; " << problem.value("message", std::string{});
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
        std::cout << Json{{"result", result}, {"replaygain", gained}}.dump() << "\n";
    } else if (const auto passed = result.value("passed", std::size_t{0}); passed > 0U) {
        std::cerr << "melody-cli: " << passed << " converted before, passed over\n";
    }
    return problems == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace trackknife::cli
