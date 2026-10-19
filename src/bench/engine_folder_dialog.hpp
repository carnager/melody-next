// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/engine_folder_listing.hpp"
#include "trackknife/core/result.hpp"
#include "workspace/engine_folder_session.hpp"

#include <QByteArray>
#include <QDialog>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class QCheckBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace trackknife::bench {

// ADR-0237: choosing a folder on an engine's machine -- a move destination
// there, its playlist folder, its undo copies. The file dialog only knows
// this computer's disk, so for an engine elsewhere its folders are listed by
// the engine (folders.list) and browsed here: a place to start -- home, a
// music folder, the root -- a step of the path, or a path typed; Enter or a
// double-click to go in, Backspace or Up to go out; New folder… where the
// engine can; Choose for the folder shown or the one selected in it. Titled
// for the engine, as everything per engine is.
class EngineFolderDialog final : public QDialog {
    Q_OBJECT

  public:
    using Listing = EngineFolderListing;
    using ListingCompletion = EngineFolderListingCompletion;
    using Lister = EngineFolderLister;

    EngineFolderDialog(QString engine_name, Lister lister, std::string start, QWidget* parent);

  signals:
    // The raw path, byte for byte.
    void folderChosen(const QByteArray& raw_path);

  public:
    // The folder Choose would take, as raw bytes.
    [[nodiscard]] std::string choice() const;
    void browse(std::string path);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void sync();

    void makeFolder();

    EngineFolderSession* session_{nullptr};
    QLineEdit* path_{nullptr};
    QHBoxLayout* crumbs_{nullptr};
    QListWidget* places_{nullptr};
    QListWidget* folders_{nullptr};
    QPushButton* up_{nullptr};
    QPushButton* make_{nullptr};
    QCheckBox* hidden_{nullptr};
    QPushButton* choose_{nullptr};
    QLabel* status_{nullptr};
};

} // namespace trackknife::bench
