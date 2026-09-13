// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/local_library_panel.hpp"
#include "bench/local_list_model.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "trackknife/query/tkq.hpp"

#include <QDialog>
#include <QFutureWatcher>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTimer;

namespace trackknife::bench {

// ADR-0153: the standalone search surface. One tkq/word query, two
// scopes: the library database through LocalLibrary::filter, or a
// snapshot of the current local tab through the shared row evaluation
// (probing missing technicals on demand).
class SearchDialog final : public QDialog {
    Q_OBJECT

  public:
    struct TabSnapshot {
        QString title;
        std::vector<LocalTrackRow> rows;
    };
    using TabAccess = std::function<std::optional<TabSnapshot>()>;
    using TechnicalsSink = std::function<void(std::string, LocalTrackTechnicals)>;

    SearchDialog(std::filesystem::path database_path, TabAccess tab_access,
                 TechnicalsSink technicals_sink, QWidget* parent = nullptr);
    ~SearchDialog() override;

  signals:
    // Both scopes carry cached rows directly; opening never starts file discovery.
    void rowsRequested(QString name, std::vector<LocalTrackRow> rows, LocalLibraryAction action);

  private:
    struct Outcome {
        std::vector<std::string> labels;
        std::vector<LocalTrackRow> rows;
        std::vector<std::pair<std::string, LocalTrackTechnicals>> probed;
        QString error;
        std::size_t scanned{0U};
    };

    void loadSavedSearches(std::optional<persistence::SavedSearch> write = std::nullopt,
                           bool remove = false);
    void finishSavedSearches();
    void useSavedSearch(int index);
    void saveSearch(bool update);
    void renameSearch();
    void deleteSearch();
    void updateSavedSearchButtons();
    [[nodiscard]] std::optional<persistence::SavedSearch> selectedSearch() const;

    void scheduleSearch();
    void startSearch();
    void finishSearch();
    void openResults(LocalLibraryAction action, bool selection_only);
    [[nodiscard]] std::optional<query::CompiledTkq> compileInput();
    [[nodiscard]] bool databaseScope() const;

    std::filesystem::path database_path_;
    TabAccess tab_access_;
    TechnicalsSink technicals_sink_;
    QComboBox* saved_searches_{nullptr};
    QPushButton* save_search_{nullptr};
    QPushButton* update_search_{nullptr};
    QPushButton* rename_search_{nullptr};
    QPushButton* delete_search_{nullptr};
    QLabel* saved_status_{nullptr};
    std::vector<persistence::SavedSearch> catalog_;
    QFutureWatcher<core::Result<std::vector<persistence::SavedSearch>>> catalog_watcher_;
    std::optional<core::StableId> catalog_selection_;
    bool catalog_busy_{false};
    bool catalog_ready_{false};
    std::size_t search_job_generation_{0U};
    QComboBox* scope_{nullptr};
    QLineEdit* input_{nullptr};
    QCheckBox* query_mode_{nullptr};
    QLabel* error_{nullptr};
    QListWidget* results_{nullptr};
    QLabel* status_{nullptr};
    QPushButton* open_button_{nullptr};
    QTimer* debounce_{nullptr};
    QFutureWatcher<Outcome> watcher_;
    core::CancellationSource cancellation_;
    std::size_t generation_{0U};
    bool searching_{false};
    // The last successful search's full result payload.
    std::vector<LocalTrackRow> result_rows_;
    QString result_query_;
};

} // namespace trackknife::bench
