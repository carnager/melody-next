// SPDX-License-Identifier: GPL-3.0-only

// ADR-0286: the format of melodyd.conf.

#include "trackknife/core/config_file.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

using trackknife::core::ConfigFile;

int failures = 0;

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

[[nodiscard]] std::string value_of(const ConfigFile& file, const std::string_view kind,
                                   const std::string_view name, const std::string_view key) {
    const auto* entry = file.find(kind, name, key);
    return entry == nullptr ? std::string{"<none>"} : entry->value;
}

[[nodiscard]] std::string refusal(const std::string_view text) {
    const auto parsed = ConfigFile::parse(text, "melodyd.conf");
    return parsed ? std::string{"<parsed>"} : parsed.error().message;
}

constexpr std::string_view caprica = "# melodyd.conf -- caprica\n"
                                     "\n"
                                     "name = caprica\n"
                                     "password = se#cret = really\n"
                                     "\n"
                                     "# How this machine plays for every engine.\n"
                                     "agent-music-root = /mnt/nas/Music/Rips/flac\n"
                                     "agent-stream-away =   opus 128  \n"
                                     "\n"
                                     "[engine gemenon]\n"
                                     "# Its address, for when the network can't find it.\n"
                                     "address = 192.168.0.200:6603\n"
                                     "\n"
                                     "[engine Pixel 10 Pro]\n"
                                     "agent = no\n";

void readsWhatItSays() {
    const auto parsed = ConfigFile::parse(caprica, "melodyd.conf");
    CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    CHECK(parsed->top().size() == 4U);
    CHECK(value_of(*parsed, "", "", "name") == "caprica");
    // The rest of the line: a # or an = in a value is the value's.
    CHECK(value_of(*parsed, "", "", "password") == "se#cret = really");
    // Trimmed at both ends, inner spaces kept.
    CHECK(value_of(*parsed, "", "", "agent-stream-away") == "opus 128");
    CHECK(parsed->top()[2].line == 7U);
    CHECK(parsed->sections().size() == 2U);
    CHECK(value_of(*parsed, "engine", "gemenon", "address") == "192.168.0.200:6603");
    // A section's name is the rest of its header, spaces and all.
    CHECK(value_of(*parsed, "engine", "Pixel 10 Pro", "agent") == "no");
    CHECK(parsed->section("engine", "Pixel 10 Pro") != nullptr);
    CHECK(value_of(*parsed, "engine", "gemenon", "name") == "<none>");
    CHECK(value_of(*parsed, "", "", "address") == "<none>");
    // Unchanged, it is written as it was read.
    CHECK(parsed->text() == caprica);
}

void readsLineEndingsAndBytes() {
    const auto crlf = ConfigFile::parse("name = a\r\n[engine b]\r\naddress = c\r\n", "f");
    CHECK(crlf.has_value());
    if (crlf) {
        CHECK(value_of(*crlf, "", "", "name") == "a");
        CHECK(value_of(*crlf, "engine", "b", "address") == "c");
    }
    // A path is raw bytes: not UTF-8 is not refused.
    const std::string latin1 = "music-root = /srv/M\xFCsik\n";
    const auto bytes = ConfigFile::parse(latin1, "f");
    CHECK(bytes.has_value() && value_of(*bytes, "", "", "music-root") == "/srv/M\xFCsik");
    // No newline at the end, an empty file, only comments.
    CHECK(ConfigFile::parse("name = a", "f").has_value());
    CHECK(ConfigFile::parse("", "f").has_value());
    CHECK(ConfigFile::parse("# nothing\n\n   # indented\n", "f").has_value());
}

void refusesWhatItIsNot() {
    CHECK(refusal("name = a\nplay for gemenon\n").starts_with("melodyd.conf:2: not a setting"));
    CHECK(refusal("Name = a\n").starts_with("melodyd.conf:1: \"Name\" is not a key"));
    CHECK(refusal("agent_music = a\n").starts_with("melodyd.conf:1:"));
    CHECK(refusal("-agent = yes\n").starts_with("melodyd.conf:1:"));
    CHECK(refusal("password =\n").starts_with("melodyd.conf:1: password has no value"));
    CHECK(refusal("name = a\n\nname = b\n") ==
          "melodyd.conf:3: name is given twice (first on line 1)");
    // The same key in two sections is two keys.
    CHECK(refusal("[engine a]\naddress = x\n[engine b]\naddress = y\n") == "<parsed>");
    CHECK(refusal("[engine a]\naddress = x\naddress = y\n").starts_with(
        "melodyd.conf:3: address is given twice"));
    CHECK(refusal("[engine a]\n[engine b]\n[engine a]\n") ==
          "melodyd.conf:3: [engine a] is given twice (first on line 1)");
    CHECK(refusal("[engine a\n").starts_with("melodyd.conf:1: a section header ends with ]"));
    CHECK(refusal("[]\n").starts_with("melodyd.conf:1:"));
    CHECK(refusal("[Engine a]\n").starts_with("melodyd.conf:1:"));
    // The error says where, for a reader that shows it as it is.
    const auto parsed = ConfigFile::parse("x\n", "/etc/x.conf");
    CHECK(!parsed && parsed.error().context.size() == 2U &&
          parsed.error().context[1].value == "1");
}

