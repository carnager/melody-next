// SPDX-License-Identifier: GPL-3.0-only

// ADR-0286: melodyd.conf and the command line over it.

#include "settings.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

using trackknife::daemon::read_settings;
using trackknife::daemon::Settings;

int failures = 0;

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

std::filesystem::path root;

[[nodiscard]] std::filesystem::path file(const std::string& name, const std::string& text) {
    const auto path = root / name;
    std::ofstream{path, std::ios::binary} << text;
    ::chmod(path.c_str(), 0600);
    return path;
}

[[nodiscard]] std::string contents(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] trackknife::core::Result<Settings> settings_of(const std::filesystem::path& config,
                                                             std::vector<std::string> flags = {}) {
    std::vector<std::string> owned{"--config", config.string()};
    owned.insert(owned.end(), flags.begin(), flags.end());
    std::vector<std::string_view> arguments{owned.begin(), owned.end()};
    return read_settings(arguments, Settings{});
}

[[nodiscard]] std::string refusal(const std::string& text, std::vector<std::string> flags = {}) {
    const auto read = settings_of(file("refused.conf", text), std::move(flags));
    return read ? std::string{"<read>"} : read.error().message;
}

void flagsAlone() {
    const auto read = settings_of(root / "none.conf",
                                  {"--name", "gemenon", "--password", "p", "--agent-stream-away",
                                   "opus 96", "--no-agent", "--upnp", "--transcode-cache", "512"});
    CHECK(read.has_value());
    if (!read) {
        return;
    }
    CHECK(!read->config_file);
    CHECK(read->engine_name == "gemenon");
    CHECK(read->password == "p");
    CHECK(read->agent_away_kbps == 96);
    CHECK(!read->agent_nearby_kbps);
    CHECK(read->agent == false);
    CHECK(read->upnp_enabled);
    CHECK(read->transcode_cache_mb == 512U);
    // Unset, the agent is decided by the password (main's business).
    const auto plain = settings_of(root / "none.conf");
    CHECK(plain && !plain->agent);
}

void theFileAndFlagsOverIt() {
    const auto config = file("caprica.conf", "# melodyd.conf -- caprica\n"
                                             "\n"
                                             "name = caprica\n"
                                             "password = shared secret\n"
                                             "agent = no\n"
                                             "\n"
                                             "# How this machine plays for every engine.\n"
                                             "agent-music-root = /mnt/nas/Music/Rips/flac\n"
                                             "agent-stream-home = original\n"
                                             "agent-stream-away = opus 128\n"
                                             "\n"
                                             "[engine gemenon]\n"
                                             "# For when the network can't find it.\n"
                                             "address = 192.168.0.200:6603\n"
                                             "stream-away = opus 64\n"
                                             "\n"
                                             "[engine Pixel 10 Pro]\n"
                                             "agent = no\n");
    const auto read = settings_of(config);
    CHECK(read.has_value());
    if (!read) {
        std::cerr << read.error().message << '\n';
        return;
    }
    CHECK(read->config_file == config);
    CHECK(read->engine_name == "caprica");
    CHECK(read->password == "shared secret");
    CHECK(read->agent == false);
    CHECK(read->agent_music_root == std::filesystem::path{"/mnt/nas/Music/Rips/flac"});
    CHECK(read->agent_nearby_kbps == 0);
    CHECK(read->agent_away_kbps == 128);
    CHECK(read->engines.size() == 2U);
    if (read->engines.size() == 2U) {
        CHECK(read->engines[0].name == "gemenon");
        CHECK(read->engines[0].address == "192.168.0.200:6603");
        CHECK(read->engines[0].away_kbps == 64);
        CHECK(!read->engines[0].agent);
        CHECK(read->engines[0].where == config.string() + ":12");
        CHECK(read->engines[1].name == "Pixel 10 Pro");
        CHECK(!read->engines[1].address);
        CHECK(read->engines[1].agent == false);
    }
    CHECK(read->notes.empty());

    // A flag wins; a password from a flag puts the file's password-file away
    // and the other way round.
    const auto over =
        settings_of(config, {"--name", "laptop", "--agent", "--password-file", "/run/secret"});
    CHECK(over && over->engine_name == "laptop" && over->agent == true && over->password.empty() &&
          over->password_file == "/run/secret");
}

