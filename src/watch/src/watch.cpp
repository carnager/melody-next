// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/watch/watch.hpp"

#include "trackknife/core/local_sources.hpp"

#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <utility>

namespace trackknife::watch {
namespace {

[[nodiscard]] std::string without_trailing_slashes(std::string path) {
    while (path.size() > 1U && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

// Folders: what inotify reports for files and folders in them. IN_CREATE
// matters only for folders -- a file created is reported again when it is
// closed after writing.
constexpr std::uint32_t folder_events = IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_FROM |
                                        IN_MOVED_TO | IN_DELETE_SELF | IN_ONLYDIR | IN_DONT_FOLLOW;

} // namespace

std::optional<FolderMapping> parse_mapping(const std::string_view text) {
    std::string local;
    std::string engine;
    if (const auto split = text.find("=/"); split != std::string_view::npos) {
        local = std::string{text.substr(0, split)};
        engine = std::string{text.substr(split + 1U)};
    } else {
        local = std::string{text};
        engine = local;
    }
    local = without_trailing_slashes(std::move(local));
    engine = without_trailing_slashes(std::move(engine));
    if (!local.starts_with('/') || !engine.starts_with('/')) {
        return std::nullopt;
    }
    return FolderMapping{.local = std::move(local), .engine = std::move(engine)};
}

bool contains(const std::string_view folder, const std::string_view path) {
    if (folder == "/") {
        return path.starts_with('/');
    }
    return path == folder ||
           (path.size() > folder.size() && path.starts_with(folder) && path[folder.size()] == '/');
}

std::optional<std::string> to_engine(const FolderMapping& mapping, const std::string_view local_path) {
    if (!contains(mapping.local, local_path)) {
        return std::nullopt;
    }
    const auto rest = mapping.local == "/" ? local_path.substr(1) : local_path.substr(mapping.local.size());
    if (mapping.engine == "/") {
        return rest.starts_with('/') ? std::string{rest} : '/' + std::string{rest};
    }
    return mapping.engine + std::string{rest};
}

CatchUpPlan plan_catch_up(std::vector<PresentFile> present, std::vector<IndexedFile> indexed) {
    std::unordered_map<std::string, IndexedFile> by_path;
    by_path.reserve(indexed.size());
    for (auto& file : indexed) {
        auto path = file.path;
        by_path.emplace(std::move(path), std::move(file));
    }
    CatchUpPlan plan;
    for (auto& file : present) {
        const auto known = by_path.find(file.path);
        if (known == by_path.end()) {
            ++plan.added;
            plan.refresh.push_back(std::move(file.path));
            continue;
        }
        const auto& was = known->second;
        if (!was.available || was.size != file.size ||
            was.modified_seconds != file.modified_seconds) {
            ++plan.changed;
            plan.refresh.push_back(std::move(file.path));
        }
        by_path.erase(known);
    }
    // What is left is indexed and not here any more.
    plan.vanished = by_path.size();
    const auto total = indexed.size();
    plan.vanished_withheld = plan.vanished > 10U && plan.vanished * 2U > total;
    if (!plan.vanished_withheld) {
        for (auto& [path, file] : by_path) {
            static_cast<void>(file);
            plan.refresh.push_back(path);
        }
    }
    std::ranges::sort(plan.refresh);
    return plan;
}

std::vector<PresentFile> present_files(const FolderMapping& mapping) {
    std::vector<PresentFile> files;
    std::error_code error;
    std::filesystem::recursive_directory_iterator walk{
        std::filesystem::path{mapping.local}, std::filesystem::directory_options::skip_permission_denied,
        error};
    for (; !error && walk != std::filesystem::recursive_directory_iterator{}; walk.increment(error)) {
        const auto& raw = walk->path().native();
        if (!core::is_audio_path(raw)) {
            continue;
        }
        struct stat status {};
        if (::lstat(raw.c_str(), &status) != 0 || !S_ISREG(status.st_mode)) {
            continue;
        }
        auto path = to_engine(mapping, raw);
        if (!path) {
            continue;
        }
        files.push_back(PresentFile{.path = std::move(*path),
                                    .size = static_cast<std::uint64_t>(status.st_size),
                                    .modified_seconds = status.st_mtim.tv_sec});
    }
    return files;
}

core::Result<std::unique_ptr<TreeWatcher>> TreeWatcher::create() {
    const auto descriptor = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (descriptor < 0) {
        return std::unexpected(core::Error{.code = core::ErrorCode::io,
                                           .message = "could not start watching",
                                           .context = {{.key = "errno", .value = std::strerror(errno)}}});
    }
    return std::unique_ptr<TreeWatcher>{new TreeWatcher{descriptor}};
}

TreeWatcher::~TreeWatcher() {
    if (descriptor_ >= 0) {
        ::close(descriptor_);
    }
}

core::Result<void> TreeWatcher::add_tree(const std::string& folder) {
    const auto root = without_trailing_slashes(folder);
    std::error_code error;
    if (!std::filesystem::is_directory(std::filesystem::path{root}, error)) {
        return std::unexpected(core::Error{.code = core::ErrorCode::not_found,
                                           .message = "not a folder",
                                           .context = {{.key = "path", .value = root}}});
    }
    watch_tree(root, nullptr);
    return {};
}

void TreeWatcher::watch_tree(const std::string& folder, std::vector<Change>* report) {
    const auto watch_one = [this](const std::string& path) {
        if (exhausted_) {
            return;
        }
        const auto handle = ::inotify_add_watch(descriptor_, path.c_str(), folder_events);
        if (handle < 0) {
            exhausted_ = exhausted_ || errno == ENOSPC;
            return;
        }
        // The same folder watched again -- moved within the tree -- keeps its
        // handle; its path is what changed.
        paths_[handle] = path;
    };
    watch_one(folder);
    std::error_code error;
    std::filesystem::recursive_directory_iterator walk{
        std::filesystem::path{folder}, std::filesystem::directory_options::skip_permission_denied,
        error};
    for (; !error && walk != std::filesystem::recursive_directory_iterator{}; walk.increment(error)) {
        const auto status = walk->symlink_status(error);
        if (error) {
            error.clear();
            continue;
        }
        if (std::filesystem::is_directory(status)) {
            watch_one(walk->path().native());
        } else if (report != nullptr && std::filesystem::is_regular_file(status)) {
            report->push_back(Change{.kind = Change::Kind::file, .path = walk->path().native()});
        }
    }
}

void TreeWatcher::forget_tree(const std::string& folder) {
    for (auto entry = paths_.begin(); entry != paths_.end();) {
        if (contains(folder, entry->second)) {
            ::inotify_rm_watch(descriptor_, entry->first);
            entry = paths_.erase(entry);
        } else {
            ++entry;
        }
    }
}

std::vector<Change> TreeWatcher::read(const std::chrono::milliseconds timeout) {
    std::vector<Change> changes;
    pollfd watched{.fd = descriptor_, .events = POLLIN, .revents = 0};
    if (::poll(&watched, 1, static_cast<int>(timeout.count())) <= 0) {
        return changes;
    }
    alignas(inotify_event) std::array<char, 64U * 1024U> buffer{};
    while (true) {
        const auto length = ::read(descriptor_, buffer.data(), buffer.size());
        if (length <= 0) {
            break;
        }
        for (std::size_t offset = 0; offset < static_cast<std::size_t>(length);) {
            inotify_event event{};
            std::memcpy(&event, buffer.data() + offset, sizeof(event));
            const char* name = buffer.data() + offset + sizeof(inotify_event);
            offset += sizeof(inotify_event) + event.len;

            if ((event.mask & IN_Q_OVERFLOW) != 0U) {
                changes.push_back(Change{.kind = Change::Kind::overflow, .path = {}});
                continue;
            }
            if ((event.mask & IN_IGNORED) != 0U) {
                paths_.erase(event.wd);
                continue;
            }
            const auto folder = paths_.find(event.wd);
            if (folder == paths_.end() || event.len == 0U) {
                continue;
            }
            const auto path = folder->second + '/' + std::string{name};
            const bool is_folder = (event.mask & IN_ISDIR) != 0U;
            if (is_folder) {
                if ((event.mask & (IN_CREATE | IN_MOVED_TO)) != 0U) {
                    // Made or moved in: watched from now on, and what it
                    // already holds is news.
                    watch_tree(path, &changes);
                } else if ((event.mask & (IN_MOVED_FROM | IN_DELETE)) != 0U) {
                    // Moved out, or away within the tree -- its new place,
                    // if it has one here, arrives as IN_MOVED_TO.
                    forget_tree(path);
                    changes.push_back(Change{.kind = Change::Kind::folder_gone, .path = path});
                }
                continue;
            }
            if ((event.mask & (IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE)) != 0U) {
                changes.push_back(Change{.kind = Change::Kind::file, .path = path});
            }
        }
    }
    return changes;
}

} // namespace trackknife::watch
