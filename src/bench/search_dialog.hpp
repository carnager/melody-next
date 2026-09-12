// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/local_library_panel.hpp"
#include "bench/local_list_model.hpp"
#include "trackknife/core/cancellation.hpp"
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
    // Database-scope results resolve to paths for the discovery pipeline.
    void resultsRequested(QString name, std::vector<std::string> raw_paths,
                          LocalLibraryAction action);
    // Tab-scope results carry the matched rows themselves.
    void rowsRequested(QString name, std::vector<LocalTrackRow> rows, LocalLibraryAction action);

  private:
    struct Outcome {
        std::vector<std::string> labels;
        std::vector<std::string> paths;
        std::vector<LocalTrackRow> rows;
        std::vector<std::pair<std::string, LocalTrackTechnicals>> probed;
        QString error;
        bool more{false};
        std::size_t scanned{0U};
    };

    void scheduleSearch();
    void startSearch();
    void finishSearch();
    void openResults(LocalLibraryAction action, bool selection_only);
    [[nodiscard]] std::optional<query::CompiledTkq> compileInput();
    [[nodiscard]] bool databaseScope() const;

    std::filesystem::path database_path_;
    TabAccess tab_access_;
    TechnicalsSink technicals_sink_;
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
    std::vector<std::string> result_paths_;
    std::vector<LocalTrackRow> result_rows_;
    QString result_query_;
};

} // namespace trackknife::bench
