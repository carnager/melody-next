// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/engine_folder_dialog.hpp"
#include "bench/output_profile_store.hpp"
#include "workspace/profiles_session.hpp"

#include <QWidget>

class QLabel;
class QLineEdit;
class QComboBox;
class QPushButton;
class QTabWidget;
class QTreeWidget;

namespace trackknife::bench {

// ADR-0185/0264: the naming-layout and move-destination managers, two tabs
// of the Settings screen's File operations page, added to `tabs`. Every
// engine's destinations in one list, under each engine's name;
// store-backed CRUD; emits profilesChanged so open tag editors can refresh
// their selectors.
class OutputProfilesManager final : public QObject {
    Q_OBJECT

  signals:
    void profilesChanged();

  public:
    OutputProfilesManager(OutputProfileStore store, QTabWidget* tabs);

    // The move destinations of the engine `key` names, shown.
    void showDestinationsOf(const QString& key);
    // The naming layouts shown.
    void showNamingLayouts();

  private:
    void sync();
    void rebuildLists();
    void updateButtons();

    ProfilesSession* session_{nullptr};
    bool syncing_{false};

    QTreeWidget* layout_list_{nullptr};
    QLineEdit* layout_name_{nullptr};
    QLineEdit* directory_expression_{nullptr};
    QLineEdit* basename_expression_{nullptr};
    QComboBox* sanitization_policy_{nullptr};
    QPushButton* layout_new_{nullptr};
    QPushButton* layout_save_{nullptr};
    QPushButton* layout_remove_{nullptr};
    QTabWidget* sections_{nullptr};
    QWidget* layouts_page_{nullptr};
    QWidget* destinations_page_{nullptr};
    QPushButton* destination_copy_{nullptr};
    QTreeWidget* destination_list_{nullptr};
    QLineEdit* destination_name_{nullptr};
    QLineEdit* destination_root_{nullptr};
    QPushButton* destination_browse_{nullptr};
    QPushButton* destination_new_{nullptr};
    QPushButton* destination_save_{nullptr};
    QPushButton* destination_remove_{nullptr};
    QLabel* layouts_status_{nullptr};
    QLabel* destinations_status_{nullptr};
};

} // namespace trackknife::bench
