// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QWidget>

class QButtonGroup;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;

namespace trackknife::bench {

class PlaylistFolderSession;

// ADR-0296: where one engine's playlists are, under its music folders in
// Settings › Engines: Playlists in the music folder, or a folder chosen on
// its machine -- MPD's, say -- and how the files it writes name their
// tracks. A choice is the engine's at once.
class PlaylistFolderWidget final : public QWidget {
    Q_OBJECT
  public:
    // `engine_name` names the machine a folder is chosen on.
    PlaylistFolderWidget(PlaylistFolderSession& session, QString engine_name,
                         QWidget* parent = nullptr);

  private:
    void sync();
    void chooseFolder();

    PlaylistFolderSession& session_;
    QString engine_name_;
    QButtonGroup* places_;
    QRadioButton* default_;
    QRadioButton* chosen_;
    QLineEdit* chosen_path_;
    QPushButton* choose_;
    QComboBox* style_;
    QLabel* status_;
};

} // namespace trackknife::bench
