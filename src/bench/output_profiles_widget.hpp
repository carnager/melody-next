// SPDX-License-Identifier: GPL-3.0-only

#pragma once

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

namespace trackknife::bench {

// The asynchronous persistence seam shared by the tag editor (profile
// selection) and the Settings screen (profile management). ADR-0185.
struct OutputProfileStore {
    using LoadCompletion =
        std::function<void(std::vector<persistence::SavedOutputLayoutProfile>,
                           std::vector<persistence::SavedDestinationProfile>, QString)>;
    using Completion = std::function<void(QString)>;

    std::function<void(LoadCompletion)> load;
    std::function<void(persistence::SavedOutputLayoutProfile, Completion)> save_layout;
    std::function<void(core::StableId, Completion)> remove_layout;
    std::function<void(persistence::SavedDestinationProfile, Completion)> save_destination;
    std::function<void(core::StableId, Completion)> remove_destination;
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

  private:
    void reload();
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
