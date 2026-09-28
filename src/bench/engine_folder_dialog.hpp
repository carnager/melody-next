// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <QByteArray>
#include <QDialog>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class QLabel;
class QListWidget;
class QPushButton;

namespace trackknife::bench {

// ADR-0237: choosing a folder on an engine's machine -- a move destination
// there. The file dialog only knows this computer's disk, so for an engine
// elsewhere its folders are listed by the engine (folders.list) and browsed
// here: double-click to go in, Up to go out, Choose for the folder shown or
// the one selected in it. Titled for the engine, as everything per engine is.
class EngineFolderDialog final : public QDialog {
    Q_OBJECT

  public:
    struct Listing {
        std::string path;
        std::optional<std::string> parent;
        std::vector<std::string> folders;
    };
    using ListingCompletion = std::function<void(core::Result<Listing>)>;
    // Lists a folder (empty: where the engine starts) and answers on this
    // dialog's thread.
    using Lister = std::function<void(std::string, ListingCompletion)>;

    EngineFolderDialog(QString engine_name, Lister lister, std::string start, QWidget* parent);

  signals:
    // The raw path, byte for byte.
    void folderChosen(const QByteArray& raw_path);

  public:
    // The folder Choose would take, as raw bytes.
    [[nodiscard]] std::string choice() const;
    void browse(std::string path);

  private:
    void present(const Listing& listing);

    Lister lister_;
    std::optional<Listing> shown_;
    std::uint64_t request_{0};
    QLabel* path_{nullptr};
    QListWidget* folders_{nullptr};
    QPushButton* up_{nullptr};
    QPushButton* choose_{nullptr};
    QLabel* status_{nullptr};
};

} // namespace trackknife::bench
