// SPDX-License-Identifier: GPL-3.0-only

#include "settings.hpp"

#include "melodyd_skeleton.hpp"

#include "trackknife/core/config_file.hpp"
#include "trackknife/core/environment.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iterator>
#include <utility>

namespace trackknife::daemon {
namespace {

using core::ConfigFile;

// ADR-0239's rates: 0 for the original files, else Opus at 16 to 512 kbps.
// The agent's own parse_kbps, here so melody-cli need not link the agent.
[[nodiscard]] std::optional<int> parse_kbps(const std::string_view text) {
    int kbps = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), kbps);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    if (kbps != 0 && (kbps < 16 || kbps > 512)) {
        return std::nullopt;
    }
    return kbps;
}

enum class Shape : std::uint8_t {
    value,
    // yes or no in the file; --NAME and --no-NAME on the command line.
    toggle,
};

struct Known {
    std::string_view key;
    Shape shape;
    // False for what only the command line has: --play-for and its options
    // are [engine NAME] sections in the file, --config names the file.
    bool in_file;
};

constexpr std::array known_keys{
    Known{"socket", Shape::value, true},
    Known{"state", Shape::value, true},
    Known{"listen", Shape::value, true},
    Known{"local-only", Shape::toggle, true},
    Known{"name", Shape::value, true},
    Known{"password", Shape::value, true},
    Known{"password-file", Shape::value, true},
    Known{"upnp", Shape::toggle, true},
    Known{"watch", Shape::toggle, true},
    Known{"upnp-interface", Shape::value, true},
    Known{"http", Shape::value, true},
    Known{"transcode-cache", Shape::value, true},
    Known{"music-root", Shape::value, true},
    Known{"agent", Shape::toggle, true},
    Known{"agent-password", Shape::value, true},
    Known{"agent-password-file", Shape::value, true},
    Known{"agent-music-root", Shape::value, true},
    Known{"agent-stream-home", Shape::value, true},
    Known{"agent-stream-away", Shape::value, true},
    Known{"play-for", Shape::value, false},
    Known{"play-for-name", Shape::value, false},
    Known{"play-for-password", Shape::value, false},
    Known{"play-for-password-file", Shape::value, false},
    Known{"play-for-music-root", Shape::value, false},
    Known{"play-for-bitrate-nearby", Shape::value, false},
    Known{"play-for-bitrate-away", Shape::value, false},
    Known{"config", Shape::value, false},
};

constexpr std::array section_keys{
    std::string_view{"address"},     std::string_view{"music-root"},
    std::string_view{"stream-home"}, std::string_view{"stream-away"},
    std::string_view{"password"},    std::string_view{"password-file"},
    std::string_view{"agent"}};

[[nodiscard]] const Known* known(const std::string_view key) {
    const auto* found = std::ranges::find(known_keys, key, &Known::key);
    return found == known_keys.end() ? nullptr : found;
}

[[nodiscard]] core::Error refused(std::string message) {
    return core::Error{
        .code = core::ErrorCode::invalid_argument, .message = std::move(message), .context = {}};
}

// What a key once was, for a file written with the old names.
[[nodiscard]] std::string renamed(const std::string_view key) {
    if (key == "agent-bitrate-nearby") {
        return "; it is agent-stream-home now (original, or opus 128)";
    }
    if (key == "agent-bitrate-away") {
        return "; it is agent-stream-away now (original, or opus 128)";
    }
    if (key.starts_with("play-for")) {
        return "; an engine played for is an [engine NAME] section now, with its address";
    }
    return {};
}

[[nodiscard]] std::string section_keys_list() {
    std::string list;
    for (const auto key : section_keys) {
        list += list.empty() ? "" : ", ";
        list += key;
    }
    return list;
}

[[nodiscard]] core::Result<bool> toggle_value(const std::string& where,
                                              const std::optional<std::string>& value) {
    if (!value || *value == "yes") {
        return true;
    }
    if (*value == "no") {
        return false;
    }
    return std::unexpected(refused(where + " is yes or no, got " + *value));
}

