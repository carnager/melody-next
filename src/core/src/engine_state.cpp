// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/engine_state.hpp"

#include "trackknife/core/environment.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace trackknife::core {
namespace {

// The user's data folder, as melodyd has always resolved it.
[[nodiscard]] std::filesystem::path data_home() {
    if (const auto* data = environment_variable("XDG_DATA_HOME"); data != nullptr && *data) {
        return data;
    }
    if (const auto* home = environment_variable("HOME"); home != nullptr && *home) {
#ifdef __APPLE__
        return std::filesystem::path{home} / "Library" / "Application Support";
#else
        return std::filesystem::path{home} / ".local" / "share";
#endif
    }
    return std::filesystem::current_path();
}

[[nodiscard]] bool holds_database(const std::filesystem::path& folder) {
    std::error_code ignored;
    return std::filesystem::exists(folder / "lists.sqlite", ignored);
}

// What an engine keeps in its folder. Anything else there -- a window's
// tabs and backups, when both shared it -- is not the engine's to move.
[[nodiscard]] bool engine_entry(const std::string_view name) {
    static constexpr std::string_view kept[]{
        "lists.sqlite", "lists.sqlite-wal", "lists.sqlite-shm", "engine-id", "acoustid.key",
        "lastfm.json",  "artwork-staging",  "transcodes",       "undo"};
    for (const auto entry : kept) {
        if (name == entry) {
            return true;
        }
    }
    // The database's own backups, made before a migration or a cleanup.
    return name.starts_with("lists-") &&
           (name.ends_with(".sqlite") || name.ends_with(".sqlite-wal") ||
            name.ends_with(".sqlite-shm"));
}

[[nodiscard]] Error io_error(std::string message, const std::filesystem::path& path,
                             const std::error_code& code) {
    return Error{.code = ErrorCode::io,
                 .message = std::move(message) + ": " + code.message(),
                 .context = {{.key = "path", .value = path.string()}}};
}

} // namespace

std::filesystem::path default_engine_state_directory() {
    if (const auto* explicit_home = environment_variable("MELODY_STATE_DIR");
        explicit_home != nullptr && *explicit_home) {
        return explicit_home;
    }
    return data_home() / "melody" / "engine";
}

std::filesystem::path legacy_engine_state_directory() {
#ifdef __APPLE__
    return data_home() / "trackknife";
#else
    return data_home() / "trackknife" / "trackknife";
#endif
}

std::filesystem::path engine_state_directory() {
    auto chosen = default_engine_state_directory();
    if (environment_variable("MELODY_STATE_DIR") == nullptr && !holds_database(chosen)) {
        if (auto legacy = legacy_engine_state_directory(); holds_database(legacy)) {
            return legacy;
        }
    }
    return chosen;
}

bool hold_engine_lock(const std::filesystem::path& state_directory) {
    std::error_code ignored;
    std::filesystem::create_directories(state_directory, ignored);
    const auto descriptor =
        ::open((state_directory / "engine.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return false;
    }
    if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        ::close(descriptor);
        return false;
    }
    return true; // Deliberately never closed: held until the process ends.
}

Result<std::size_t> adopt_legacy_engine_state(const std::filesystem::path& to) {
    const auto from = legacy_engine_state_directory();
    if (holds_database(to) || !holds_database(from)) {
        return std::size_t{0U};
    }
    // An engine of before still running there keeps it.
    if (!hold_engine_lock(from)) {
        return std::unexpected(Error{.code = ErrorCode::conflict,
                                     .message = "an engine is still using the old folder",
                                     .context = {{.key = "path", .value = from.string()}}});
    }
    std::error_code error;
    std::filesystem::create_directories(to, error);
    if (error) {
        return std::unexpected(io_error("cannot make the engine's folder", to, error));
    }
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator{from, error}) {
        if (auto name = entry.path().filename().string(); engine_entry(name)) {
            names.push_back(std::move(name));
        }
    }
    if (error) {
        return std::unexpected(io_error("cannot read the old folder", from, error));
    }
    // Each renamed in place -- the same disk -- and all put back if one
    // cannot be: either everything is in the new folder or nothing is.
    std::vector<std::string> moved;
    for (const auto& name : names) {
        std::filesystem::rename(from / name, to / name, error);
        if (error) {
            const auto failed = error;
            for (const auto& back : moved) {
                std::error_code ignored;
                std::filesystem::rename(to / back, from / back, ignored);
            }
            return std::unexpected(io_error("cannot move " + name, from / name, failed));
        }
        moved.push_back(name);
    }
    return moved.size();
}

} // namespace trackknife::core