void refusesWithTheLine() {
    CHECK(refusal("name = a\nbitrate = 3\n")
              .ends_with("refused.conf:2: bitrate is not a setting melodyd.conf has"));
    CHECK(refusal("agent-bitrate-away = 96\n")
              .ends_with(":1: agent-bitrate-away is not a setting melodyd.conf has; it is "
                         "agent-stream-away now "
                         "(original, or opus 128)"));
    CHECK(refusal("play-for = a:1\n").find("[engine NAME] section now") != std::string::npos);
    CHECK(refusal("config = x\n").ends_with(":1: config is not a setting melodyd.conf has"));
    CHECK(refusal("agent = maybe\n").ends_with(":1: agent is yes or no, got maybe"));
    CHECK(refusal("agent-stream-home = flac\n")
              .ends_with(":1: agent-stream-home is original, or opus and a bit rate from 16 to 512 "
                         "(opus 128), got "
                         "flac"));
    CHECK(refusal("agent-stream-home = opus 8\n").find(":1: agent-stream-home is") !=
          std::string::npos);
    CHECK(refusal("transcode-cache = lots\n")
              .ends_with(":1: transcode-cache wants megabytes, got lots"));
    CHECK(refusal("password = a\npassword-file = b\n")
              .ends_with(":2: password and password-file both given; one is enough"));
    CHECK(refusal("[speaker kitchen]\n")
              .ends_with(":1: [speaker] is not a section melodyd.conf has; [engine NAME] is"));
    CHECK(refusal("[engine]\n").ends_with(":1: [engine] wants the name the engine goes by"));
    CHECK(refusal("[engine a]\nname = b\n")
              .ends_with(":2: name is not a setting of an [engine]; those are address, music-root, "
                         "stream-home, "
                         "stream-away, password, password-file, agent"));
    CHECK(refusal("[engine a]\nstream-away = opus 600\n").find(":2: stream-away is") !=
          std::string::npos);
    CHECK(refusal("[engine a]\npassword = x\npassword-file = y\n")
              .ends_with(":3: password and password-file both given; one is enough"));
    // The format itself: refused by the parser, with its line too.
    CHECK(refusal("name\n").ends_with(":1: not a setting: settings read key = value, comments "
                                      "start with #"));
    // The command line.
    CHECK(refusal("", {"--bogus"}) == "unrecognised argument --bogus");
    CHECK(refusal("", {"--no-name"}) == "unrecognised argument --no-name");
    CHECK(refusal("", {"--name"}) == "--name wants a value");
    CHECK(refusal("", {"--agent-stream-away", "lossless"}).starts_with("--agent-stream-away is"));
}

void whichFile() {
    // --config wins over MELODYD_CONFIG, which wins over the default.
    const auto named = file("named.conf", "name = from-environment\n");
    ::setenv("MELODYD_CONFIG", named.c_str(), 1);
    const std::vector<std::string_view> none{};
    auto read = read_settings(none, Settings{});
    CHECK(read && read->engine_name == "from-environment");
    read = settings_of(file("given.conf", "name = from-flag\n"));
    CHECK(read && read->engine_name == "from-flag");
    ::unsetenv("MELODYD_CONFIG");
    ::setenv("XDG_CONFIG_HOME", (root / "xdg").c_str(), 1);
    CHECK(trackknife::daemon::default_config_path() == root / "xdg" / "melody" / "melodyd.conf");
    // No file is no settings from one, not a refusal.
    read = read_settings(none, Settings{});
    CHECK(read && !read->config_file);
    ::unsetenv("XDG_CONFIG_HOME");
}

void saysWhenOthersCanReadThePassword() {
    const auto open = file("open.conf", "password = x\n");
    ::chmod(open.c_str(), 0644);
    const auto read = settings_of(open);
    CHECK(read && read->notes.size() == 1U &&
          read->notes[0].find("others can read it: chmod 600") != std::string::npos);
    const auto no_password = file("open-plain.conf", "name = x\n");
    ::chmod(no_password.c_str(), 0644);
    const auto plain = settings_of(no_password);
    CHECK(plain && plain->notes.empty());
}