[[nodiscard]] core::Result<int> stream_value(const std::string& where, const std::string& value) {
    if (const auto kbps = parse_stream(value)) {
        return *kbps;
    }
    return std::unexpected(refused(where +
                                   " is original, or opus and a bit rate from 16 to 512 "
                                   "(opus 128), got " +
                                   value));
}

// One setting, from the file or a flag. `where` is how to name it in a
// refusal: "--transcode-cache", "melodyd.conf:4: transcode-cache".
[[nodiscard]] core::Result<void> apply(Settings& settings, const std::string_view key,
                                       const std::optional<std::string>& given,
                                       const std::string& where) {
    const auto value = given.value_or(std::string{});
    const auto play_for = [&settings]() -> PlayFor& {
        return settings.play_fors.empty() ? settings.play_for_defaults : settings.play_fors.back();
    };
    if (key == "socket") {
        settings.socket_path = value;
    } else if (key == "state") {
        settings.state_directory = value;
    } else if (key == "listen") {
        settings.listen_address = value;
    } else if (key == "local-only") {
        auto on = toggle_value(where, given);
        if (!on) {
            return std::unexpected(on.error());
        }
        settings.local_only = *on;
    } else if (key == "name") {
        settings.engine_name = value;
    } else if (key == "password") {
        settings.password = value;
        settings.password_file.clear();
    } else if (key == "password-file") {
        settings.password_file = value;
        settings.password.clear();
    } else if (key == "upnp") {
        auto on = toggle_value(where, given);
        if (!on) {
            return std::unexpected(on.error());
        }
        settings.upnp_enabled = *on;
    } else if (key == "watch") {
        auto on = toggle_value(where, given);
        if (!on) {
            return std::unexpected(on.error());
        }
        settings.watch_library = *on;
    } else if (key == "upnp-interface") {
        settings.upnp_interface = value;
        settings.upnp_enabled = true;
    } else if (key == "http") {
        settings.http_address = value;
    } else if (key == "transcode-cache") {
        std::uint64_t megabytes = 0;
        const auto* end = value.data() + value.size();
        const auto parsed = std::from_chars(value.data(), end, megabytes);
        if (parsed.ec != std::errc{} || parsed.ptr != end) {
            return std::unexpected(refused(where + " wants megabytes, got " + value));
        }
        settings.transcode_cache_mb = megabytes;
    } else if (key == "music-root") {
        settings.music_root = std::filesystem::path{value};
    } else if (key == "agent") {
        auto on = toggle_value(where, given);
        if (!on) {
            return std::unexpected(on.error());
        }
        settings.agent = *on;
    } else if (key == "agent-password") {
        settings.agent_password = value;
        settings.agent_password_file.clear();
    } else if (key == "agent-password-file") {
        settings.agent_password_file = value;
        settings.agent_password.clear();
    } else if (key == "agent-music-root") {
        settings.agent_music_root = std::filesystem::path{value};
    } else if (key == "agent-stream-home" || key == "agent-stream-away") {
        auto kbps = stream_value(where, value);
        if (!kbps) {
            return std::unexpected(kbps.error());
        }
        (key == "agent-stream-home" ? settings.agent_nearby_kbps : settings.agent_away_kbps) =
            *kbps;
    } else if (key == "play-for") {
        settings.play_fors.push_back(PlayFor{.address = value,
                                             .name = {},
                                             .password = {},
                                             .password_file = {},
                                             .music_root = {},
                                             .nearby_kbps = {},
                                             .away_kbps = {}});
    } else if (key == "play-for-name") {
        play_for().name = value;
    } else if (key == "play-for-password") {
        play_for().password = value;
    } else if (key == "play-for-password-file") {
        play_for().password_file = value;
    } else if (key == "play-for-music-root") {
        play_for().music_root = std::filesystem::path{value};
    } else if (key == "play-for-bitrate-nearby" || key == "play-for-bitrate-away") {
        const auto kbps = parse_kbps(value);
        if (!kbps) {
            return std::unexpected(
                refused(where + " wants 0 (the original files) or 16 to 512 kbps, got " + value));
        }
        (key == "play-for-bitrate-nearby" ? play_for().nearby_kbps : play_for().away_kbps) = *kbps;
    }
    return {};
}

