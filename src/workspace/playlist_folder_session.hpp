// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "bench/engine_folder_listing.hpp"

#include <QObject>
#include <QString>

#include <memory>
#include <optional>
#include <string>

namespace trackknife::engine {
class RemoteFileWork;
}

namespace trackknife::bench {

// ADR-0296: one engine's playlist folder -- its saved playlists are the
// files there -- asked of the engine and changed there: Playlists in its
// first music folder, or a folder chosen on its machine; and how the files
// it writes name their tracks.
class PlaylistFolderSession final : public QObject {
    Q_OBJECT

  public:
    // `folders` browses the engine's machine; empty, this computer's file
    // dialog is used.
    PlaylistFolderSession(std::shared_ptr<engine::RemoteFileWork> work, EngineFolderLister folders,
                          QObject* parent = nullptr);

    [[nodiscard]] bool loaded() const { return loaded_; }
    [[nodiscard]] bool busy() const { return busy_; }
    // The folder chosen -- empty, the default -- and the folders as shown.
    [[nodiscard]] const std::string& chosen() const { return chosen_; }
    [[nodiscard]] QString chosenText() const;
    [[nodiscard]] QString defaultText() const;
    [[nodiscard]] QString folderText() const;
    // "root", "playlist" or "absolute".
    [[nodiscard]] const std::string& style() const { return style_; }
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] const EngineFolderLister& folders() const { return folders_; }

    void load();
    // Another folder -- empty, the default -- or style.
    void choose(std::optional<std::string> folder, std::optional<std::string> style = {});

  signals:
    void changed();

  private:
    std::shared_ptr<engine::RemoteFileWork> work_;
    EngineFolderLister folders_;
    std::string folder_;
    std::string default_;
    std::string chosen_;
    std::string style_{"root"};
    QString status_;
    bool loaded_{false};
    bool busy_{false};
};

} // namespace trackknife::bench
