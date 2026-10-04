// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QWidget>

class QButtonGroup;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;

namespace trackknife::bench {

class UndoLocationSession;

// ADR-0266: where one engine keeps its undo copies, in Settings › File
// operations › Undo: its own folder, a folder chosen on its machine, or
// beside each file. A choice is the engine's at once.
class UndoLocationWidget final : public QWidget {
    Q_OBJECT
  public:
    // `engine_name` names the machine a folder is chosen on.
    UndoLocationWidget(UndoLocationSession& session, QString engine_name,
                       QWidget* parent = nullptr);

  private:
    void sync();
    void chooseFolder();

    UndoLocationSession& session_;
    QString engine_name_;
    QButtonGroup* places_;
    QRadioButton* engine_;
    QLabel* engine_folder_;
    QRadioButton* folder_;
    QLineEdit* folder_path_;
    QPushButton* choose_;
    QRadioButton* beside_;
    QLabel* note_;
    QLabel* status_;
};

} // namespace trackknife::bench