void setsWithoutTouchingTheRest() {
    auto file = *ConfigFile::parse(caprica, "melodyd.conf");

    // An existing key: its line, in place.
    CHECK(file.set("", "", "agent-stream-away", "opus 96").has_value());
    CHECK(value_of(file, "", "", "agent-stream-away") == "opus 96");
    auto expected = std::string{caprica};
    expected.replace(expected.find("agent-stream-away =   opus 128  "),
                     std::string_view{"agent-stream-away =   opus 128  "}.size(),
                     "agent-stream-away = opus 96");
    CHECK(file.text() == expected);

    // A new top-level key: after the last one, so before the first section
    // and its comments.
    CHECK(file.set("", "", "upnp", "yes").has_value());
    CHECK(file.text().find("agent-stream-away = opus 96\nupnp = yes\n\n[engine gemenon]") !=
          std::string::npos);

    // A new key of an existing section: after its last entry.
    CHECK(file.set("engine", "gemenon", "music-root", "/mnt/gemenon").has_value());
    CHECK(file.text().find("address = 192.168.0.200:6603\nmusic-root = /mnt/gemenon\n") !=
          std::string::npos);

    // A new section: at the end, after a blank line.
    CHECK(file.set("engine", "kitchen", "agent", "no").has_value());
    CHECK(file.text().ends_with("agent = no\n\n[engine kitchen]\nagent = no\n"));

    // Every comment is still there.
    CHECK(file.text().find("# Its address, for when the network can't find it.\n") !=
          std::string::npos);
    CHECK(file.text().starts_with("# melodyd.conf -- caprica\n\nname = caprica\n"));

    // What it writes, it reads back.
    const auto again = ConfigFile::parse(file.text(), "again");
    CHECK(again.has_value() && value_of(*again, "engine", "kitchen", "agent") == "no");

    CHECK(file.unset("engine", "gemenon", "music-root"));
    CHECK(!file.unset("engine", "gemenon", "music-root"));
    CHECK(!file.unset("engine", "nowhere", "address"));
    CHECK(value_of(file, "engine", "gemenon", "music-root") == "<none>");
    CHECK(file.text().find("music-root = /mnt/gemenon") == std::string::npos);

    // Refused: what the format cannot hold.
    CHECK(!file.set("", "", "Name", "a").has_value());
    CHECK(!file.set("", "", "name", "").has_value());
    CHECK(!file.set("", "", "name", " padded").has_value());
    CHECK(!file.set("", "", "name", "two\nlines").has_value());
    CHECK(!file.set("engine", "a]b", "address", "x").has_value());
    CHECK(!file.set("Engine", "a", "address", "x").has_value());
}

void setsIntoFilesWithoutTopLevelKeys() {
    // Only a heading comment and a section: the key goes above the
    // section's own comments, apart from them.
    auto file = *ConfigFile::parse("# caprica\n\n# The NAS.\n[engine gemenon]\naddress = a\n", "f");
    CHECK(file.set("", "", "name", "caprica").has_value());
    CHECK(file.text() == "# caprica\nname = caprica\n\n# The NAS.\n[engine gemenon]\naddress = a\n");

    auto empty = *ConfigFile::parse("", "f");
    CHECK(empty.set("", "", "name", "caprica").has_value());
    CHECK(empty.text() == "name = caprica\n");
    CHECK(empty.set("engine", "gemenon", "address", "a").has_value());
    CHECK(empty.text() == "name = caprica\n\n[engine gemenon]\naddress = a\n");
}

[[nodiscard]] std::string contents(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] unsigned mode_of(const std::filesystem::path& path) {
    struct stat info{};
    return ::stat(path.c_str(), &info) == 0 ? static_cast<unsigned>(info.st_mode & 07777U) : 0U;
}

void writesInOneStep() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("trackknife-config-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);

    // A new file, in a folder not there yet: its owner's only.
    const auto fresh = root / "melody" / "melodyd.conf";
    CHECK(trackknife::core::write_config_file(fresh, "password = x\n").has_value());
    CHECK(contents(fresh) == "password = x\n");
    CHECK(mode_of(fresh) == 0600U);
    CHECK(mode_of(fresh.parent_path()) == 0700U);
    const auto read = ConfigFile::read(fresh);
    CHECK(read.has_value() && value_of(*read, "", "", "password") == "x");

    // An existing file keeps its permissions.
    ::chmod(fresh.c_str(), 0640);
    CHECK(trackknife::core::write_config_file(fresh, "password = y\n").has_value());
    CHECK(contents(fresh) == "password = y\n");
    CHECK(mode_of(fresh) == 0640U);

    // Through a symlink: the target is written, the link stays a link.
    const auto dotfiles = root / "dotfiles" / "melodyd.conf";
    std::filesystem::create_directories(dotfiles.parent_path());
    {
        std::ofstream{dotfiles} << "name = a\n";
    }
    const auto linked = root / "linked.conf";
    std::filesystem::create_symlink(std::filesystem::path{"dotfiles"} / "melodyd.conf", linked);
    CHECK(trackknife::core::write_config_file(linked, "name = b\n").has_value());
    CHECK(std::filesystem::is_symlink(linked));
    CHECK(contents(dotfiles) == "name = b\n");

    // No prepared copy is left behind.
    std::size_t files = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator{root / "melody"}) {
        ++files;
    }
    CHECK(files == 1U);

    const auto missing = ConfigFile::read(root / "nothing.conf");
    CHECK(!missing && missing.error().code == trackknife::core::ErrorCode::not_found);

    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    readsWhatItSays();
    readsLineEndingsAndBytes();
    refusesWhatItIsNot();
    setsWithoutTouchingTheRest();
    setsIntoFilesWithoutTopLevelKeys();
    writesInOneStep();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