[[nodiscard]] core::Result<ListedEngine> listed(const ConfigFile::Section& section,
                                                const std::string& origin) {
    const auto at = [&origin](const std::size_t line) {
        return origin + ":" + std::to_string(line);
    };
    ListedEngine engine{.name = section.name,
                        .address = {},
                        .password = {},
                        .password_file = {},
                        .music_root = {},
                        .nearby_kbps = {},
                        .away_kbps = {},
                        .agent = {},
                        .where = at(section.line)};
    if (section.kind != "engine") {
        return std::unexpected(refused(at(section.line) + ": [" + section.kind +
                                       "] is not a section melodyd.conf has; [engine NAME] is"));
    }
    if (section.name.empty()) {
        return std::unexpected(
            refused(at(section.line) + ": [engine] wants the name the engine goes by"));
    }
    bool password = false;
    bool password_file = false;
    for (const auto& entry : section.entries) {
        const auto where = at(entry.line) + ": " + entry.key;
        if (!std::ranges::contains(section_keys, entry.key)) {
            return std::unexpected(refused(at(entry.line) + ": " + entry.key +
                                           " is not a setting of an [engine]; those are " +
                                           section_keys_list()));
        }
        if (entry.key == "address") {
            engine.address = entry.value;
        } else if (entry.key == "music-root") {
            engine.music_root = std::filesystem::path{entry.value};
        } else if (entry.key == "stream-home" || entry.key == "stream-away") {
            auto kbps = stream_value(where, entry.value);
            if (!kbps) {
                return std::unexpected(kbps.error());
            }
            (entry.key == "stream-home" ? engine.nearby_kbps : engine.away_kbps) = *kbps;
        } else if (entry.key == "password") {
            engine.password = entry.value;
            password = true;
        } else if (entry.key == "password-file") {
            engine.password_file = entry.value;
            password_file = true;
        } else if (entry.key == "agent") {
            auto on = toggle_value(where, entry.value);
            if (!on) {
                return std::unexpected(on.error());
            }
            engine.agent = *on;
        }
        if (password && password_file) {
            return std::unexpected(refused(at(entry.line) +
                                           ": password and password-file both given; one is "
                                           "enough"));
        }
    }
    return engine;
}

[[nodiscard]] core::Result<void> apply_file(Settings& settings, const ConfigFile& file) {
    const auto& origin = file.origin();
    std::optional<std::size_t> password_line;
    std::optional<std::size_t> password_file_line;
    for (const auto& entry : file.top()) {
        const auto at = origin + ":" + std::to_string(entry.line);
        const auto* key = known(entry.key);
        if (key == nullptr || !key->in_file) {
            return std::unexpected(refused(
                at + ": " + entry.key + " is not a setting melodyd.conf has" + renamed(entry.key)));
        }
        if (entry.key == "password") {
            password_line = entry.line;
        } else if (entry.key == "password-file") {
            password_file_line = entry.line;
        }
        if (password_line && password_file_line) {
            return std::unexpected(refused(at + ": password and password-file both given; one is "
                                                "enough"));
        }
        if (auto applied = apply(settings, entry.key, entry.value, at + ": " + entry.key);
            !applied) {
            return applied;
        }
    }
    for (const auto& section : file.sections()) {
        auto engine = listed(section, origin);
        if (!engine) {
            return std::unexpected(engine.error());
        }
        settings.engines.push_back(std::move(*engine));
    }
    return {};
}

