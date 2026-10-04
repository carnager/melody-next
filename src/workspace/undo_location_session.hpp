// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "bench/engine_folder_listing.hpp"

#include <QObject>
#include <QString>

#include <cstdint>
#include <memory>
#include <string>

namespace trackknife::engine {
class RemoteFileWork;
}

namespace trackknife::bench {

// ADR-0266: where one engine keeps the files its writes replace, for undo --
// asked of the engine, and changed there: its own folder, a folder chosen on
// its machine, or beside each file. A change is the engine's at once, and it
// moves every undo copy it has there.
class UndoLocationSession final : public QObject {
    Q_OBJECT

  public:
    enum class Place : std::uint8_t { engine, folder, beside };

    // `folders` browses the engine's machine; empty, this computer's file
    // dialog is used.
    UndoLocationSession(std::shared_ptr<engine::RemoteFileWork> work, EngineFolderLister folders,
                        QObject* parent = nullptr);

    [[nodiscard]] bool loaded() const { return loaded_; }
    [[nodiscard]] bool busy() const { return busy_; }
    [[nodiscard]] Place place() const { return place_; }
    // The folder chosen, as raw path and as shown.
    [[nodiscard]] const std::string& folder() const { return folder_; }
    [[nodiscard]] QString folderText() const;
    // Where the engine keeps them now; empty beside each file.
    [[nodiscard]] QString keptIn() const;
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] const EngineFolderLister& folders() const { return folders_; }

    void load();
    // Kept there from now on. A folder needs its path.
    void choose(Place place, std::string folder = {});

  signals:
    void changed();

  private:
    void settle(Place place, std::string folder, std::string kept_in);

    std::shared_ptr<engine::RemoteFileWork> work_;
    EngineFolderLister folders_;
    Place place_{Place::engine};
    std::string folder_;
    std::string kept_in_;
    QString status_;
    bool loaded_{false};
    bool busy_{false};
};

} // namespace trackknife::bench
