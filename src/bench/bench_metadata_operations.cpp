// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"
#include "bench/remote_mount.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "bench/convert_dialog.hpp"
#include "bench/local_library_panel.hpp"
#include "bench/metadata_properties_dialog.hpp"
#include "bench/post_back.hpp"
#include "bench/preparation_feedback_dialog.hpp"
#include "bench/replaygain_dialog.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/operations/file_publication_apply.hpp"
#include "trackknife/operations/metadata_commit.hpp"
#include "uicommon/list_persistence_service.hpp"

#include <QAbstractItemView>
#include <QAbstractTableModel>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>
#include <QPromise>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <QToolButton>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace trackknife::bench {

void BenchMainWindow::showConvertDialog() {
    auto* tab = currentListTab();
    showConvertForView(tab ? tab->view : nullptr);
}

void BenchMainWindow::showConvertForView(QTableView* view) {
    auto* model = view ? qobject_cast<LocalListModel*>(view->model()) : nullptr;
    if (model == nullptr || !view->selectionModel()) {
        return;
    }
    std::vector<int> rows;
    for (const auto& index : view->selectionModel()->selectedRows()) {
        rows.push_back(index.row());
    }
    if (rows.empty()) {
        return;
    }
    auto opening = workspace_.convertItems(*model, engineOfView(view), std::move(rows));
    if (opening.unreachable > 0U) {
        statusBar()->showMessage(
            QStringLiteral("%1 of %2 tracks are not reachable on this computer and were left "
                           "out. Where that engine's music is reachable here is set in "
                           "Settings → Engine.")
                .arg(opening.unreachable)
                .arg(opening.unreachable + opening.items.size()),
            10'000);
    }
    if (!opening.items.empty()) {
        openConvertItems(std::move(opening.items));
    }
}

void BenchMainWindow::openConvertItems(std::vector<ConvertDialogItem> items) {
    auto* dialog = new ConvertDialog(std::move(items), workspace_.convertProfiles(),
                                     workspace_.convertPresets(), this);
    connect(dialog, &ConvertDialog::filesConverted, this,
            [this] { workspace_.refreshLocalLibrary(); });
    dialog->show();
}

// Observable for tests: what engines could not settle. Shown when some of
// it is new.
void BenchMainWindow::engineInterruptionsChanged(const bool reported_now) {
    const auto moves =
        std::ranges::count_if(engine_interruptions_, [](const auto& known) { return known.move; });
    setProperty("trackknife-file-reconciliation-count", static_cast<qulonglong>(moves));
    setProperty("trackknife-metadata-reconciliation-count",
                static_cast<qulonglong>(engine_interruptions_.size()) -
                    static_cast<qulonglong>(moves));
    if (reported_now) {
        presentInterruptedOperations();
    }
}

std::shared_ptr<engine::RemoteFileWork> BenchMainWindow::fileWorkOf(QTableView* view) const {
    return workspace_.fileWorkOf(engineOfView(view));
}

void BenchMainWindow::showReplayGainDialog() {
    auto* tab = currentListTab();
    showReplayGainForView(tab ? tab->view : nullptr);
}

void BenchMainWindow::showReplayGainForView(QTableView* view) {
    auto* model = view ? qobject_cast<LocalListModel*>(view->model()) : nullptr;
    if (!model || !view->selectionModel()) {
        return;
    }
    // ADR-0237: the engine holding the files measures and writes them, at its
    // own paths -- the same dialog, doing the same work, somewhere else.
    auto work = requireFileWork(view, tr("Measuring ReplayGain"));
    if (!work) {
        return;
    }
    auto selected = view->selectionModel()->selectedRows();
    std::ranges::sort(selected, {}, &QModelIndex::row);
    if (selected.empty()) {
        return;
    }
    std::vector<QPersistentModelIndex> selected_rows;
    selected_rows.reserve(static_cast<std::size_t>(selected.size()));
    for (const auto& index : selected) {
        selected_rows.emplace_back(index);
    }
    const auto count = selected_rows.size();
    auto* dialog =
        new ReplayGainDialog(count, selectionSourceReader(model, std::move(selected_rows)),
                             engineMetadataPlanApplierFactory(work), metadataApplyObserver(), this,
                             engineFileWorkTools(work));
    // Observable for tests and diagnostics: which did the work.
    dialog->setProperty("trackknife-file-work", QStringLiteral("engine"));
    dialog->show();
}

std::shared_ptr<engine::RemoteFileWork> BenchMainWindow::requireFileWork(QTableView* view,
                                                                         const QString& what) {
    auto work = fileWorkOf(view);
    if (!work) {
        statusBar()->showMessage(
            tr("%1 is done by the engine on %2, which is not available right now")
                .arg(what, engineName(engineOfView(view))),
            10'000);
    }
    return work;
}

void BenchMainWindow::openForScreenshot(const QString& name) {
    if (name == QStringLiteral("tagger")) {
        if (auto* view = activeTrackView()) {
            view->selectAll();
            showMetadataForView(view);
        }
        return;
    }
    if (name == QStringLiteral("replaygain-menu")) {
        if (auto* button = findChild<QToolButton*>(QStringLiteral("bench-local-replaygain"))) {
            QTimer::singleShot(0, button, &QToolButton::showMenu);
        }
        return;
    }
    if (name == QStringLiteral("quickpick")) {
        if (auto* quick_album = findChild<QAction*>(QStringLiteral("action-quick-album"))) {
            quick_album->trigger();
        }
        return;
    }
    if (name.startsWith(QStringLiteral("settings"))) {
        static const std::array pages{
            std::pair{"general", SettingsDialog::Page::general},
            std::pair{"playback", SettingsDialog::Page::playback},
            std::pair{"library", SettingsDialog::Page::library},
            std::pair{"engine", SettingsDialog::Page::engine},
            std::pair{"naming", SettingsDialog::Page::naming},
            std::pair{"replaygain", SettingsDialog::Page::replaygain},
            std::pair{"covers", SettingsDialog::Page::covers},
            std::pair{"metadata", SettingsDialog::Page::metadata_services},
            std::pair{"lastfm", SettingsDialog::Page::lastfm},
            std::pair{"shortcuts", SettingsDialog::Page::shortcuts},
        };
        auto page = SettingsDialog::Page::general;
        for (const auto& [key, value] : pages) {
            if (name == QStringLiteral("settings-") + QLatin1String{key}) {
                page = value;
            }
        }
        showSettingsDialog(page);
    }
}

void BenchMainWindow::showMetadataProperties() {
    auto* tab = currentListTab();
    showMetadataForView(tab ? tab->view : nullptr);
}

void BenchMainWindow::showMetadataForView(QTableView* view) {
    auto* model = view ? qobject_cast<LocalListModel*>(view->model()) : nullptr;
    if (!model || !view->selectionModel()) {
        return;
    }
    // ADR-0237: the engine holding the files reads, probes and writes them at
    // its own paths.
    auto work = requireFileWork(view, tr("Editing tags"));
    if (!work) {
        return;
    }
    auto selected = view->selectionModel()->selectedRows();
    std::ranges::sort(selected, {}, &QModelIndex::row);
    if (selected.empty()) {
        return;
    }
    std::vector<QPersistentModelIndex> selected_rows;
    selected_rows.reserve(static_cast<std::size_t>(selected.size()));
    for (const auto& index : selected) {
        selected_rows.emplace_back(index);
    }
    const auto selected_row_count = selected_rows.size();
    openMetadataProperties(selected_row_count,
                           selectionSourceReader(model, std::move(selected_rows)), std::move(work));
}

void BenchMainWindow::openMetadataProperties(const std::size_t selected_row_count,
                                             MetadataPropertiesSourceReader reader,
                                             std::shared_ptr<engine::RemoteFileWork> work) {
    auto opening = workspace_.taggerServices(std::move(work));
    const auto work_engine = opening.engine;
    auto* properties =
        new MetadataPropertiesDialog(selected_row_count, std::move(reader),
                                     Workspace::taggerFields(), std::move(opening.services), tabs_);
    // Observable for tests and diagnostics: which did the work.
    properties->setProperty("trackknife-file-work", QStringLiteral("engine"));
    properties->setArtworkMutationServices(std::move(opening.artwork_applier),
                                           std::move(opening.artwork_observer));
    connect(properties, &MetadataPropertiesDialog::statusMessage, this,
            [this](const QString& message) { statusBar()->showMessage(message, 12'000); });
    connect(properties, &MetadataPropertiesDialog::openSettingsRequested, this,
            [this](const SettingsDialog::Page page) {
                auto* settings = showSettingsDialog(page);
                if (settings != nullptr && page == SettingsDialog::Page::naming) {
                    settings->showNamingLayouts();
                }
            });
    // Managing destinations opens those of the engine its tracks are on.
    connect(properties, &MetadataPropertiesDialog::openDestinationsRequested, this,
            [this, work_engine] {
                if (auto* settings = showSettingsDialog(SettingsDialog::Page::naming)) {
                    settings->showDestinationsOf(work_engine.text());
                }
            });
    // ADR-0221: the tagger is a window, not a tab. Every tab is a list of
    // playable tracks; this is an editing surface holding staged, uncommitted
    // state with its own commit/cancel lifecycle, and tabs get closed
    // casually. As a window it keeps its own file list in its splitter, several
    // can stand open over different selections, and it survives being pointed
    // at a remote engine in Phase 2, where applying tags becomes a job rather
    // than a local write.
    properties->setWindowFlags(Qt::Window);
    // The dialog titles itself "Edit tags"; the selection size is appended so
    // several open taggers stay tellable apart in a window list.
    properties->setWindowTitle(
        QStringLiteral("%1 · %2 %3")
            .arg(properties->windowTitle())
            .arg(selected_row_count)
            .arg(selected_row_count == 1U ? QStringLiteral("track") : QStringLiteral("tracks")));
    properties->setAttribute(Qt::WA_DeleteOnClose);
    connect(properties, &QObject::destroyed, this, [this] {
        QTimer::singleShot(0, this, [this] {
            if (list_tabs_.empty()) {
                addListTab(
                    persistence::ListDocument{
                        .id = core::StableId::random(),
                        .kind = persistence::ListKind::scratch,
                        .name = untitled_list_name,
                        .pinned = false,
                        .dirty = false,
                        .items = {},
                    },
                    true);
            }
            refreshTabActions();
            refreshTrackViewActions();
            refreshSelectionStatus();
        });
    });
    properties->show();
    properties->raise();
    properties->activateWindow();
}

// Crash recovery is silent when it succeeds. Only operations recovery could
// neither finish nor safely roll back are surfaced, each exactly once: shown
// journal ids are remembered so a known incident does not reopen the window on
// every start.
void BenchMainWindow::presentInterruptedOperations() {
    if (engine_interruptions_.empty()) {
        return;
    }
    constexpr auto acknowledged_key = "workspace/acknowledged-interrupted-operations-v2";
    constexpr auto legacy_acknowledged_key = "workspace/acknowledged-interrupted-operations-v1";
    QSettings settings;
    const auto legacy_acknowledgement_exists =
        settings.contains(QLatin1String{legacy_acknowledged_key});
    auto acknowledged_list = settings.value(QLatin1String{acknowledged_key})
                                 .toString()
                                 .split(QChar{','}, Qt::SkipEmptyParts);
    QSet<QString> acknowledged{acknowledged_list.begin(), acknowledged_list.end()};
    std::vector<PreparationFeedbackRow> rows;
    const auto collect = [&acknowledged, &rows, legacy_acknowledgement_exists](
                             const core::StableId& id, const std::string& raw_path,
                             QString detail) {
        const auto key = QString::fromStdString(id.to_string());
        if (acknowledged.contains(key) || legacy_acknowledgement_exists) {
            acknowledged.insert(key);
            return;
        }
        acknowledged.insert(key);
        rows.push_back(PreparationFeedbackRow{
            .file = QString::fromStdString(core::display_raw_path(raw_path)),
            .detail = std::move(detail),
        });
    };
    for (const auto& interruption : engine_interruptions_) {
        collect(interruption.id, interruption.raw_path, interruption.detail);
    }
    // Keep acknowledgements even when a transient database/open error omits an
    // incident from one startup scan. Replacing the list with only the current
    // scan made old terminal journal entries reappear later. Sync before the
    // dialog is shown so even a forced shutdown after Close cannot lose it.
    acknowledged_list = acknowledged.values();
    acknowledged_list.sort(Qt::CaseInsensitive);
    settings.setValue(QLatin1String{acknowledged_key}, acknowledged_list.join(QChar{','}));
    if (legacy_acknowledgement_exists) {
        settings.remove(QLatin1String{legacy_acknowledged_key});
    }
    settings.sync();
    if (rows.empty()) {
        return;
    }
    if (interrupted_operations_dialog_ != nullptr) {
        interrupted_operations_dialog_->close();
    }
    auto* dialog = createPreparationFeedbackDialog(
        QStringLiteral("Interrupted file work"),
        QStringLiteral("Trackknife could not finish or safely undo %1 earlier %2. The listed "
                       "files were left as they are — check them before editing further.")
            .arg(rows.size())
            .arg(rows.size() == 1U ? QStringLiteral("operation") : QStringLiteral("operations")),
        rows, this);
    interrupted_operations_dialog_ = dialog;
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

} // namespace trackknife::bench
