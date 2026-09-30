// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/engine_key.hpp"

#include "bench/catalogue_source.hpp"

#include "bench/local_library_panel.hpp"
#include "bench/local_list_model.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "trackknife/query/search_presets.hpp"
#include "trackknife/query/tkq.hpp"
#include "workspace/search_session.hpp"

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
    using TabSnapshot = SearchSession::TabSnapshot;
    using TabAccess = SearchSession::TabAccess;
    using TechnicalsSink = SearchSession::TechnicalsSink;
    using OtherLibrary = SearchSession::OtherLibrary;

    SearchDialog(const CatalogueSource& catalogues, TabAccess tab_access,
                 TechnicalsSink technicals_sink, QWidget* parent = nullptr,
                 std::vector<OtherLibrary> others = {});
    ~SearchDialog() override;

    void watchCurrentModel(QAbstractItemModel* model) { session_->watchCurrentModel(model); }
    void focusInput();
    // The library of the tab it was opened from: its engine's. "Current tab"
    // is kept.
    void followLibrary(const EngineKey& engine) { session_->followLibrary(engine); }

  protected:
    void showEvent(QShowEvent* event) override;

  signals:
    // Every scope carries cached rows directly; opening never starts file
    // discovery. `engine`: whose library the rows are, for a tab of that
    // engine.
    void rowsRequested(QString name, std::vector<LocalTrackRow> rows, LocalLibraryAction action,
                       const trackknife::ui::EngineKey& engine);

  private:
    void populatePresets(QMenu* menu);
    void usePreset(int preset);
    void sync();
    void syncSaved();
    void syncResults();
    void openSelected(LocalLibraryAction action);

    SearchSession* session_;
    QComboBox* saved_searches_{nullptr};
    QPushButton* save_search_{nullptr};
    QPushButton* update_search_{nullptr};
    QPushButton* rename_search_{nullptr};
    QPushButton* delete_search_{nullptr};
    QLabel* saved_status_{nullptr};
    QComboBox* scope_{nullptr};
    QLineEdit* input_{nullptr};
    QCheckBox* query_mode_{nullptr};
    QLabel* error_{nullptr};
    QListWidget* results_{nullptr};
    QLabel* status_{nullptr};
    QPushButton* open_button_{nullptr};
};

} // namespace trackknife::bench
