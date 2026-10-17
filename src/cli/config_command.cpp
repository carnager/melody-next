// SPDX-License-Identifier: GPL-3.0-only

#include "config_command.hpp"

#include "settings.hpp"
#include "trackknife/core/config_file.hpp"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>

namespace trackknife::cli {
namespace {

using core::ConfigFile;

constexpr std::string_view restart_hint =
    "Restart the engine to use it: systemctl --user restart melodyd";

[[nodiscard]] std::optional<std::string> read_text(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        return std::nullopt;
    }
    return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

int refuse(const std::string& message) {
    std::cerr << "melody-cli: " << message << "\n";
    return EXIT_FAILURE;
}

// Checked as melodyd would take it, then written in one step.
int write_checked(const std::filesystem::path& path, const std::string& text) {
    if (auto checked = daemon::check_config(text, path.string()); !checked) {
        return refuse(checked.error().message + "\n(" + path.string() + " is left as it was)");
    }
    if (auto written = core::write_config_file(path, text); !written) {
        return refuse(written.error().message);
    }
    return EXIT_SUCCESS;
}

// $VISUAL, else $EDITOR, else vi, on `file`; whether it ended well.
[[nodiscard]] bool edit(const std::filesystem::path& file) {
    const auto child = ::fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        // Through the shell, for an editor given with its options ("code -w").
        ::execl("/bin/sh", "sh", "-c", "exec ${VISUAL:-${EDITOR:-vi}} \"$1\"", "sh", file.c_str(),
                static_cast<char*>(nullptr));
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

[[nodiscard]] bool ask_again() {
    std::cerr << "Edit again? [Y/n] ";
    std::string answer;
    if (!std::getline(std::cin, answer)) {
        return false;
    }
    return answer.empty() || answer == "y" || answer == "Y" || answer == "yes";
}

int edit_command(const std::filesystem::path& path) {
    const auto before = read_text(path);
    auto text = before ? *before : std::string{daemon::config_skeleton()};
    if (!before) {
        std::cerr << "No " << path.string() << " yet; starting from the skeleton.\n";
    }
    std::error_code error;
    const auto folder =
        path.parent_path().empty() ? std::filesystem::path{"."} : path.parent_path();
    if (!std::filesystem::exists(folder, error)) {
        std::filesystem::create_directories(folder, error);
        std::filesystem::permissions(folder, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, error);
    }
    // Edited beside it, its owner's only: it may hold a password. The file
    // itself is touched only once what was written is taken.
    auto draft = (folder / ("." + path.filename().string() + ".edit-XXXXXX")).string();
    const int fd = ::mkstemp(draft.data());
    if (fd < 0) {
        return refuse("cannot write in " + folder.string());
    }
    ::close(fd);
    const auto discard = [&draft] {
        std::error_code ignored;
        std::filesystem::remove(draft, ignored);
    };
    if (auto written = core::write_config_file(draft, text); !written) {
        discard();
        return refuse(written.error().message);
    }
    while (true) {
        if (!edit(draft)) {
            discard();
            return refuse("the editor did not finish; " + path.string() + " is left as it was");
        }
        const auto edited = read_text(draft);
        if (!edited) {
            discard();
            return refuse("cannot read what was written");
        }
        if (before && *edited == *before) {
            discard();
            std::cerr << path.string() << " is unchanged.\n";
            return EXIT_SUCCESS;
        }
        if (auto checked = daemon::check_config(*edited, path.string()); !checked) {
            std::cerr << checked.error().message << "\n";
            if (ask_again()) {
                continue;
            }
            discard();
            return refuse(path.string() + " is left as it was");
        }
        discard();
        if (auto written = core::write_config_file(path, *edited); !written) {
            return refuse(written.error().message);
        }
        std::cerr << "Written " << path.string() << ". " << restart_hint << "\n";
        return EXIT_SUCCESS;
    }
}

} // namespace

int config_command(const std::vector<std::string>& words) {
    std::optional<std::filesystem::path> given;
    std::string section;
    bool unset = false;
    bool editing = false;
    std::vector<std::string> rest;
    for (std::size_t index = 0; index < words.size(); ++index) {
        const auto& word = words[index];
        if (word == "--config" || word == "--engine") {
            if (index + 1 >= words.size()) {
                return refuse(word + " wants a value");
            }
            const auto& value = words[++index];
            if (word == "--config") {
                given = std::filesystem::path{value};
            } else {
                section = value;
            }
        } else if (word == "--unset") {
            unset = true;
        } else if (word == "--edit") {
            editing = true;
        } else if (word.starts_with("--") && rest.empty()) {
            return refuse("config does not know " + word);
        } else {
            rest.push_back(word);
        }
    }
    const auto path = given ? *given : daemon::config_path({});
    if (editing) {
        if (!rest.empty() || unset || !section.empty()) {
            return refuse("config --edit takes nothing else");
        }
        return edit_command(path);
    }

    const auto text = read_text(path);
    if (rest.empty()) {
        if (!section.empty() || unset) {
            return refuse("config " + std::string{unset ? "--unset" : "--engine NAME"} +
                          " wants a key");
        }
        if (!text) {
            std::cerr << "No " << path.string()
                      << " yet: melody-cli config --edit starts one from the skeleton, "
                         "melody-cli config KEY VALUE with one setting.\n";
            return EXIT_SUCCESS;
        }
        std::cout << *text;
        return EXIT_SUCCESS;
    }

    const auto& key = rest.front();
    const std::string_view kind = section.empty() ? "" : "engine";
    auto parsed =
        ConfigFile::parse(text ? *text : std::string{daemon::config_skeleton()}, path.string());
    if (!parsed) {
        return refuse(parsed.error().message + "\n(melody-cli config --edit to mend it)");
    }
    const auto described = section.empty() ? key : "[engine " + section + "] " + key;

    if (unset) {
        if (rest.size() != 1U) {
            return refuse("config --unset takes one key");
        }
        if (!text || !parsed->unset(kind, section, key)) {
            std::cerr << described << " is not set in " << path.string() << "\n";
            return EXIT_SUCCESS;
        }
        if (const int status = write_checked(path, parsed->text()); status != EXIT_SUCCESS) {
            return status;
        }
        std::cerr << described << " unset in " << path.string() << ". " << restart_hint << "\n";
        return EXIT_SUCCESS;
    }

    if (rest.size() == 1U) {
        const auto* entry = text ? parsed->find(kind, section, key) : nullptr;
        if (entry == nullptr) {
            std::cerr << described << " is not set in " << path.string() << "\n";
            return EXIT_FAILURE;
        }
        std::cout << entry->value << "\n";
        return EXIT_SUCCESS;
    }

    // The rest of the line is the value, as in the file: "opus 96" needs no
    // quotes.
    std::string value = rest[1];
    for (std::size_t index = 2; index < rest.size(); ++index) {
        value += " " + rest[index];
    }
    if (auto set = parsed->set(kind, section, key, value); !set) {
        return refuse(set.error().message);
    }
    if (const int status = write_checked(path, parsed->text()); status != EXIT_SUCCESS) {
        return status;
    }
    if (!text) {
        std::cerr << "Started " << path.string() << " from the skeleton.\n";
    }
    std::cerr << described << " = " << (key == "password" ? "(set)" : value) << " in "
              << path.string() << ". " << restart_hint << "\n";
    return EXIT_SUCCESS;
}

} // namespace trackknife::cli