[[nodiscard]] std::optional<std::string> read_text(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        return std::nullopt;
    }
    return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] std::optional<std::string_view> options_value(std::string_view line) {
    line = trimmed(line);
    if (line.starts_with("export ")) {
        line = trimmed(line.substr(7));
    }
    constexpr std::string_view name{"MELODYD_OPTIONS="};
    if (!line.starts_with(name)) {
        return std::nullopt;
    }
    return line.substr(name.size());
}

// Words as a shell would split them, quotes grouping and going.
[[nodiscard]] std::vector<std::string> words(const std::string_view text) {
    std::vector<std::string> split;
    std::string word;
    bool in_word = false;
    char quote = 0;
    for (const char c : text) {
        if (quote != 0) {
            if (c == quote) {
                quote = 0;
            } else {
                word += c;
            }
        } else if (c == '"' || c == '\'') {
            quote = c;
            in_word = true;
        } else if (c == ' ' || c == '\t') {
            if (in_word) {
                split.push_back(std::move(word));
                word.clear();
                in_word = false;
            }
        } else {
            word += c;
            in_word = true;
        }
    }
    if (in_word) {
        split.push_back(std::move(word));
    }
    return split;
}

} // namespace

std::filesystem::path default_config_path() {
    if (const auto* config_home = core::environment_variable("XDG_CONFIG_HOME");
        config_home != nullptr && *config_home != '\0') {
        return std::filesystem::path{config_home} / "melody" / "melodyd.conf";
    }
    const auto* home = core::environment_variable("HOME");
    return std::filesystem::path{home == nullptr ? "." : home} / ".config" / "melody" /
           "melodyd.conf";
}

std::filesystem::path config_path(const std::span<const std::string_view> arguments) {
    for (std::size_t index = 0; index + 1 < arguments.size(); ++index) {
        if (arguments[index] == "--config") {
            return std::filesystem::path{std::string{arguments[index + 1]}};
        }
    }
    if (const auto* named = core::environment_variable("MELODYD_CONFIG");
        named != nullptr && *named != '\0') {
        return named;
    }
    return default_config_path();
}

std::string_view config_skeleton() { return melodyd_skeleton; }

std::optional<int> parse_stream(const std::string_view text) {
    if (text == "original") {
        return 0;
    }
    constexpr std::string_view opus{"opus"};
    if (!text.starts_with(opus)) {
        return std::nullopt;
    }
    const auto rate = trimmed(text.substr(opus.size()));
    if (rate.empty() || rate.size() == text.size() - opus.size()) {
        return std::nullopt;
    }
    const auto kbps = parse_kbps(rate);
    if (!kbps || *kbps == 0) {
        return std::nullopt;
    }
    return kbps;
}

std::string stream_text(const int kbps) {
    return kbps == 0 ? std::string{"original"} : "opus " + std::to_string(kbps);
}

