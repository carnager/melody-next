// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/engine/job_registry.hpp"
#include "trackknife/lists/playlist_file.hpp"
#include "trackknife/protocol/dispatch.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace trackknife::engine {

class LocalCatalogue;
class Workspace;

// ADR-0296: an engine's saved playlists are the files in its playlist
// folder. The engine's list store is their index, as the library is its
// files': a saved list changed by anyone -- a client's edit, a rename, a
// move followed -- is written to its file, and a file another program put,
// changed or took away is read into its list, made or dropped.
//
// What it keeps of its own -- the folder chosen, the style new files name
// their tracks in, and which file is which list -- is in `state_file`.
class PlaylistFolder {
  public:
    PlaylistFolder(Workspace& workspace, const LocalCatalogue& catalogue,
                   std::filesystem::path state_file, EventSink sink);
    PlaylistFolder(const PlaylistFolder&) = delete;
    PlaylistFolder& operator=(const PlaylistFolder&) = delete;
    ~PlaylistFolder();

    // The sink lists announce their changes through: each passes on, and a
    // saved list's change is written to its file.
    [[nodiscard]] EventSink list_sink();

    // Reads the folder, on a thread of its own, and then every `every` until
    // it goes.
    void start(std::chrono::milliseconds every);
    // Reads the folder: what another program did there becomes the lists'.
    void sync();

    // The folder its playlists are in: the one chosen, else Playlists in
    // the first music folder; none without either.
    [[nodiscard]] std::optional<std::filesystem::path> folder() const;
    [[nodiscard]] protocol::Json describe() const;
    // Another folder -- empty, the default -- or style. The lists are
    // written to a new folder, and its files read.
    [[nodiscard]] core::Result<protocol::Json> configure(const protocol::Json& params);

  private:
    struct Known {
        std::string id;
        std::int64_t modified_ns{0};
        std::uintmax_t size{0};
        lists::PlaylistPathStyle style{lists::PlaylistPathStyle::root_relative};
    };

    void changed(const core::StableId& id);
    void sync_locked();
    void write_locked(const core::StableId& id);
    void load_state();
    void save_state_locked() const;
    [[nodiscard]] std::optional<std::filesystem::path> folder_locked() const;
    [[nodiscard]] std::vector<std::string> music_roots() const;

    Workspace& workspace_;
    const LocalCatalogue& catalogue_;
    std::filesystem::path state_file_;
    EventSink sink_;

    mutable std::recursive_mutex mutex_;
    std::string chosen_folder_;
    lists::PlaylistPathStyle style_{lists::PlaylistPathStyle::root_relative};
    // The folder the index is of, and which file -- by name -- is which list.
    std::string indexed_folder_;
    std::map<std::string, Known> files_;
    // While the folder is read: what it says is its files' already.
    bool syncing_{false};

    std::mutex wake_mutex_;
    std::condition_variable wake_;
    bool stopping_{false};
    std::thread poller_;
};

// ADR-0296:
//   playlists.folder {}                        -> {folder, default, chosen, style}
//   playlists.set_folder {folder?, style?}     -> the same, the folder read
//   playlists.sync {}                          -> {} once the folder is read
// `style` is "root" (relative to the music folder, MPD's), "playlist"
// (relative to the playlist) or "absolute"; a file already there keeps its own.
void register_playlist_folder_methods(protocol::Dispatcher& dispatcher, PlaylistFolder& folder);

} // namespace trackknife::engine
