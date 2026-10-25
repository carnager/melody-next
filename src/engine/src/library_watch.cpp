// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/library_watch.hpp"

#include "trackknife/core/local_sources.hpp"
#include "trackknife/watch/watch.hpp"

#include <sys/stat.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace trackknife::engine {
namespace {

using Clock = std::chrono::steady_clock;

// Changes are taken together: once a second goes by without one, or after
// five seconds of them -- an album copied in is re-read as one.
[[nodiscard]] std::chrono::milliseconds quiet() {
    // Tests wait less.
    const auto* named = std::getenv("MELODY_WATCH_QUIET_MS"); // NOLINT(concurrency-mt-unsafe)
    return std::chrono::milliseconds{named != nullptr ? std::atoi(named) : 1'000};
}
constexpr auto at_most = std::chrono::seconds{5};
// How often the library's folders are looked at again: one added, a share
// mounted over its mountpoint since.
constexpr auto look_again = std::chrono::seconds{10};
constexpr std::size_t part = 500U;

void say(const std::string& text) { std::cerr << "melodyd: " << text << "\n"; }

} // namespace

void LibraryWatch::start() {
    if (thread_.joinable()) {
        return;
    }
    thread_ = std::thread{[this, token = stopped_.token()] { run(token); }};
}

void LibraryWatch::stop() {
    stopped_.request_cancellation();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void LibraryWatch::run(const core::CancellationToken& token) {
    auto made = watch::TreeWatcher::create();
    if (!made) {
        say("cannot watch the library for changes: " + made.error().message);
        return;
    }
    auto tree = std::move(*made);
    const auto settle = quiet();
    // Each library folder watched, by the device it was on then: a share
    // mounted over its empty mountpoint since is watched again.
    std::map<std::string, std::uint64_t> watched;
    bool said_exhausted = false;
    std::optional<Clock::time_point> next_look;
    std::set<std::string> pending;
    bool overflow = false;
    std::optional<Clock::time_point> first;
    Clock::time_point last{};
    while (!token.is_cancellation_requested()) {
        if (const auto now = Clock::now(); !next_look || now >= *next_look) {
            next_look = now + look_again;
            if (auto roots = catalogue_.roots()) {
                for (const auto& root : *roots) {
                    struct stat status{};
                    if (::stat(root.raw_path.c_str(), &status) != 0) {
                        continue;
                    }
                    const auto device = static_cast<std::uint64_t>(status.st_dev);
                    if (const auto known = watched.find(root.raw_path);
                        known != watched.end() && known->second == device) {
                        continue;
                    }
                    const auto before = tree->watched();
                    if (tree->add_tree(root.raw_path)) {
                        watched[root.raw_path] = device;
                        say("watching " + root.raw_path + " for changes (" +
                            std::to_string(tree->watched() - before) + " folders)");
                    }
                }
            }
            if (tree->exhausted() && !said_exhausted) {
                said_exhausted = true;
                say("not every library folder is watched: the system's limit on watches "
                    "(fs.inotify.max_user_watches) was reached; raise it, or Refresh to see "
                    "changes in the others");
            }
        }
        for (auto& change : tree->read(std::chrono::milliseconds{250})) {
            if (change.kind == watch::Change::Kind::overflow) {
                overflow = true;
            } else if (change.kind == watch::Change::Kind::folder_gone ||
                       core::is_audio_path(change.path)) {
                pending.insert(std::move(change.path));
            } else {
                continue;
            }
            last = Clock::now();
            if (!first) {
                first = last;
            }
        }
        if (!first) {
            continue;
        }
        if (const auto now = Clock::now(); now - last < settle && now - *first < at_most) {
            continue;
        }
        first.reset();
        if (overflow) {
            // Changes were lost: only a walk finds them, and a walk reads
            // only what changed.
            overflow = false;
            pending.clear();
            say("changes were lost (inotify queue overflow); refreshing the library");
            persistence::LibraryScanProgress progress;
            if (auto scanned = catalogue_.scan(token, progress); !scanned) {
                say("refreshing the library failed: " + scanned.error().message);
            }
            continue;
        }
        const std::vector<std::string> paths{pending.begin(), pending.end()};
        pending.clear();
        for (std::size_t at = 0; at < paths.size() && !token.is_cancellation_requested();
             at += part) {
            const auto end = paths.begin() + static_cast<std::ptrdiff_t>(
                                                  std::min(paths.size(), at + part));
            const std::vector<std::string> some{paths.begin() + static_cast<std::ptrdiff_t>(at),
                                                end};
            if (auto refreshed = catalogue_.refresh(some, token, true); !refreshed) {
                say("could not re-read changed files: " + refreshed.error().message);
            }
        }
    }
}

} // namespace trackknife::engine