bool is_options_file(const std::string_view text) {
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        if (options_value(text.substr(start, end == std::string_view::npos ? std::string_view::npos
                                                                           : end - start))) {
            return true;
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
}

core::Result<std::string> convert_options_file(const std::string_view text) {
    std::string kept;
    std::vector<std::string> flags;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        const auto line =
            text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
        if (auto value = options_value(line)) {
            // As systemd reads an environment file: quotes around the value
            // are the value's bounds, and $MELODYD_OPTIONS is then split.
            if (value->size() >= 2 && (value->front() == '"' || value->front() == '\'') &&
                value->back() == value->front()) {
                *value = value->substr(1, value->size() - 2);
            }
            auto parsed = words(*value);
            flags.insert(flags.end(), parsed.begin(), parsed.end());
        } else if (const auto rest = trimmed(line); !rest.empty()) {
            kept += rest.starts_with('#') ? std::string{rest} : "# " + std::string{rest};
            kept += '\n';
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }

    struct Group {
        std::string address;
        std::string name;
        std::vector<std::pair<std::string, std::string>> keys;
    };
    std::vector<std::pair<std::string, std::string>> top;
    std::vector<std::pair<std::string, std::string>> every_group;
    std::vector<Group> groups;
    const auto group_keys = [&]() -> std::vector<std::pair<std::string, std::string>>& {
        return groups.empty() ? every_group : groups.back().keys;
    };
    for (std::size_t index = 0; index < flags.size(); ++index) {
        const std::string_view flag = flags[index];
        if (!flag.starts_with("--")) {
            return std::unexpected(
                refused("MELODYD_OPTIONS has " + std::string{flag} + ", which is not an option"));
        }
        const auto key = flag.substr(2);
        const auto next = [&]() -> core::Result<std::string> {
            if (index + 1 >= flags.size()) {
                return std::unexpected(
                    refused("MELODYD_OPTIONS has " + std::string{flag} + " without its value"));
            }
            return flags[++index];
        };
        const auto stream = [&](const std::string& value) -> core::Result<std::string> {
            const auto kbps = parse_kbps(value);
            if (!kbps) {
                return std::unexpected(refused("MELODYD_OPTIONS has " + std::string{flag} + " " +
                                               value + ", which is not a bit rate"));
            }
            return stream_text(*kbps);
        };
        if (key == "agent" || key == "local-only" || key == "upnp") {
            top.emplace_back(key, "yes");
        } else if (key == "agent-bitrate-nearby" || key == "agent-bitrate-away") {
            auto value = next();
            auto rate = value ? stream(*value) : value;
            if (!rate) {
                return std::unexpected(rate.error());
            }
            top.emplace_back(
                key == "agent-bitrate-nearby" ? "agent-stream-home" : "agent-stream-away", *rate);
        } else if (key == "play-for") {
            auto value = next();
            if (!value) {
                return std::unexpected(value.error());
            }
            groups.push_back(Group{.address = *value, .name = {}, .keys = {}});
        } else if (key == "play-for-name") {
            auto value = next();
            if (!value) {
                return std::unexpected(value.error());
            }
            if (!groups.empty()) {
                groups.back().name = *value;
            }
        } else if (key == "play-for-password" || key == "play-for-password-file" ||
                   key == "play-for-music-root") {
            auto value = next();
            if (!value) {
                return std::unexpected(value.error());
            }
            group_keys().emplace_back(key.substr(std::string_view{"play-for-"}.size()), *value);
        } else if (key == "play-for-bitrate-nearby" || key == "play-for-bitrate-away") {
            auto value = next();
            auto rate = value ? stream(*value) : value;
            if (!rate) {
                return std::unexpected(rate.error());
            }
            group_keys().emplace_back(
                key == "play-for-bitrate-nearby" ? "stream-home" : "stream-away", *rate);
        } else if (const auto* known_key = known(key);
                   known_key != nullptr && known_key->in_file && known_key->shape == Shape::value) {
            auto value = next();
            if (!value) {
                return std::unexpected(value.error());
            }
            top.emplace_back(key, *value);
        } else {
            return std::unexpected(refused("MELODYD_OPTIONS has " + std::string{flag} +
                                           ", which melodyd.conf has no setting for"));
        }
    }

    std::string converted = "# melodyd.conf, written by melodyd from MELODYD_OPTIONS "
                            "(ADR-0286).\n# The file as it was is melodyd.conf.old.\n";
    if (!kept.empty()) {
        converted += "\n" + kept;
    }
    // One key given twice on the command line is the last one there.
    std::vector<std::pair<std::string, std::string>> unique;
    for (auto& [key, value] : top) {
        std::erase_if(unique, [&key](const auto& earlier) { return earlier.first == key; });
        unique.emplace_back(std::move(key), std::move(value));
    }
    if (!unique.empty()) {
        converted += '\n';
    }
    const auto line = [&converted](const std::string& key, const std::string& value) {
        converted.append(key).append(" = ").append(value).append("\n");
    };
    for (const auto& [key, value] : unique) {
        line(key, value);
    }
    for (const auto& group : groups) {
        converted.append("\n[engine ")
            .append(group.name.empty() ? group.address : group.name)
            .append("]\n");
        line("address", group.address);
        std::vector<std::pair<std::string, std::string>> keys = every_group;
        for (const auto& [key, value] : group.keys) {
            std::erase_if(keys, [&key](const auto& earlier) { return earlier.first == key; });
            keys.emplace_back(key, value);
        }
        for (const auto& [key, value] : keys) {
            line(key, value);
        }
    }
    if (auto check = ConfigFile::parse(converted, "melodyd.conf"); !check) {
        return std::unexpected(check.error());
    }
    return converted;
}

core::Result<Settings> check_config(const std::string_view text, std::string origin) {
    auto file = ConfigFile::parse(text, std::move(origin));
    if (!file) {
        return std::unexpected(file.error());
    }
    Settings settings;
    if (auto applied = apply_file(settings, *file); !applied) {
        return std::unexpected(applied.error());
    }
    return settings;
}

core::Result<Settings> read_settings(const std::span<const std::string_view> arguments,
                                     Settings defaults) {
    Settings settings = std::move(defaults);
    if (std::ranges::contains(arguments, std::string_view{"--help"}) ||
        std::ranges::contains(arguments, std::string_view{"-h"})) {
        settings.help = true;
        return settings;
    }

    const auto path = config_path(arguments);
    settings.config_path = path;
    if (auto text = read_text(path)) {
        if (is_options_file(*text)) {
            auto converted = convert_options_file(*text);
            if (!converted) {
                return std::unexpected(refused(path.string() +
                                               " is the environment file of before ADR-0286 "
                                               "and could not be converted: " +
                                               converted.error().message));
            }
            const auto old = std::filesystem::path{path.string() + ".old"};
            if (auto kept = core::write_config_file(old, *text); !kept) {
                return std::unexpected(kept.error());
            }
            if (auto written = core::write_config_file(path, *converted); !written) {
                return std::unexpected(written.error());
            }
            settings.notes.push_back("converted " + path.string() +
                                     " from MELODYD_OPTIONS; the old one is " + old.string());
            text = std::move(*converted);
        }
        auto file = ConfigFile::parse(*text, path.string());
        if (!file) {
            return std::unexpected(file.error());
        }
        if (auto applied = apply_file(settings, *file); !applied) {
            return std::unexpected(applied.error());
        }
        settings.config_file = path;
        if (struct stat info{}; ::stat(path.c_str(), &info) == 0 && (info.st_mode & 077) != 0 &&
                                (file->find("", "", "password") != nullptr ||
                                 std::ranges::any_of(settings.engines, [](const auto& engine) {
                                     return !engine.password.empty();
                                 }))) {
            settings.notes.push_back(path.string() +
                                     " holds a password and others can read it: chmod 600 " +
                                     path.string());
        }
    }

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto argument = arguments[index];
        if (!argument.starts_with("--")) {
            return std::unexpected(refused("unrecognised argument " + std::string{argument}));
        }
        auto key = argument.substr(2);
        std::optional<std::string> value;
        const auto* found = known(key);
        if (found == nullptr && key.starts_with("no-")) {
            if (const auto* toggle = known(key.substr(3));
                toggle != nullptr && toggle->shape == Shape::toggle) {
                found = toggle;
                key = key.substr(3);
                value = "no";
            }
        }
        if (found == nullptr) {
            return std::unexpected(refused("unrecognised argument " + std::string{argument}));
        }
        if (found->shape == Shape::value) {
            if (index + 1 >= arguments.size()) {
                return std::unexpected(refused(std::string{argument} + " wants a value"));
            }
            value = std::string{arguments[++index]};
        }
        if (key == "config") {
            continue;
        }
        if (auto applied = apply(settings, key, value, std::string{argument}); !applied) {
            return std::unexpected(applied.error());
        }
        settings.from_flags.emplace_back(key);
    }
    return settings;
}

} // namespace trackknife::daemon
