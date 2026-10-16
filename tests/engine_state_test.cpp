// SPDX-License-Identifier: GPL-3.0-only

// ADR-0287: an engine's state moves from Trackknife's folder to melody's,
// once, and only what is the engine's.

#include "trackknife/core/engine_state.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace core = trackknife::core;
namespace fs = std::filesystem;

int failures = 0;

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

void touch(const fs::path& path, const std::string& text = "x") {
    fs::create_directories(path.parent_path());
    std::ofstream{path} << text;
}

[[nodiscard]] std::string read(const fs::path& path) {
    std::ifstream in{path};
    return std::string{std::istreambuf_iterator<char>{in}, {}};
}

} // namespace

int main() {
    const auto root =
        fs::temp_directory_path() / ("melody-engine-state-" + std::to_string(::getpid()));
    fs::remove_all(root);
    ::setenv("XDG_DATA_HOME", root.c_str(), 1);
    ::unsetenv("MELODY_STATE_DIR");
    const auto legacy = core::legacy_engine_state_directory();
    const auto fresh = core::default_engine_state_directory();
    CHECK(fresh == root / "melody" / "engine");

    // Nothing anywhere: the new folder, and nothing to move.
    CHECK(core::engine_state_directory() == fresh);
    CHECK(core::adopt_legacy_engine_state(fresh).value_or(99U) == 0U);

    // Trackknife's folder, the engine's files and the window's together.
    touch(legacy / "lists.sqlite", "the library");
    touch(legacy / "lists.sqlite-wal");
    touch(legacy / "lists-before-engine-lists.sqlite");
    touch(legacy / "engine-id", "id");
    touch(legacy / "undo" / "copy");
    touch(legacy / "tabs" / "one.json");
    touch(legacy / "before-main-restore-20260914" / "lists.sqlite");
    touch(legacy / "engine.token");
    // Not moved yet: a window follows the engine to where it still is.
    CHECK(core::engine_state_directory() == legacy);

    // An engine of before still running there keeps it.
    {
        const auto held = ::open((legacy / "engine.lock").c_str(), O_RDWR | O_CREAT, 0600);
        CHECK(held >= 0 && ::flock(held, LOCK_EX | LOCK_NB) == 0);
        const auto refused = core::adopt_legacy_engine_state(fresh);
        CHECK(!refused && refused.error().code == core::ErrorCode::conflict);
        CHECK(fs::exists(legacy / "lists.sqlite") && !fs::exists(fresh / "lists.sqlite"));
        ::close(held);
    }

    // Moved: the engine's, and only those.
    const auto moved = core::adopt_legacy_engine_state(fresh);
    CHECK(moved.has_value() && *moved == 5U);
    CHECK(read(fresh / "lists.sqlite") == "the library");
    CHECK(fs::exists(fresh / "lists.sqlite-wal"));
    CHECK(fs::exists(fresh / "lists-before-engine-lists.sqlite"));
    CHECK(read(fresh / "engine-id") == "id");
    CHECK(fs::exists(fresh / "undo" / "copy"));
    CHECK(!fs::exists(legacy / "lists.sqlite") && !fs::exists(legacy / "undo"));
    // The window's stay for the window; what nothing uses stays put.
    CHECK(fs::exists(legacy / "tabs" / "one.json"));
    CHECK(fs::exists(legacy / "before-main-restore-20260914" / "lists.sqlite"));
    CHECK(fs::exists(legacy / "engine.token") && !fs::exists(fresh / "engine.token"));
    CHECK(core::engine_state_directory() == fresh);

    // Once: a second start moves nothing.
    touch(legacy / "lists.sqlite", "left behind");
    CHECK(core::adopt_legacy_engine_state(fresh).value_or(99U) == 0U);
    CHECK(read(fresh / "lists.sqlite") == "the library");

    // A folder named outright is the one used, whatever else there is.
    ::setenv("MELODY_STATE_DIR", (root / "named").c_str(), 1);
    CHECK(core::default_engine_state_directory() == root / "named");
    CHECK(core::engine_state_directory() == root / "named");

    fs::remove_all(root);
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
