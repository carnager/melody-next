// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace trackknife::watch {

// ADR-0232: a folder as the watcher's machine sees it, and as the engine does
// -- the NAS's /volume1/music is the engine's /mnt/nas/music. The same path
// on both when they agree.
struct FolderMapping {
    std::string local;
    std::string engine;
};

// "LOCAL" or "LOCAL=ENGINE". Both absolute; trailing slashes are dropped. A
// path may itself contain '=', so the split is at the first "=/".
[[nodiscard]] std::optional<FolderMapping> parse_mapping(std::string_view text);

// Whether `path` is `folder` or inside it.
[[nodiscard]] bool contains(std::string_view folder, std::string_view path);

// A path under mapping.local named as the engine names it; nullopt when it
// is not under mapping.local.
[[nodiscard]] std::optional<std::string> to_engine(const FolderMapping& mapping,
                                                   std::string_view local_path);

// A file as it is on the watcher's machine now, named as the engine names it.
struct PresentFile {
    std::string path;
    std::uint64_t size{0};
    std::int64_t modified_seconds{0};
};

// A file as the engine indexed it (catalogue.inventory).
struct IndexedFile {
    std::string path;
    std::uint64_t size{0};
    std::int64_t modified_seconds{0};
    bool available{true};
};

struct CatchUpPlan {
    // What the engine should re-read: new, changed and -- unless withheld --
    // vanished files.
    std::vector<std::string> refresh;
    std::size_t added{0};
    std::size_t changed{0};
    std::size_t vanished{0};
    // Most of what is indexed seems to be gone: a wrong mapping or a missing
    // mount, far more likely than a deleted library. The vanished files are
    // then left out of `refresh`, and whoever runs this says so.
    bool vanished_withheld{false};
};

// ADR-0232: what changed while nobody was watching, by comparing the folder
// with the index. Neither side needs to be sorted.
[[nodiscard]] CatchUpPlan plan_catch_up(std::vector<PresentFile> present,
                                        std::vector<IndexedFile> indexed);

// Every audio file under `folder` on this machine, named for the engine.
// Symlinked folders are not followed, as the engine's scan does not.
[[nodiscard]] std::vector<PresentFile> present_files(const FolderMapping& mapping);

// A change in a watched tree, by its path on this machine.
struct Change {
    enum class Kind : std::uint8_t {
        // A file written, created, moved in or out, or deleted: which of
        // those it was is for the engine to find out by looking.
        file,
        // A folder moved out of the tree or deleted as a whole.
        folder_gone,
        // The kernel's queue overflowed: changes were lost, and only
        // comparing the tree with the index finds them again.
        overflow,
    };
    Kind kind{Kind::file};
    std::string path;

    friend bool operator==(const Change&, const Change&) = default;
};

// Recursive inotify watching of folders on this machine. A folder that
// appears -- made, or moved in -- is watched too, and the files already in it
// are reported, since some may have been written before its watch existed.
class TreeWatcher final {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<TreeWatcher>> create();
    TreeWatcher(const TreeWatcher&) = delete;
    TreeWatcher(TreeWatcher&&) = delete;
    TreeWatcher& operator=(const TreeWatcher&) = delete;
    TreeWatcher& operator=(TreeWatcher&&) = delete;
    ~TreeWatcher();

    // Watches `folder` and every folder under it.
    [[nodiscard]] core::Result<void> add_tree(const std::string& folder);
    // Waits up to `timeout` for changes, and returns what arrived.
    [[nodiscard]] std::vector<Change> read(std::chrono::milliseconds timeout);

    [[nodiscard]] std::size_t watched() const noexcept { return paths_.size(); }
    // Whether a folder went unwatched because the kernel's limit on watches
    // (fs.inotify.max_user_watches) was reached.
    [[nodiscard]] bool exhausted() const noexcept { return exhausted_; }

  private:
    explicit TreeWatcher(int descriptor) : descriptor_(descriptor) {}
    // Watches one folder and those under it; with `report`, the files found
    // in them are added to `changes`.
    void watch_tree(const std::string& folder, std::vector<Change>* report);
    void forget_tree(const std::string& folder);

    int descriptor_{-1};
    std::unordered_map<int, std::string> paths_;
    bool exhausted_{false};
};

} // namespace trackknife::watch
