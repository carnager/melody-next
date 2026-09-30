// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/engine_folder_listing.hpp"

#include <QObject>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>
#include <string>

namespace trackknife::bench {

// ADR-0237: choosing a folder on an engine's machine, as that engine lists
// it -- down into a folder, up to its parent, and the folder shown or one
// in it taken. Both windows' folder choosers draw it.
class EngineFolderSession final : public QObject {
    Q_OBJECT

  public:
    EngineFolderSession(EngineFolderLister lister, std::string start, QObject* parent = nullptr);

    [[nodiscard]] QString path() const;
    [[nodiscard]] QStringList folders() const;
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] bool canGoUp() const;
    [[nodiscard]] bool canChoose() const;
    // The folder Choose takes, as raw bytes: `row` in the one shown, or
    // (-1) the one shown.
    [[nodiscard]] std::string choice(int row) const;

    void browse(std::string path);
    void open(int row);
    void up();

  signals:
    void changed();

  private:
    EngineFolderLister lister_;
    std::optional<EngineFolderListing> shown_;
    std::uint64_t request_{0};
    bool listing_{false};
    QString status_;
};

} // namespace trackknife::bench
