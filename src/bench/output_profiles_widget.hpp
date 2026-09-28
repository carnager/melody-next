// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/engine_folder_dialog.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/persistence/list_repository.hpp"

#include <QWidget>

#include <functional>
#include <optional>
#include <string>
#include <vector>

class QLabel;
class QLineEdit;
class QComboBox;
class QPushButton;
class QTabWidget;

namespace trackknife::bench {

// The asynchronous persistence seam shared by the tag editor (profile
// selection) and the Settings screen (profile management). ADR-0185.
// ADR-0237: the move destinations of one engine -- folders on its machine --
// and how a folder there is chosen.
struct DestinationPlace {
    using LoadCompletion =
        std::function<void(std::vector<persistence::SavedDestinationProfile>, QString)>;
    using Completion = std::function<void(QString)>;

    QString key;  // the engine's, as EngineKey spells it
    QString name; // as shown: "this computer", or the engine's name
    std::function<void(LoadCompletion)> load;
    std::function<void(persistence::SavedDestinationProfile, Completion)> save;
    std::function<void(core::StableId, Completion)> remove;
    // Lists that engine's folders; empty for this computer, whose file
    // dialog is used.
    EngineFolderDialog::Lister folders;
    // This computer's destinations that lie under the engine's mount, as
    // that engine names them, for copying there; empty when there is none.
    std::function<std::vector<persistence::SavedDestinationProfile>()> copyable;
};

struct OutputProfileStore {
    using LoadCompletion =
        std::function<void(std::vector<persistence::SavedOutputLayoutProfile>,
                           std::vector<persistence::SavedDestinationProfile>, QString)>;
    using Completion = std::function<void(QString)>;

    // Naming layouts, and the move destinations of the engine `destinations_on`
    // names (empty: this computer).
    std::function<void(LoadCompletion)> load;
    std::function<void(persistence::SavedOutputLayoutProfile, Completion)> save_layout;
    std::function<void(core::StableId, Completion)> remove_layout;
    std::function<void(persistence::SavedDestinationProfile, Completion)> save_destination;
    std::function<void(core::StableId, Completion)> remove_destination;
    QString destinations_on;
    // For the manager: every engine whose destinations can be managed, this
    // computer first. Empty: only the destinations above.
    std::vector<DestinationPlace> places;
};

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

  private:
    void reload();
    void reloadDestinations();
    void selectPlace(int index);
    void copyDestinations();
    void rebuildLists(std::optional<core::StableId> layout_id,
                      std::optional<core::StableId> destination_id);
    void selectLayoutRow(int row);
    void selectDestinationRow(int row);
    void saveLayout();
    void saveDestination();
    void removeLayout();
    void removeDestination();
    void updateButtons();

    OutputProfileStore store_;
    std::vector<DestinationPlace> places_;
    int place_{0};
    std::vector<persistence::SavedOutputLayoutProfile> layouts_;
    std::vector<persistence::SavedDestinationProfile> destinations_;
    std::optional<core::StableId> editing_layout_id_;
    std::optional<core::StableId> editing_destination_id_;
    std::string destination_root_raw_path_;
    bool loading_{false};
    bool mutation_running_{false};

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
