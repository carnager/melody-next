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

namespace trackknife::bench {

// ADR-0185: the naming-layout and move-destination managers, hosted by the
// Settings screen's Naming page. Compact preset selection, store-backed CRUD;
// emits profilesChanged so open tag editors can refresh their selectors.
class OutputProfilesManagerWidget final : public QWidget {
    Q_OBJECT

  signals:
    void profilesChanged();

  public:
    explicit OutputProfilesManagerWidget(OutputProfileStore store, QWidget* parent = nullptr);

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

    QComboBox* layout_list_{nullptr};
    QLineEdit* layout_name_{nullptr};
    QLineEdit* directory_expression_{nullptr};
    QLineEdit* basename_expression_{nullptr};
    QComboBox* sanitization_policy_{nullptr};
    QPushButton* layout_new_{nullptr};
    QPushButton* layout_save_{nullptr};
    QPushButton* layout_remove_{nullptr};
    QTabWidget* sections_{nullptr};
    QComboBox* place_list_{nullptr};
    QPushButton* destination_copy_{nullptr};
    QComboBox* destination_list_{nullptr};
    QLineEdit* destination_name_{nullptr};
    QLineEdit* destination_root_{nullptr};
    QPushButton* destination_browse_{nullptr};
    QPushButton* destination_new_{nullptr};
    QPushButton* destination_save_{nullptr};
    QPushButton* destination_remove_{nullptr};
    QLabel* status_{nullptr};
};

} // namespace trackknife::bench
