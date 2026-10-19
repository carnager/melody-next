// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/engine_folder_listing.hpp"

#include <QObject>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::bench {

// ADR-0237: choosing a folder on an engine's machine, as that engine lists
// it -- down into a folder, up to its parent, to a place it names (home, a
// music folder, the root) or a path typed, and the folder shown or one in
// it taken. Hidden folders are shown when asked; a folder is made there
// where the engine can. The window's folder chooser draws it.
class EngineFolderSession final : public QObject {
    Q_OBJECT

  public:
    EngineFolderSession(EngineFolderLister lister, std::string start, QObject* parent = nullptr);

    struct Place {
        QString name;
        std::string path;
    };

    [[nodiscard]] QString path() const;
    // The folders shown, and which of them are links.
    [[nodiscard]] QStringList folders() const;
    [[nodiscard]] bool linked(int row) const;
    // Where browsing starts: home, the music folders, the root.
    [[nodiscard]] std::vector<Place> places() const;
    // The folder shown, a step for each folder down to it.
    [[nodiscard]] std::vector<Place> crumbs() const;
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] bool canGoUp() const;
    [[nodiscard]] bool canChoose() const;
    [[nodiscard]] bool canMake() const;
    [[nodiscard]] bool showsHidden() const { return show_hidden_; }
    // The folder Choose takes, as raw bytes: `row` in the one shown, or
    // (-1) the one shown.
    [[nodiscard]] std::string choice(int row) const;

    void browse(std::string path);
    void open(int row);
    void up();
    void setShowHidden(bool shown);
    // A folder `name` made in the one shown, and gone into.
    void make(const QString& name);

  signals:
    void changed();

  private:
    void refilter();
    [[nodiscard]] std::optional<std::size_t> indexOf(int row) const;

    EngineFolderLister lister_;
    std::optional<EngineFolderListing> shown_;
    // Of the folders shown, which rows are which -- the hidden left out.
    std::vector<std::size_t> visible_;
    bool show_hidden_{false};
    std::uint64_t request_{0};
    bool listing_{false};
    QString status_;
};

} // namespace trackknife::bench
