// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/local_library_panel.hpp"
#include "bench/local_list_model.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "trackknife/query/search_presets.hpp"
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
class QMenu;

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

    // The optional server-library scope: the window translates the tkq query
    // for the connected server and reports labels or a typed refusal, so the
    // dialog stays protocol-agnostic. Open hands the query back to the
    // window, which owns MPD search tabs.
    struct ServerScope {
        std::function<bool()> available;
        std::function<void(query::CompiledTkq compiled,
                           std::function<void(QStringList labels, int total, QString error)>)>
            run;
        std::function<void(query::CompiledTkq compiled, QString query_text)> open;
        std::function<bool()> current_available{};
        std::function<void(query::CompiledTkq, std::function<void(QStringList, int, QString)>)>
            run_current{};
        std::function<void(query::CompiledTkq, QString)> open_current{};
        std::function<QString(const query::CompiledTkq&, bool current)> unsupported_reason{};
    };

    SearchDialog(std::filesystem::path database_path, TabAccess tab_access,
                 TechnicalsSink technicals_sink, ServerScope server_scope,
                 QWidget* parent = nullptr);
    SearchDialog(std::filesystem::path database_path, TabAccess tab_access,
                 TechnicalsSink technicals_sink, QWidget* parent = nullptr);
    ~SearchDialog() override;

    // Opens on the scope that matches where the search was started: a
    // server-side tab searches the server library.
    void preferServerScope();
    void watchCurrentModel(QAbstractItemModel* model);
    void focusInput();

  protected:
    void showEvent(QShowEvent* event) override;

  signals:
    // Both scopes carry cached rows directly; opening never starts file discovery.
    void rowsRequested(QString name, std::vector<LocalTrackRow> rows, LocalLibraryAction action);

  private:
    void populatePresets(QMenu* menu);
    void usePreset(const query::SearchPreset& preset);
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
    [[nodiscard]] bool serverScope() const;
    [[nodiscard]] bool serverCurrentScope() const;
    void startServerSearch(query::CompiledTkq compiled);

    std::filesystem::path database_path_;
    TabAccess tab_access_;
    TechnicalsSink technicals_sink_;
    ServerScope server_scope_;
    std::vector<QMetaObject::Connection> current_model_connections_;
    std::optional<query::CompiledTkq> server_result_query_;
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
