// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::daemon {

// ADR-0234: an engine this one plays for by address, on these speakers;
// `--play-for-*` after one applies to it, before any to all.
struct PlayFor {
    std::string address;
    std::string name;
    std::string password;
    std::string password_file;
    std::optional<std::filesystem::path> music_root;
    // ADR-0239: kbps asked streamed nearby and away; 0 the original.
    std::optional<int> nearby_kbps;
    std::optional<int> away_kbps;
};

// ADR-0286: an `[engine NAME]` section of melodyd.conf -- another engine,
// and what is different about playing for it.
struct ListedEngine {
    std::string name;
    std::optional<std::string> address;
    std::string password;
    std::string password_file;
    std::optional<std::filesystem::path> music_root;
    std::optional<int> nearby_kbps;
    std::optional<int> away_kbps;
    std::optional<bool> agent;
    // "melodyd.conf:12", for what is said about it.
    std::string where;
};

// What melodyd is asked to be: melodyd.conf first, then the command line,
// which wins.
struct Settings {
    std::filesystem::path socket_path;
    std::filesystem::path state_directory;
    std::string listen_address;
    std::string http_address;
    std::uint64_t transcode_cache_mb = 2048;
    bool local_only = false;
    bool upnp_enabled = false;
    std::string upnp_interface;
    std::string password;
    std::string password_file;
    std::string engine_name;
    std::optional<std::filesystem::path> music_root;
    std::vector<PlayFor> play_fors;
    PlayFor play_for_defaults;
    std::vector<ListedEngine> engines;
    // Unset: on when this engine has a password (ADR-0286), for without one
    // no other engine would take it.
    std::optional<bool> agent;
    std::string agent_password;
    std::string agent_password_file;
    std::optional<std::filesystem::path> agent_music_root;
    std::optional<int> agent_nearby_kbps;
    std::optional<int> agent_away_kbps;

    // The file read, when there was one.
    std::optional<std::filesystem::path> config_file;
    // Worth saying at start: a file converted, one others can read.
    std::vector<std::string> notes;
    bool help = false;
};

// $XDG_CONFIG_HOME/melody/melodyd.conf, else ~/.config/melody/melodyd.conf.
[[nodiscard]] std::filesystem::path default_config_path();

// The file to read: --config, else $MELODYD_CONFIG, else the default.
[[nodiscard]] std::filesystem::path
config_path(std::span<const std::string_view> arguments);

// Reads the config file, when there is one, then the arguments (without the
// program's name) over it. `defaults` holds the socket and state folder used
// when neither names one. Refusals are what to print, the file's with their
// line.
[[nodiscard]] core::Result<Settings> read_settings(std::span<const std::string_view> arguments,
                                                   Settings defaults);

// "original" is 0, "opus N" is N, for N in 16..512 (ADR-0239).
[[nodiscard]] std::optional<int> parse_stream(std::string_view text);
// The other way round, for a file written from flags.
[[nodiscard]] std::string stream_text(int kbps);

// The environment file melodyd.conf was until ADR-0286 -- MELODYD_OPTIONS
// holding flags -- written as settings. Lines other than MELODYD_OPTIONS are
// kept, commented out. Refuses flags it does not know.
[[nodiscard]] core::Result<std::string> convert_options_file(std::string_view text);
[[nodiscard]] bool is_options_file(std::string_view text);

} // namespace trackknife::daemon