void convertsTheOldFile() {
    const auto old = std::string{
        "# melodyd on gemenon\n"
        "MELODYD_OPTIONS=\"--name gemenon --agent --password-file "
        "/home/you/.config/melody/password "
        "--upnp --agent-bitrate-nearby 0 --agent-bitrate-away 96 "
        "--play-for-music-root /mnt/all --play-for 10.8.0.1:6603 --play-for-name caprica "
        "--play-for-bitrate-away 64 --play-for nas:6603\"\n"
        "OTHER=1\n"};
    const auto path = file("melodyd.conf", old);
    const auto read = settings_of(path);
    CHECK(read.has_value());
    if (!read) {
        std::cerr << read.error().message << '\n';
        return;
    }
    CHECK(contents(std::filesystem::path{path.string() + ".old"}) == old);
    const auto converted = contents(path);
    CHECK(converted.find("# melodyd on gemenon\n# OTHER=1\n") != std::string::npos);
    CHECK(converted.find("name = gemenon\nagent = yes\npassword-file = "
                         "/home/you/.config/melody/password\nupnp = yes\n"
                         "agent-stream-home = original\nagent-stream-away = opus 96\n") !=
          std::string::npos);
    CHECK(converted.find("[engine caprica]\naddress = 10.8.0.1:6603\nmusic-root = /mnt/all\n"
                         "stream-away = opus 64\n") != std::string::npos);
    // Unnamed, the section is named after its address; every group's
    // defaults are its own.
    CHECK(converted.find("[engine nas:6603]\naddress = nas:6603\nmusic-root = /mnt/all\n") !=
          std::string::npos);
    CHECK(read->engine_name == "gemenon");
    CHECK(read->password_file == "/home/you/.config/melody/password");
    CHECK(read->agent_away_kbps == 96);
    CHECK(read->engines.size() == 2U);
    CHECK(read->notes.size() == 1U && read->notes[0].starts_with("converted "));
    // Once: read again, it is the new file.
    const auto again = settings_of(path);
    CHECK(again && again->notes.empty() && again->engine_name == "gemenon");

    // What cannot be converted is left as it was, and said.
    const auto odd = file("odd.conf", "MELODYD_OPTIONS=--frobnicate\n");
    const auto refused = settings_of(odd);
    CHECK(!refused && refused.error().message.find("--frobnicate") != std::string::npos);
    CHECK(contents(odd) == "MELODYD_OPTIONS=--frobnicate\n");
}

void streams() {
    using trackknife::daemon::parse_stream;
    CHECK(parse_stream("original") == 0);
    CHECK(parse_stream("opus 128") == 128);
    CHECK(parse_stream("opus   16") == 16);
    CHECK(!parse_stream("opus"));
    CHECK(!parse_stream("opus128"));
    CHECK(!parse_stream("opus 0"));
    CHECK(!parse_stream("opus 513"));
    CHECK(!parse_stream("Original"));
    CHECK(trackknife::daemon::stream_text(0) == "original");
    CHECK(trackknife::daemon::stream_text(96) == "opus 96");
}

// The skeleton melody-cli config starts a file from: taken as it is, and
// each example in it -- "#key = value" -- a setting melodyd takes, alone.
void theSkeletonHolds() {
    std::ifstream stream{MELODY_MELODYD_SKELETON, std::ios::binary};
    const std::string skeleton{std::istreambuf_iterator<char>{stream},
                               std::istreambuf_iterator<char>{}};
    CHECK(!skeleton.empty());
    const auto as_is = trackknife::daemon::check_config(skeleton, "skeleton");
    CHECK(as_is && as_is->engines.empty());
    std::size_t examples = 0;
    std::string section;
    std::size_t start = 0;
    while (start < skeleton.size()) {
        const auto end = skeleton.find('\n', start);
        const auto line = skeleton.substr(start, end - start);
        start = end == std::string::npos ? skeleton.size() : end + 1;
        if (line.starts_with("#[")) {
            section = line.substr(1);
            continue;
        }
        if (line.size() < 2 || line[0] != '#' || line[1] < 'a' || line[1] > 'z' ||
            line.find(" = ") == std::string::npos) {
            continue;
        }
        ++examples;
        const auto text = (section.empty() ? "" : section + "\n") + line.substr(1) + "\n";
        const auto taken = trackknife::daemon::check_config(text, "example");
        if (!taken) {
            std::cerr << "skeleton example \"" << line << "\": " << taken.error().message << '\n';
        }
        CHECK(taken.has_value());
    }
    // Every key has its example: the 18 of the file and the 7 of an
    // [engine]. A key added to melodyd and not here fails this.
    CHECK(examples == 25U);
}

} // namespace

int main() {
    root = std::filesystem::temp_directory_path() /
           ("trackknife-daemon-settings-" + std::to_string(::getpid()));
    std::filesystem::create_directories(root);
    ::unsetenv("MELODYD_CONFIG");
    flagsAlone();
    theFileAndFlagsOverIt();
    refusesWithTheLine();
    whichFile();
    saysWhenOthersCanReadThePassword();
    convertsTheOldFile();
    streams();
    theSkeletonHolds();
    std::filesystem::remove_all(root);
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
