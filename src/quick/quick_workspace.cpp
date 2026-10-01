// SPDX-License-Identifier: GPL-3.0-only

#include "quick/quick_workspace.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "bench/desktop_notifier.hpp"
#include "bench/mpris_service.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "uicommon/rating_color.hpp"
#include "uicommon/track_row_roles.hpp"
#include "workspace/color_scheme.hpp"
#include "workspace/command_search.hpp"
#include "workspace/language_reference.hpp"
#include "workspace/sources.hpp"

#include <QJsonObject>

#include <QCoreApplication>
#include <QDir>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QFile>
#include <QGuiApplication>
#include <QKeySequence>
#include <QSettings>

#include <algorithm>
#include <limits>
#include <ranges>

namespace trackknife::quick {
namespace {

constexpr int transport_refresh_ms = 33;
constexpr std::size_t visited_limit = 32;

QuickWorkspace* instance_ = nullptr;

} // namespace

QString QuickWorkspace::iconBase() {
    return QStringLiteral("image://icon/r%1/").arg(bench::ColorSchemes::instance().iconRevision());
}

QuickWorkspace::QuickWorkspace(QObject* parent) : QObject(parent) {
    workspace_.setView(this);
    connect(&bench::ColorSchemes::instance(), &bench::ColorSchemes::applied, this,
            &QuickWorkspace::iconBaseChanged);
    connect(&rows_, &TrackRowsModel::selectionChanged, this, &QuickWorkspace::selectionEdited);
    connect(&edit_job_, &bench::ListEditJob::edited, this, [this](bench::LocalListModel* model) {
        for (const auto& tab : workspace_.list_tabs_) {
            if (tab->model == model) {
                workspace_.markTabDirty(*tab);
                workspace_.syncArtwork(*tab);
                break;
            }
        }
        refreshSelectionStatus();
    });
    transport_timer_.setInterval(transport_refresh_ms);
    connect(&transport_timer_, &QTimer::timeout, this, &QuickWorkspace::refreshTransport);
    connect(&transfer_, &bench::PlaylistTransfer::changed, this, &QuickWorkspace::transferChanged);
    connect(&find_, &bench::ListFind::changed, this, &QuickWorkspace::findChanged);
    connect(&panel_arrangement_, &bench::PanelArrangement::changed, this,
            &QuickWorkspace::panelsChanged);
    connect(&panel_arrangement_, &bench::PanelArrangement::editingChanged, this,
            &QuickWorkspace::panelsChanged);
    if (const auto error = panel_arrangement_.load(); !error.isEmpty()) {
        QTimer::singleShot(0, this, [this, error] {
            showMessage(QStringLiteral("Panel layout was not loaded (%1); the saved value was "
                                       "preserved")
                            .arg(error),
                        7'000);
        });
    }
    connect(&lists_catalog_, &bench::ListsCatalog::changed, this,
            &QuickWorkspace::listGroupsChanged);
    connect(&lists_catalog_, &bench::ListsCatalog::closeWanted, this, [this](const QString& id) {
        if (auto* tab = workspace_.tabForDocument(id); tab != nullptr) {
            closeTab(tabs_.indexOf(tab));
        }
    });
    connect(&lists_catalog_, &bench::ListsCatalog::saveWanted, this,
            &QuickWorkspace::saveListWanted);
    // What is open here, as it changes.
    for (const auto signal : {&QuickWorkspace::currentTabChanged, &QuickWorkspace::listChanged}) {
        connect(this, signal, this, [this] {
            if (listsInPanel()) {
                lists_catalog_.present();
            }
        });
    }
    connect(&find_, &bench::ListFind::opened, this, &QuickWorkspace::findOpened);
    connect(&find_, &bench::ListFind::dismissed, this, &QuickWorkspace::findDismissed);
    connect(&find_, &bench::ListFind::found, this, &QuickWorkspace::revealRow);
    connect(&rows_, &TrackRowsModel::selectionChanged, &find_, &bench::ListFind::selectionChanged);
    connect(this, &QuickWorkspace::currentTabChanged, this, [this] {
        auto* tab = currentTabPointer();
        find_.setList(tab != nullptr ? tab->model : nullptr, [this] { return rows_.currentRow(); });
    });
    connect(&transfer_, &bench::PlaylistTransfer::imported, this,
            [this](std::shared_ptr<std::vector<bench::LocalTrackRow>> rows, const QString& name) {
                workspace_.addImportedList(std::move(*rows), name);
            });
    connect(&transfer_, &bench::PlaylistTransfer::completed, this,
            [this](bool, const QString& message) { showMessage(message, 8'000); });
}

QuickWorkspace::~QuickWorkspace() {
    transport_timer_.stop();
    workspace_.probe_cancellation_.request_cancellation();
    workspace_.probe_queue_.clear();
    workspace_.artwork_queue_.clear();
    workspace_.probe_watcher_.waitForFinished();
    workspace_.artwork_watcher_.waitForFinished();
    workspace_.discovery_watcher_.waitForFinished();
}

QuickWorkspace* QuickWorkspace::create(QQmlEngine*, QJSEngine*) {
    // Owned by main(), not by the QML engine.
    QJSEngine::setObjectOwnership(instance_, QJSEngine::CppOwnership);
    return instance_;
}

void QuickWorkspace::setInstance(QuickWorkspace* instance) { instance_ = instance; }

void QuickWorkspace::addLibrary(EngineLink& engine) {
    if (engine.catalogue == nullptr) {
        return;
    }
    auto* browser = new bench::LibraryBrowser(*engine.catalogue, engine.key, this);
    workspace_.attachLibrary(browser);
    libraries_.push_back(Library{.engine = engine.key, .browser = browser});
    workspace_.refreshRatings();
    selectPreferredSource();
}

// The sources in the order the tabs show them: Folders, this computer's
// library unless hidden, then each engine elsewhere's.
QVariantList QuickWorkspace::sources() const {
    QVariantList list{QVariantMap{{QStringLiteral("kind"), QStringLiteral("folders")},
                                  {QStringLiteral("title"), QStringLiteral("Folders")}}};
    for (const auto& library : libraries_) {
        if (library.engine.isLocal()) {
            if (bench::localLibraryShown()) {
                list.push_back(QVariantMap{{QStringLiteral("kind"), QStringLiteral("library")},
                                           {QStringLiteral("title"), QStringLiteral("Library")}});
            }
            continue;
        }
        const auto* catalogue = workspace_.catalogueOf(library.engine);
        list.push_back(QVariantMap{
            {QStringLiteral("kind"), QStringLiteral("remote")},
            {QStringLiteral("key"), library.engine.text()},
            {QStringLiteral("title"), catalogue != nullptr ? catalogue->name() : QString{}},
            {QStringLiteral("tooltip"), catalogue != nullptr ? catalogue->describe() : QString{}}});
    }
    return list;
}

int QuickWorkspace::sourceIndexOf(const bench::EngineKey& engine) const {
    const auto list = sources();
    for (int index = 0; index < list.size(); ++index) {
        const auto source = list.at(index).toMap();
        const auto kind = source.value(QStringLiteral("kind")).toString();
        if ((engine.isLocal() && kind == QStringLiteral("library")) ||
            (!engine.isLocal() &&
             source.value(QStringLiteral("key")).toString() == engine.text())) {
            return index;
        }
    }
    return -1;
}

bench::LibraryBrowser* QuickWorkspace::library() const {
    const auto list = sources();
    if (current_source_ <= 0 || current_source_ >= list.size()) {
        return nullptr;
    }
    const auto source = list.at(current_source_).toMap();
    const auto engine =
        source.value(QStringLiteral("kind")).toString() == QStringLiteral("library")
            ? bench::EngineKey::local()
            : bench::EngineKey::fromText(source.value(QStringLiteral("key")).toString());
    const auto found = std::ranges::find(libraries_, engine, &Library::engine);
    return found != libraries_.end() ? found->browser : nullptr;
}

QImage QuickWorkspace::libraryCover(const QString& engine, const QString& album_key) const {
    const auto found =
        std::ranges::find(libraries_, bench::EngineKey::fromText(engine), &Library::engine);
    return found != libraries_.end() ? found->browser->cover(album_key) : QImage{};
}

void QuickWorkspace::selectSource(const int index, const bool chosen) {
    const auto list = sources();
    if (index < 0 || index >= list.size()) {
        return;
    }
    if (chosen) {
        const auto kind = list.at(index).toMap().value(QStringLiteral("kind")).toString();
        bench::rememberSource(kind);
    }
    if (index != current_source_) {
        current_source_ = index;
        emit sourcesChanged();
    }
}

// A library unless Folders was chosen: this computer's, or an engine
// elsewhere's when this computer's is hidden or that one was chosen.
void QuickWorkspace::selectPreferredSource() {
    const auto list = sources();
    const auto wanted = bench::preferredSource();
    int remote = -1;
    int local = -1;
    for (int index = 0; index < list.size(); ++index) {
        const auto kind = list.at(index).toMap().value(QStringLiteral("kind")).toString();
        if (kind == QStringLiteral("remote")) {
            remote = index;
        } else if (kind == QStringLiteral("library")) {
            local = index;
        }
    }
    int target = 0;
    if (wanted == QStringLiteral("folders")) {
        target = 0;
    } else if (wanted == QStringLiteral("remote") || local < 0) {
        target = remote >= 0 ? remote : local >= 0 ? local : 0;
    } else {
        target = local;
    }
    current_source_ = std::max(0, target);
    emit sourcesChanged();
}

void QuickWorkspace::openFolderEntry(const QModelIndex& index) {
    if (index.isValid()) {
        workspace_.openLocalPaths({folders_.rawPath(index)});
    }
}

void QuickWorkspace::locate(const bool album) { locateOf(shownPicked(), album); }

void QuickWorkspace::locateOf(const Picked& picked, const bool album) {
    const auto row = picked.current;
    if (picked.model == nullptr || row < 0 || row >= picked.model->rowCount()) {
        return;
    }
    const auto index = sourceIndexOf(picked.engine);
    const auto found = std::ranges::find(libraries_, picked.engine, &Library::engine);
    if (index < 0 || found == libraries_.end()) {
        return;
    }
    selectSource(index, false);
    found->browser->locatePath(picked.model->rows()[static_cast<std::size_t>(row)].raw_path, album);
}

QVariantList QuickWorkspace::libraryListTargets() const {
    QVariantList targets;
    if (auto* browser = library(); browser != nullptr) {
        for (const auto& [id, name] : workspace_.listTargets(browser->engine())) {
            targets.push_back(
                QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("name"), name}});
        }
    }
    return targets;
}

void QuickWorkspace::focusLibrarySearch() {
    // On Folders, which has no search: the library it would switch to.
    if (library() == nullptr) {
        selectPreferredSource();
        if (library() == nullptr && sources().size() > 1) {
            selectSource(1, false);
        }
    }
    emit librarySearchFocused();
}

void QuickWorkspace::start() {
    // ADR-0226: this window plays nothing itself. The engine owns playback,
    // and the buffer shown here is the one it reports.
    workspace_.selected_buffer_profile_ = bench::Workspace::loadPlaybackBufferPreference().profile;
    follow_playback_ =
        QSettings{}.value(QStringLiteral("workspace/follow-playback"), false).toBool();
    workspace_.startLastFm();
    workspace_.start();
    transport_timer_.start();
    // The tree browses the whole filesystem; bookmarks are the fast lane.
    folders_.model()->addRoot("/");
    const auto home = QFile::encodeName(QDir::homePath());
    folders_.reveal(std::string{home.constData(), static_cast<std::size_t>(home.size())});
    buildDesktopServices();
    refreshLocalPlaybackControls();
    refreshTransport();
}

// Desktop commands land on the same commands the visible controls use
// (ADR-0135).
void QuickWorkspace::buildDesktopServices() {
    mpris_ = new bench::MprisService(this);
    connect(mpris_, &bench::MprisService::playPauseRequested, this, &QuickWorkspace::playPause);
    connect(mpris_, &bench::MprisService::playRequested, this, [this] {
        if (mpris_->currentState().status != QStringLiteral("Playing")) {
            playPause();
        }
    });
    connect(mpris_, &bench::MprisService::pauseRequested, this, [this] {
        if (mpris_->currentState().status == QStringLiteral("Playing")) {
            playPause();
        }
    });
    connect(mpris_, &bench::MprisService::stopRequested, this, &QuickWorkspace::stop);
    connect(mpris_, &bench::MprisService::nextRequested, this, &QuickWorkspace::next);
    connect(mpris_, &bench::MprisService::previousRequested, this, &QuickWorkspace::previous);
    connect(mpris_, &bench::MprisService::positionRequested, this,
            [this](const qlonglong position_ms) {
                if (transport_.value(QStringLiteral("seekEnabled")).toBool()) {
                    seek(position_ms);
                }
            });
    connect(mpris_, &bench::MprisService::volumeRequested, this,
            [this](const int volume_percent) { setVolume(volume_percent); });
    // ADR-0144: quiet, opt-in track-change notifications share the MPRIS
    // now-playing snapshot.
    notifier_ = new bench::DesktopNotifier(this);
    notifier_->setBackgroundOnly(
        QSettings{}.value(QStringLiteral("desktop/notifications-background-only"), false).toBool());
    connect(notifier_, &bench::DesktopNotifier::deliveryFinished, this,
            [this](const QString& error) {
                if (!error.isEmpty()) {
                    showMessage(QStringLiteral("Notification failed: %1").arg(error), 8000);
                }
            });
    notifier_->setEnabled(
        QSettings{}.value(QStringLiteral("desktop/notifications"), false).toBool());
    workspace_.mpris_ = mpris_;
    workspace_.notifier_ = notifier_;
}

// --- Lists ------------------------------------------------------------------

QuickWorkspace::ListTab* QuickWorkspace::currentTabPointer() const { return tabs_.at(current_); }

void QuickWorkspace::setCurrentTab(const int index) {
    auto* tab = tabs_.at(index);
    if (tab == nullptr || index == current_) {
        return;
    }
    current_ = index;
    std::erase(visited_, tab);
    visited_.push_back(tab);
    if (visited_.size() > visited_limit) {
        visited_.erase(visited_.begin());
    }
    rows_.setSource(tab->model, tab->view_layout);
    edit_job_.setModel(tab->model);
    emit currentTabChanged();
    refreshList();
    refreshSelectionStatus();
    refreshHistory();
    refreshPlaybackCursor(false);
}

QAbstractItemModel* QuickWorkspace::listModel() const {
    auto* tab = currentTabPointer();
    return tab != nullptr ? tab->model : nullptr;
}

void QuickWorkspace::refreshList() {
    auto* tab = currentTabPointer();
    QVariantMap list;
    if (tab != nullptr) {
        const auto engine = bench::EngineKey::of(tab->document);
        list.insert(QStringLiteral("emptyTitle"), workspace_.emptyListTitle(engine));
        list.insert(QStringLiteral("emptyHint"), workspace_.emptyListHint(engine));
        list.insert(QStringLiteral("presentation"),
                    ui::trackViewPresentationId(tab->view_layout.presentation));
        list.insert(QStringLiteral("remote"), !engine.isLocal());
        list.insert(QStringLiteral("pinned"), tab->document.pinned);
        list.insert(QStringLiteral("name"), bench::displayText(tab->document.name));
        list.insert(QStringLiteral("scratch"),
                    tab->document.kind == persistence::ListKind::scratch);
        QStringList shown;
        for (const auto& column : tab->view_layout.columns) {
            if (column.visible) {
                shown.push_back(column.id);
            }
        }
        list.insert(QStringLiteral("visibleColumns"), shown);
        list.insert(QStringLiteral("playingRow"), workspace_.resolvePlaybackRow(tab));
        list.insert(QStringLiteral("accessibleName"), workspace_.tabChrome(*tab).accessible_name);
    }
    if (list != list_) {
        list_ = std::move(list);
        emit listChanged();
    }
}

QVariantMap QuickWorkspace::tabAt(const int index) const {
    auto* tab = tabs_.at(index);
    if (tab == nullptr) {
        return {};
    }
    return {{QStringLiteral("name"), bench::displayText(tab->document.name)},
            {QStringLiteral("dirty"), tab->document.dirty},
            {QStringLiteral("pinned"), tab->document.pinned},
            {QStringLiteral("scratch"), tab->document.kind == persistence::ListKind::scratch}};
}

void QuickWorkspace::closeTab(const int index) {
    auto* tab = tabs_.at(index);
    if (tab == nullptr) {
        return;
    }
    if (tab->document.pinned) {
        showMessage(QStringLiteral("Unpin this list before closing it"), 3'000);
        return;
    }
    std::erase(visited_, tab);
    const bool was_current = index == current_;
    tabs_.remove(index);
    if (current_ > index || (was_current && current_ >= tabs_.rowCount())) {
        --current_;
    }
    workspace_.closeList(*tab);
    // The tab visited before it, as the widgets window goes back to.
    if (was_current && !visited_.empty()) {
        const auto back = tabs_.indexOf(visited_.back());
        if (back >= 0) {
            current_ = -1;
            setCurrentTab(back);
            return;
        }
    }
    if (auto* shown = currentTabPointer(); shown != nullptr) {
        rows_.setSource(shown->model, shown->view_layout);
        edit_job_.setModel(shown->model);
    } else {
        rows_.setSource(nullptr, {});
        edit_job_.setModel(nullptr);
    }
    emit currentTabChanged();
    refreshList();
    refreshSelectionStatus();
}

void QuickWorkspace::moveTab(const int from, const int to) {
    auto* current = currentTabPointer();
    tabs_.move(from, to);
    current_ = tabs_.indexOf(current);
    emit currentTabChanged();
    workspace_.schedulePersist();
}

void QuickWorkspace::selectionEdited() { refreshSelectionStatus(); }

void QuickWorkspace::activateRow(const int row) {
    if (auto* tab = currentTabPointer();
        tab != nullptr && row >= 0 && row < tab->model->rowCount()) {
        workspace_.playRow(*tab, row);
    }
}

void QuickWorkspace::convertFiles() { convertFilesOf(shownPicked()); }

void QuickWorkspace::convertFilesOf(Picked picked) {
    if (picked.model == nullptr || picked.rows.empty()) {
        return;
    }
    const auto selected = picked.rows.size();
    auto opening = workspace_.convertItems(*picked.model, picked.engine, std::move(picked.rows));
    if (opening.unreachable > 0U) {
        showMessage(QStringLiteral("%1 of %2 tracks are not reachable on this computer and were "
                                   "left out. Where that engine's music is reachable here is set "
                                   "in Settings → Engine.")
                        .arg(opening.unreachable)
                        .arg(selected),
                    10'000);
    }
    if (opening.items.empty()) {
        return;
    }
    auto* convert = new QuickConvert(new bench::ConvertJob(std::move(opening.items),
                                                           workspace_.convertProfiles(),
                                                           workspace_.convertPresets()),
                                     this);
    connect(convert, &QuickConvert::filesConverted, this,
            [this] { workspace_.refreshLocalLibrary(); });
    emit convertOpened(convert);
}

void QuickWorkspace::replayGain() { replayGainOf(shownPicked()); }

void QuickWorkspace::replayGainOf(Picked picked) {
    if (picked.model == nullptr || picked.rows.empty()) {
        return;
    }
    // ADR-0237: the engine holding the files measures and writes them, at
    // its own paths.
    auto work = workspace_.fileWorkOf(picked.engine);
    if (!work) {
        showMessage(tr("%1 is done by the engine on %2, which is not available right now")
                        .arg(tr("Measuring ReplayGain"), workspace_.engineName(picked.engine)),
                    10'000);
        return;
    }
    const auto count = picked.rows.size();
    auto* job = new QuickReplayGain(
        new bench::ReplayGainJob(count, pickedReader(picked), 
                                 workspace_.engineMetadataPlanApplierFactory(work),
                                 workspace_.metadataApplyObserver(),
                                 bench::engineFileWorkTools(work)),
        this);
    emit replayGainOpened(job);
}

void QuickWorkspace::editTags() { editTagsOf(shownPicked()); }

void QuickWorkspace::editTagsOf(Picked picked) {
    if (picked.model == nullptr || picked.rows.empty()) {
        return;
    }
    // ADR-0237: the engine holding the files reads, probes and writes them at
    // its own paths.
    auto work = workspace_.fileWorkOf(picked.engine);
    if (!work) {
        showMessage(tr("%1 is done by the engine on %2, which is not available right now")
                        .arg(tr("Editing tags"), workspace_.engineName(picked.engine)),
                    10'000);
        return;
    }
    const auto count = picked.rows.size();
    auto opening = workspace_.taggerServices(std::move(work));
    // The selection size in the title, so several open editors stay
    // tellable apart.
    auto* tagger =
        new QuickTagger(QStringLiteral("Edit tags · %1 %2")
                            .arg(count)
                            .arg(count == 1U ? QStringLiteral("track") : QStringLiteral("tracks")),
                        count, pickedReader(picked), std::move(opening.services),
                        std::move(opening.artwork_applier), std::move(opening.artwork_observer),
                        this);
    tagger->setEngineKey(opening.engine.text());
    connect(tagger, &QuickTagger::statusMessage, this,
            [this](const QString& message) { showMessage(message, 12'000); });
    emit taggerOpened(tagger);
}

QuickWorkspace::Picked QuickWorkspace::shownPicked() const {
    auto* tab = currentTabPointer();
    if (tab == nullptr) {
        return {};
    }
    auto rows = rows_.selectedRows();
    std::ranges::sort(rows);
    return {.tab = tab,
            .model = tab->model,
            .engine = bench::EngineKey::of(tab->document),
            .rows = std::move(rows),
            .current = rows_.currentRow()};
}

bench::MetadataPropertiesSourceReader QuickWorkspace::pickedReader(const Picked& picked) {
    std::vector<QPersistentModelIndex> selected;
    selected.reserve(picked.rows.size());
    for (const auto row : picked.rows) {
        selected.emplace_back(picked.model->index(row, 0));
    }
    return picked.tab != nullptr ? workspace_.selectionSourceReader(*picked.tab, std::move(selected))
                                 : workspace_.selectionSourceReader(picked.model, std::move(selected));
}

QuickSettings* QuickWorkspace::openSettings() {
    auto* settings = new QuickSettings(workspace_, workspace_.buildOutputProfileStore(), this);
    // ADR-0185: profile edits in Settings refresh every open tag editor's
    // selectors immediately.
    connect(settings, &QuickSettings::outputProfilesChanged, this, [this] {
        for (auto* tagger : findChildren<QuickTagger*>(Qt::FindDirectChildrenOnly)) {
            tagger->reloadOutputProfiles();
        }
    });
    connect(settings, &QuickSettings::saved, this, [this] {
        for (auto* tagger : findChildren<QuickTagger*>(Qt::FindDirectChildrenOnly)) {
            tagger->refreshStoragePolicy();
        }
        ++shortcut_revision_;
        emit shortcutsChanged();
        emit desktopChanged();
        // The local library shown or hidden, the lists as tabs or a pane.
        emit sourcesChanged();
        emit listsPanelChanged();
        if (listsInPanel()) {
            lists_catalog_.fetch();
        }
    });
    QQmlEngine::setObjectOwnership(settings, QQmlEngine::CppOwnership);
    return settings;
}

QString QuickWorkspace::shortcut(const QString& id, const QString& default_key) {
    return bench::ShortcutSession::saved(id, QKeySequence(default_key, QKeySequence::PortableText))
        .toString(QKeySequence::PortableText);
}

void QuickWorkspace::importPlaylist(const QUrl& file) {
    if (!workspace_.lists_restored_ || transfer_.busy()) {
        return;
    }
    const auto encoded = QFile::encodeName(file.toLocalFile());
    transfer_.importFile({encoded.constData(), static_cast<std::size_t>(encoded.size())});
}

void QuickWorkspace::exportPlaylist(const QUrl& file) {
    auto* tab = currentTabPointer();
    if (tab == nullptr || transfer_.busy()) {
        return;
    }
    auto path = file.toLocalFile();
    if (!path.endsWith(QStringLiteral(".m3u8"), Qt::CaseInsensitive)) {
        path += QStringLiteral(".m3u8");
    }
    const auto encoded = QFile::encodeName(path);
    transfer_.exportFile({encoded.constData(), static_cast<std::size_t>(encoded.size())},
                         tab->model);
}

QVariantMap QuickWorkspace::panels() const {
    const auto& root = panel_arrangement_.layout().root;
    QStringList order;
    QVariantList weights;
    for (const auto& child : root.children) {
        order.append(child.panel_id);
    }
    for (const auto weight : root.weights) {
        weights.append(weight);
    }
    return {{QStringLiteral("kind"), root.kind == ui::PanelLayoutNodeKind::tabs
                                         ? QStringLiteral("tabs")
                                         : QStringLiteral("split")},
            {QStringLiteral("vertical"), root.orientation == Qt::Vertical},
            {QStringLiteral("order"), order},
            {QStringLiteral("weights"), weights},
            {QStringLiteral("active"), root.active_child},
            {QStringLiteral("editing"), panel_arrangement_.editing()}};
}

void QuickWorkspace::arrangePanels(const QString& arrangement) {
    if (!panel_arrangement_.editing()) {
        return;
    }
    if (arrangement == QStringLiteral("tabs")) {
        panel_arrangement_.arrange(ui::PanelLayoutNodeKind::tabs, Qt::Horizontal);
    } else {
        panel_arrangement_.arrange(ui::PanelLayoutNodeKind::split,
                                   arrangement == QStringLiteral("stacked") ? Qt::Vertical
                                                                             : Qt::Horizontal);
    }
}

void QuickWorkspace::swapPanels() {
    if (panel_arrangement_.editing()) {
        panel_arrangement_.swap();
    }
}

void QuickWorkspace::resetPanels() { panel_arrangement_.reset(); }

void QuickWorkspace::setPanelEditing(const bool editing) {
    panel_arrangement_.setEditing(editing);
    showMessage(editing ? QStringLiteral("Panel layout editing: choose an arrangement or swap "
                                         "panels")
                        : QString{},
                editing ? 0 : 1);
}

void QuickWorkspace::setPanelSizes(const QVariantList& sizes) {
    auto layout = panel_arrangement_.layout();
    if (layout.root.kind != ui::PanelLayoutNodeKind::split ||
        sizes.size() != static_cast<qsizetype>(layout.root.children.size())) {
        return;
    }
    std::vector<int> weights;
    for (const auto& size : sizes) {
        weights.push_back(std::max(1, size.toInt()));
    }
    layout.root.weights = std::move(weights);
    panel_arrangement_.adopt(std::move(layout), true);
}

void QuickWorkspace::setPanelTab(const int index) {
    auto layout = panel_arrangement_.layout();
    if (layout.root.kind != ui::PanelLayoutNodeKind::tabs || index < 0 ||
        index >= static_cast<int>(layout.root.children.size())) {
        return;
    }
    layout.root.active_child = index;
    panel_arrangement_.adopt(std::move(layout), false);
    emit panelsChanged();
}

void QuickWorkspace::dragRows() {
    dragPicked(shownPicked(), false);
}

void QuickWorkspace::dragPicked(const Picked& picked, const bool dynamic) {
    dragged_ = {};
    if (picked.model == nullptr || picked.rows.empty()) {
        return;
    }
    dragged_.from_tab = picked.tab;
    dragged_.from_model = picked.model;
    dragged_.dynamic = dynamic;
    dragged_.rows = picked.rows;
    dragged_.engine = picked.engine;
    dragged_model_ = picked.model;
}

void QuickWorkspace::dragLibrary(const QModelIndexList& indexes) {
    dragged_ = {};
    auto* browser = library();
    if (browser == nullptr || indexes.isEmpty()) {
        return;
    }
    auto entries = browser->selectedEntries(indexes);
    if (entries.empty()) {
        return;
    }
    dragged_.library = browser;
    dragged_.entries = entries;
    dragged_.engine = browser->engine();
    dragged_.resolve = [browser = QPointer{browser}, entries](
                           std::function<void(std::vector<std::string>)> done) {
        if (browser) {
            browser->resolveEntries(entries, std::move(done));
        }
    };
}

void QuickWorkspace::dragFolder(const QModelIndex& index) {
    dragged_ = {};
    auto path = folders_.rawPath(index);
    if (!path.empty()) {
        dragged_.paths.push_back(std::move(path));
    }
}

QString QuickWorkspace::draggedKind() const {
    if (dragging_up_next_) {
        return QStringLiteral("upnext");
    }
    if (dragged_.isRows() && dragged_model_) {
        return QStringLiteral("rows");
    }
    return dragged_.isFiles() ? QStringLiteral("files") : QString{};
}

bool QuickWorkspace::dropOnTab(const int tab, const int row, const bool copy) {
    if (draggedKind().isEmpty() || dragging_up_next_) {
        return false;
    }
    auto dragged = std::exchange(dragged_, {});
    if (tab < 0) {
        return workspace_.dropOnNewList(std::move(dragged), copy);
    }
    auto* target = tabs_.at(tab);
    if (target == nullptr) {
        return false;
    }
    // Rows dropped on their own tab's name: nothing to move.
    if (row < 0 && target->model == dragged.from_model) {
        return false;
    }
    const bool taken = workspace_.dropOnList(std::move(dragged), *target, row, copy);
    if (taken && tab != current_) {
        setCurrentTab(tab);
    }
    return taken;
}

bool QuickWorkspace::dropOnUpNext(const int position) {
    if (draggedKind().isEmpty() || dragging_up_next_) {
        return false;
    }
    return workspace_.dropOnUpNext(std::exchange(dragged_, {}), position);
}

bool QuickWorkspace::dropOnPanelList(const QString& engine, const QString& id) {
    if (draggedKind().isEmpty() || dragging_up_next_) {
        return false;
    }
    return workspace_.dropOnEngineList(std::exchange(dragged_, {}),
                                       bench::EngineKey::fromText(engine), id);
}

namespace {

// Files from a file manager: this computer's.
[[nodiscard]] bench::Workspace::Dragged droppedFiles(const QList<QUrl>& urls) {
    bench::Workspace::Dragged dragged;
    for (const auto& url : urls) {
        if (url.isLocalFile()) {
            const auto encoded = QFile::encodeName(url.toLocalFile());
            dragged.paths.emplace_back(encoded.constData(),
                                       static_cast<std::size_t>(encoded.size()));
        }
    }
    return dragged;
}

} // namespace

bool QuickWorkspace::dropUrlsOnPanelList(const QList<QUrl>& urls, const QString& engine,
                                         const QString& id) {
    auto dragged = droppedFiles(urls);
    return !dragged.paths.empty() &&
           workspace_.dropOnEngineList(std::move(dragged), bench::EngineKey::fromText(engine), id);
}

bool QuickWorkspace::dropUrls(const QList<QUrl>& urls, const int tab, const int row) {
    auto dragged = droppedFiles(urls);
    if (dragged.paths.empty()) {
        return false;
    }
    // Files from a file manager are this computer's.
    if (tab < 0) {
        return workspace_.dropOnNewList(std::move(dragged), true);
    }
    auto* target = tabs_.at(tab);
    return target != nullptr && workspace_.dropOnList(std::move(dragged), *target, row, true);
}

void QuickWorkspace::engineListsChanged() {
    if (listsInPanel()) {
        lists_catalog_.fetch();
    }
}

bool QuickWorkspace::listsInPanel() const {
    return QSettings{}.value(QStringLiteral("appearance/lists-display")).toString() ==
           QStringLiteral("panel");
}

void QuickWorkspace::setListsInPanel(const bool on) {
    if (on == listsInPanel()) {
        return;
    }
    QSettings{}.setValue(QStringLiteral("appearance/lists-display"),
                         on ? QStringLiteral("panel") : QStringLiteral("tabs"));
    if (on) {
        lists_catalog_.fetch();
    }
    emit listsPanelChanged();
}

QVariantList QuickWorkspace::listGroups() const {
    const auto* shown = currentTabPointer();
    const auto current = shown != nullptr ? bench::document_text(shown->document.id) : QString{};
    QVariantList groups;
    for (const auto& group : lists_catalog_.groups()) {
        QVariantList lists;
        for (const auto& list : group.lists) {
            lists.append(QVariantMap{{QStringLiteral("id"), list.id},
                                     {QStringLiteral("name"), list.name},
                                     {QStringLiteral("saved"), list.saved},
                                     {QStringLiteral("open"), list.open},
                                     {QStringLiteral("dirty"), list.dirty},
                                     {QStringLiteral("pinned"), list.pinned},
                                     {QStringLiteral("playing"), list.playing},
                                     {QStringLiteral("tracks"), list.tracks},
                                     {QStringLiteral("current"), list.id == current}});
        }
        groups.append(QVariantMap{{QStringLiteral("name"), group.name},
                                  {QStringLiteral("engine"), group.engine.text()},
                                  {QStringLiteral("note"), group.note},
                                  {QStringLiteral("lists"), lists}});
    }
    return groups;
}

void QuickWorkspace::openPanelList(const QString& engine, const QString& id) {
    lists_catalog_.open(bench::EngineKey::fromText(engine), id);
}

void QuickWorkspace::closePanelList(const QString& engine, const QString& id) {
    lists_catalog_.close(bench::EngineKey::fromText(engine), id);
}

void QuickWorkspace::savePanelList(const QString& engine, const QString& id) {
    lists_catalog_.save(bench::EngineKey::fromText(engine), id);
}

void QuickWorkspace::renamePanelList(const QString& engine, const QString& id,
                                     const QString& name) {
    lists_catalog_.rename(bench::EngineKey::fromText(engine), id, name);
}

void QuickWorkspace::deletePanelList(const QString& engine, const QString& id) {
    lists_catalog_.remove(bench::EngineKey::fromText(engine), id);
}

int QuickWorkspace::listsPanelWidth() {
    return QSettings{}.value(QStringLiteral("lists-panel/width"), 220).toInt();
}

void QuickWorkspace::setListsPanelWidth(const int width) {
    QSettings{}.setValue(QStringLiteral("lists-panel/width"), width);
}

QVariantMap QuickWorkspace::find() const {
    return {{QStringLiteral("query"), find_.query()},
            {QStringLiteral("status"), find_.status()},
            {QStringLiteral("shown"), find_.shown()},
            {QStringLiteral("available"), find_.available()}};
}

QVariantMap QuickWorkspace::transfer() const {
    return {{QStringLiteral("shown"), transfer_.shown()},
            {QStringLiteral("active"), transfer_.active()},
            {QStringLiteral("status"), transfer_.status()}};
}

void QuickWorkspace::backupWorkspace(const QUrl& file) {
    workspace_.backupWorkspace(file.toLocalFile());
}

void QuickWorkspace::scheduleWorkspaceRestore(const QUrl& file) {
    bench::Workspace::scheduleWorkspaceRestore(file.toLocalFile());
}

QuickOpenList* QuickWorkspace::openList() {
    auto* list = new QuickOpenList(new bench::OpenListSession(workspace_), this);
    QQmlEngine::setObjectOwnership(list, QQmlEngine::CppOwnership);
    return list;
}

QuickSearch* QuickWorkspace::openSearch() {
    auto* local = workspace_.localCatalogue();
    if (local == nullptr) {
        return nullptr;
    }
    std::vector<bench::SearchSession::OtherLibrary> others;
    for (const auto& engine : workspace_.engines_) {
        if (!engine->key.isLocal() && engine->catalogue != nullptr) {
            others.push_back({.engine = engine->key,
                              .catalogues = engine->catalogue.get(),
                              .name = engine->catalogue->name()});
        }
    }
    // ADR-0153: database scope reads the workspace index; tab scope
    // snapshots the current local tab's rows and reports on-demand
    // technicals back onto every tab holding the probed file.
    auto* session = new bench::SearchSession(
        *local,
        [this]() -> std::optional<bench::SearchSession::TabSnapshot> {
            auto* tab = currentTabPointer();
            if (tab == nullptr) {
                return std::nullopt;
            }
            return bench::SearchSession::TabSnapshot{QString::fromUtf8(tab->document.name),
                                                     tab->model->rows()};
        },
        [this](std::string raw_path, bench::LocalTrackTechnicals technicals) {
            for (const auto& tab : workspace_.list_tabs_) {
                tab->model->applyTechnicals(raw_path, technicals);
            }
        },
        std::move(others));
    connect(session, &bench::SearchSession::rowsRequested, this,
            [this](const QString& name, std::vector<bench::LocalTrackRow> rows,
                   const bench::LocalLibraryAction action, const bench::EngineKey& engine) {
                workspace_.placeFoundRows(name, std::move(rows), action, engine);
            });
    auto* search = new QuickSearch(session, this);
    connect(this, &QuickWorkspace::currentTabChanged, search, [this, search] {
        auto* tab = currentTabPointer();
        search->session()->watchCurrentModel(tab != nullptr ? tab->model : nullptr);
    });
    followSearch(search);
    QQmlEngine::setObjectOwnership(search, QQmlEngine::CppOwnership);
    return search;
}

void QuickWorkspace::followSearch(QuickSearch* search) {
    if (search == nullptr) {
        return;
    }
    // Opened from a tab of another engine, it searches that engine's library.
    auto* tab = currentTabPointer();
    search->session()->watchCurrentModel(tab != nullptr ? tab->model : nullptr);
    search->session()->followLibrary(tab != nullptr ? bench::EngineKey::of(tab->document)
                                                    : bench::EngineKey::local());
}

QuickDynamic* QuickWorkspace::openDynamic() {
    // One set of definitions: a rule reads the same on either library.
    const auto profile = QStringLiteral("local");
    // ADR-0220: through the catalogue source, like every other library read.
    bench::DynamicPlaylistSession::LibrarySearch search =
        [this](const bench::EngineKey& engine, query::CompiledTkq compiled,
               core::CancellationToken cancellation,
               bench::DynamicPlaylistService::Completion completion) {
            auto* catalogues = workspace_.catalogueOf(engine);
            if (catalogues == nullptr) {
                completion(std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                                       .message = "That engine is not configured",
                                                       .context = {}}));
                return;
            }
            auto* watcher = new QFutureWatcher<bench::DynamicPlaylistService::Result>(this);
            connect(watcher, &QFutureWatcher<bench::DynamicPlaylistService::Result>::finished,
                    this, [watcher, completion = std::move(completion)] {
                        completion(watcher->future().takeResult());
                        watcher->deleteLater();
                    });
            watcher->setFuture(QtConcurrent::run(
                [catalogues, compiled = std::move(compiled),
                 cancellation]() -> bench::DynamicPlaylistService::Result {
                    const auto catalogue = catalogues->open();
                    return bench::queryDynamicLibrary(*catalogue, compiled, cancellation);
                }));
        };
    std::vector<bench::DynamicPlaylistSession::Library> libraries;
    for (const auto& engine : workspace_.engines_) {
        if (engine->catalogue != nullptr) {
            libraries.push_back({engine->key, engine->key.isLocal()
                                                  ? tr("This computer")
                                                  : engine->catalogue->name()});
        }
    }
    auto* session =
        new bench::DynamicPlaylistSession(profile, std::move(libraries), std::move(search));
    session->results()->setListeningHistoryService(workspace_.persistence_);
    auto* dynamic = new QuickDynamic(session, *this, this);
    // Rules follow changes to the library they read, and only that one.
    if (workspace_.persistence_ != nullptr) {
        connect(workspace_.persistence_, &ui::ListPersistenceService::listeningHistoryChanged,
                session, &bench::DynamicPlaylistSession::libraryChanged);
    }
    for (const auto& library : libraries_) {
        if (library.browser == nullptr) {
            continue;
        }
        const auto changed = [session, key = library.engine] {
            if (session->engine() == key)
                session->libraryChanged();
        };
        connect(library.browser, &bench::LibraryBrowser::ratingsChanged, session, changed);
        connect(library.browser, &bench::LibraryBrowser::libraryContentChanged, session, changed);
    }
    followDynamic(dynamic);
    QQmlEngine::setObjectOwnership(dynamic, QQmlEngine::CppOwnership);
    return dynamic;
}

void QuickWorkspace::followDynamic(QuickDynamic* dynamic) {
    if (dynamic == nullptr) {
        return;
    }
    // As Search does: opened from a remote tab, it starts on the remote's
    // library; the dropdown switches.
    const auto* current = currentTabPointer();
    dynamic->session()->followLibrary(
        current != nullptr && workspace_.catalogueOf(bench::EngineKey::of(current->document))
            ? bench::EngineKey::of(current->document)
            : bench::EngineKey::local());
}

QuickPick* QuickWorkspace::openQuickPick(const bool albums) {
    // The library of the tab in front: a remote tab's albums come from its
    // engine and go where that tab's would.
    const auto* current = currentTabPointer();
    auto engine = current != nullptr ? bench::EngineKey::of(current->document)
                                     : bench::EngineKey::local();
    if (workspace_.catalogueOf(engine) == nullptr ||
        std::ranges::find(libraries_, engine, &Library::engine) == libraries_.end()) {
        engine = bench::EngineKey::local();
    }
    const auto* source = workspace_.catalogueOf(engine);
    const auto library = std::ranges::find(libraries_, engine, &Library::engine);
    if (source == nullptr || library == libraries_.end() || library->browser == nullptr) {
        return nullptr;
    }
    auto* session = new bench::QuickPickSession(
        albums ? bench::QuickPickKind::album : bench::QuickPickKind::track,
        std::shared_ptr<engine::Catalogue>{source->open()}, source->name());
    // Exactly what the library's own menu does with it.
    connect(session, &bench::QuickPickSession::chosen, library->browser,
            [browser = library->browser](std::vector<persistence::LibraryEntry> picked,
                                         bench::LocalLibraryAction action) {
                emit browser->actionRequested(std::move(picked), action);
            });
    auto* pick = new QuickPick(session, this);
    QQmlEngine::setObjectOwnership(pick, QQmlEngine::CppOwnership);
    return pick;
}

QStringList QuickWorkspace::workspaceCommandIds() {
    QStringList ids;
    for (const auto* id : bench::workspace_command_ids) {
        ids.append(QString::fromLatin1(id));
    }
    return ids;
}

bool QuickWorkspace::commandMatches(const QString& filter, const QString& name,
                                    const QString& shortcut, const QString& id) {
    return bench::commandMatches(filter, name, shortcut, id);
}

void QuickWorkspace::bookmarkFolder(const QUrl& folder) {
    const auto encoded = QFile::encodeName(folder.toLocalFile());
    if (encoded.isEmpty()) {
        return;
    }
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    folders_.addBookmark(raw_path);
    selectSource(0, true);
    folders_.reveal(raw_path);
}

QString QuickWorkspace::nativeShortcut(const QString& portable) {
    return QKeySequence(portable, QKeySequence::PortableText).toString(QKeySequence::NativeText);
}

bool QuickWorkspace::panelAnimations() {
    return QSettings{}.value(QStringLiteral("appearance/panel-animations"), true).toBool();
}

bench::LibraryBrowser* QuickWorkspace::localLibrary() const {
    const auto found = std::ranges::find(libraries_, bench::EngineKey::local(), &Library::engine);
    return found != libraries_.end() ? found->browser : nullptr;
}

void QuickWorkspace::removeSelectedRows() {
    if (auto* tab = currentTabPointer(); tab != nullptr) {
        workspace_.removeRows(*tab, rows_.selectedRows());
    }
}

void QuickWorkspace::undoListEdit() {
    if (workspace_.replayListEdit(currentTabPointer(), true)) {
        refreshSelectionStatus();
    }
    refreshHistory();
}

void QuickWorkspace::redoListEdit() {
    if (workspace_.replayListEdit(currentTabPointer(), false)) {
        refreshSelectionStatus();
    }
    refreshHistory();
}

void QuickWorkspace::refreshHistory() {
    const auto texts = workspace_.historyTexts(currentTabPointer());
    QVariantMap history{{QStringLiteral("canUndo"), texts.can_undo},
                        {QStringLiteral("canRedo"), texts.can_redo},
                        {QStringLiteral("undoText"), texts.undo},
                        {QStringLiteral("redoText"), texts.redo},
                        {QStringLiteral("editable"), texts.editable}};
    if (history != history_) {
        history_ = std::move(history);
        emit historyChanged();
    }
}

void QuickWorkspace::jumpToPlaying() { refreshPlaybackCursor(true); }

void QuickWorkspace::transferSelection(const QString& target_id, const bool move) {
    transferOf(shownPicked(), target_id, move);
}

void QuickWorkspace::transferOf(const Picked& picked, const QString& target_id, const bool move) {
    if (picked.model == nullptr || picked.rows.empty()) {
        return;
    }
    workspace_.transferRows(picked.tab, picked.model, picked.engine, false, picked.rows, target_id,
                            move && picked.tab != nullptr, -1);
}

void QuickWorkspace::transferSelectionToNewTab(const QString& name, const bool move) {
    transferToNewTabOf(shownPicked(), name, move);
}

void QuickWorkspace::transferToNewTabOf(const Picked& picked, const QString& name,
                                        const bool move) {
    if (picked.model == nullptr || picked.rows.empty() || name.trimmed().isEmpty()) {
        return;
    }
    if (auto* destination = workspace_.transferRowsToNewList(
            picked.tab, picked.model, picked.engine, false, picked.rows,
            move && picked.tab != nullptr, name.trimmed());
        destination != nullptr) {
        showList(*destination);
    }
}

QVariantList QuickWorkspace::otherLists() const {
    QVariantList lists;
    auto* shown = currentTabPointer();
    for (const auto& tab : workspace_.list_tabs_) {
        if (tab.get() == shown) {
            continue;
        }
        lists.push_back(
            QVariantMap{{QStringLiteral("id"), bench::document_text(tab->document.id)},
                        {QStringLiteral("name"), bench::displayText(tab->document.name)}});
    }
    return lists;
}

QVariantMap QuickWorkspace::ratingState() const { return ratingStateOf(shownPicked()); }

QVariantMap QuickWorkspace::ratingStateOf(const Picked& picked) const {
    QVariantMap state{{QStringLiteral("canRate"), false},
                      {QStringLiteral("canRateAlbum"), false},
                      {QStringLiteral("rating"), -1},
                      {QStringLiteral("albumRating"), -1}};
    if (picked.model == nullptr) {
        return state;
    }
    const auto& rows = picked.model->rows();
    std::optional<unsigned> rating;
    std::optional<unsigned> album_rating;
    bool ratings_match = true;
    bool album_ratings_match = true;
    bool tracks = false;
    bool albums = false;
    for (const auto row : picked.rows) {
        const auto& track = rows[static_cast<std::size_t>(row)];
        if (!rating) {
            rating = track.rating;
        } else if (*rating != track.rating) {
            ratings_match = false;
        }
        if (!album_rating) {
            album_rating = track.album_rating;
        } else if (*album_rating != track.album_rating) {
            album_ratings_match = false;
        }
        tracks = tracks || !track.rating_hash.empty();
        albums = albums || !track.album_rating_hash.empty();
    }
    const auto ready = workspace_.canRate(picked.engine);
    state.insert(QStringLiteral("canRate"), ready && tracks);
    state.insert(QStringLiteral("canRateAlbum"), ready && albums);
    state.insert(QStringLiteral("rating"),
                 rating && ratings_match ? static_cast<int>(*rating) : -1);
    state.insert(QStringLiteral("albumRating"),
                 album_rating && album_ratings_match ? static_cast<int>(*album_rating) : -1);
    return state;
}

void QuickWorkspace::rateSelection(const bool album, const int rating) {
    rateOf(shownPicked(), album, rating);
}

void QuickWorkspace::rateOf(const Picked& picked, const bool album, const int rating) {
    if (picked.model == nullptr) {
        return;
    }
    QStringList hashes;
    for (const auto row : picked.rows) {
        const auto& track = picked.model->rows()[static_cast<std::size_t>(row)];
        const auto hash =
            QString::fromStdString(album ? track.album_rating_hash : track.rating_hash);
        if (!hash.isEmpty() && !hashes.contains(hash)) {
            hashes.push_back(hash);
        }
    }
    workspace_.rate(picked.engine, hashes, album,
                    static_cast<unsigned>(std::clamp(rating, 0, 10)));
}

QString QuickWorkspace::ratingLabel(const int rating) {
    return ui::ratingMenuLabel(static_cast<unsigned>(std::clamp(rating, 0, 10)));
}

QVariantMap QuickWorkspace::lastFmTrack() const { return lastFmTrackOf(shownPicked()); }

QVariantMap QuickWorkspace::lastFmTrackOf(const Picked& picked) const {
    if (picked.model == nullptr || picked.rows.size() != 1U || workspace_.lastfm_ == nullptr) {
        return {};
    }
    const auto& track = picked.model->rows()[static_cast<std::size_t>(picked.rows.front())];
    return {{QStringLiteral("artist"), QString::fromStdString(track.artist)},
            {QStringLiteral("title"), QString::fromStdString(track.title)}};
}

void QuickWorkspace::askLastFm() { askLastFmOf(lastFmTrack()); }

void QuickWorkspace::askLastFmOf(const QVariantMap& track) {
    const auto artist = track.value(QStringLiteral("artist")).toString();
    const auto title = track.value(QStringLiteral("title")).toString();
    lastfm_state_ = QStringLiteral("Checking loved state…");
    emit lastFmStateChanged();
    if (artist.isEmpty() || title.isEmpty()) {
        return;
    }
    // One answer is waited for at a time; the menu asks again when shown.
    disconnect(lastfm_answer_);
    lastfm_answer_ = connect(
        workspace_.lastfm_, &bench::LastFmService::completed, this,
        [this, artist, title](const QString& op, const QJsonObject& state, const QString& error) {
            if (op != QStringLiteral("info")) {
                return;
            }
            if (!error.isEmpty()) {
                lastfm_state_ = error;
            } else if (state.value(QStringLiteral("artist")).toString() == artist &&
                       state.value(QStringLiteral("title")).toString() == title) {
                lastfm_state_ = state.value(QStringLiteral("loved")).toBool()
                                    ? QStringLiteral("♥ Loved on Last.fm")
                                    : QStringLiteral("Not loved on Last.fm");
            }
            emit lastFmStateChanged();
        });
    workspace_.lastfm_->execute(QStringLiteral("info"), {artist, title});
}

void QuickWorkspace::loveOnLastFm(const bool love) { loveOnLastFmOf(lastFmTrack(), love); }

void QuickWorkspace::loveOnLastFmOf(const QVariantMap& track, const bool love) {
    if (track.isEmpty() || workspace_.lastfm_ == nullptr) {
        return;
    }
    workspace_.lastfm_->execute(love ? QStringLiteral("love") : QStringLiteral("unlove"),
                                {track.value(QStringLiteral("artist")).toString(),
                                 track.value(QStringLiteral("title")).toString()});
}

QVariantList QuickWorkspace::columns() const {
    QVariantList columns;
    auto* tab = currentTabPointer();
    if (tab == nullptr) {
        return columns;
    }
    for (const auto& column : tab->view_layout.columns) {
        const auto logical = bench::trackColumnLogical(column.id);
        const auto spec =
            std::ranges::find(bench::track_column_specs, logical, &bench::TrackColumnSpec::logical);
        if (spec == bench::track_column_specs.end()) {
            continue;
        }
        columns.push_back(QVariantMap{
            {QStringLiteral("id"), column.id},
            {QStringLiteral("logical"), logical},
            {QStringLiteral("label"), QString::fromLatin1(spec->label)},
            {QStringLiteral("header"),
             QString::fromLatin1(ui::track_column_headers[static_cast<std::size_t>(logical)])},
            {QStringLiteral("width"), column.width},
            {QStringLiteral("minimum"), spec->minimum_width},
            {QStringLiteral("visible"), column.visible},
        });
    }
    return columns;
}

void QuickWorkspace::setColumnWidth(const QString& id, const int width) {
    auto* tab = currentTabPointer();
    if (tab == nullptr) {
        return;
    }
    for (auto& column : tab->view_layout.columns) {
        if (column.id == id) {
            column.width = std::clamp(width, 24, 4096);
        }
    }
    tab->view_layout_persistence_protected = false;
    tab->preserved_view_layout.clear();
    rows_.setLayout(tab->view_layout);
    workspace_.schedulePersist();
}

void QuickWorkspace::setPresentation(const QString& presentation) {
    auto* tab = currentTabPointer();
    const auto chosen = ui::trackViewPresentationFromId(presentation);
    if (tab == nullptr || !chosen) {
        return;
    }
    // Choosing a presentation replaces the list's columns with its defaults.
    tab->view_layout = bench::Workspace::defaultTrackViewLayout(*chosen);
    tab->view_layout_persistence_protected = false;
    tab->preserved_view_layout.clear();
    rows_.setLayout(tab->view_layout);
    workspace_.schedulePersist();
    refreshList();
}

void QuickWorkspace::setColumnVisible(const QString& id, const bool visible) {
    if (auto* tab = currentTabPointer();
        tab != nullptr && workspace_.setColumnVisible(*tab, tab->view_layout, id, visible)) {
        rows_.setLayout(tab->view_layout);
        refreshList();
    }
}

void QuickWorkspace::resetLayout() {
    if (auto* tab = currentTabPointer(); tab != nullptr) {
        workspace_.setTrackViewLayout(*tab, bench::Workspace::defaultTrackViewLayout());
        rows_.setLayout(tab->view_layout);
        refreshList();
    }
}

void QuickWorkspace::copyLayoutToAll() {
    auto* source = currentTabPointer();
    if (source == nullptr) {
        return;
    }
    const auto layout = source->view_layout;
    for (auto& tab : workspace_.list_tabs_) {
        workspace_.setTrackViewLayout(*tab, layout);
    }
}

void QuickWorkspace::newList(const QString& name) { workspace_.createList(name); }

void QuickWorkspace::duplicateTab() {
    if (auto* tab = currentTabPointer(); tab != nullptr) {
        if (auto* duplicated = workspace_.duplicateList(*tab);
            duplicated != nullptr && duplicated == currentTabPointer()) {
            rows_.setLayout(duplicated->view_layout);
        }
    }
}

void QuickWorkspace::togglePinned() {
    if (auto* tab = currentTabPointer(); tab != nullptr) {
        workspace_.togglePinned(*tab);
    }
}

void QuickWorkspace::saveTab(const QString& name) {
    if (auto* tab = currentTabPointer(); tab != nullptr) {
        workspace_.saveList(*tab, name);
    }
}

void QuickWorkspace::renameTab(const QString& name) {
    if (auto* tab = currentTabPointer(); tab != nullptr) {
        workspace_.renameList(*tab, name);
    }
}

void QuickWorkspace::openUrls(const QList<QUrl>& urls) {
    std::vector<std::string> paths;
    for (const auto& url : urls) {
        if (url.isLocalFile()) {
            const auto encoded = QFile::encodeName(url.toLocalFile());
            paths.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
        }
    }
    if (!paths.empty()) {
        workspace_.openLocalPaths(std::move(paths));
    }
}

bool QuickWorkspace::notifications() const {
    return notifier_ != nullptr && notifier_->isEnabled();
}

void QuickWorkspace::setNotifications(const bool on) {
    QSettings{}.setValue(QStringLiteral("desktop/notifications"), on);
    if (notifier_ != nullptr) {
        notifier_->setEnabled(on);
    }
    emit desktopChanged();
}

void QuickWorkspace::setFollowPlayback(const bool on) {
    follow_playback_ = on;
    QSettings{}.setValue(QStringLiteral("workspace/follow-playback"), on);
    emit desktopChanged();
    if (on) {
        followed_tab_ = nullptr;
        refreshPlaybackCursor(false);
    }
}

// --- Transport --------------------------------------------------------------

void QuickWorkspace::playPause() { workspace_.togglePlayPause(); }

void QuickWorkspace::setVolume(const int percent) {
    if (percent > 0) {
        unmuted_volume_ = percent;
    }
    workspace_.setVolume(percent);
}

void QuickWorkspace::toggleMute() {
    const auto volume = transport_.value(QStringLiteral("volume")).toInt();
    if (volume > 0) {
        unmuted_volume_ = volume;
        workspace_.setVolume(0);
    } else {
        workspace_.setVolume(unmuted_volume_);
    }
}

QVariantList QuickWorkspace::replayGainModes() const {
    QVariantList modes;
    for (const auto& [label, value] : bench::Workspace::replayGainModes()) {
        modes.push_back(
            QVariantMap{{QStringLiteral("label"), label},
                        {QStringLiteral("value"), value},
                        {QStringLiteral("checked"), value == workspace_.local_replaygain_}});
    }
    return modes;
}

void QuickWorkspace::selectOutput(const QString& id) { workspace_.selectOutput(id.toStdString()); }

void QuickWorkspace::setOutputDevice(const QVariant& target) {
    const auto name = target.toString();
    workspace_.setOutputDevice(name.isEmpty() ? std::nullopt : std::optional{name.toStdString()});
}

QVariantList QuickWorkspace::bufferProfiles() const {
    QVariantList profiles;
    for (const auto preset :
         {audio::PlaybackBufferPreset::responsive, audio::PlaybackBufferPreset::balanced,
          audio::PlaybackBufferPreset::resilient}) {
        const auto id = audio::playback_buffer_preset_id(preset);
        const auto value = QString::fromLatin1(id.data(), static_cast<qsizetype>(id.size()));
        const auto config = audio::playback_buffer_preset_config(preset);
        profiles.push_back(QVariantMap{
            {QStringLiteral("value"), value},
            {QStringLiteral("label"), bench::Workspace::bufferProfileLabel(value)},
            {QStringLiteral("tooltip"), QStringLiteral("%1 ms capacity; playback starts at %2 ms")
                                            .arg(config.capacity.count())
                                            .arg(config.start_threshold.count())},
        });
    }
    return profiles;
}

void QuickWorkspace::setBufferProfile(const QString& profile) {
    const auto preset = audio::playback_buffer_preset_from_id(profile.toStdString());
    if (!preset) {
        return;
    }
    const auto config = audio::playback_buffer_preset_config(*preset);
    workspace_.configurePlaybackBuffer(profile, static_cast<int>(config.capacity.count()),
                                       static_cast<int>(config.start_threshold.count()));
}

void QuickWorkspace::refreshTransport() {
    QVariantMap shown;
    const auto playing_on_engine = workspace_.playingOnEngine();
    shown.insert(QStringLiteral("engine"), playing_on_engine);
    if (!playing_on_engine) {
        // ADR-0226: without an engine nothing plays; this window has no
        // player of its own to fall back on.
        refreshPlaybackCursor(false);
        shown.insert(QStringLiteral("title"), QStringLiteral("No engine"));
        shown.insert(QStringLiteral("windowTitle"), QStringLiteral("Trackknife"));
        shown.insert(QStringLiteral("outputTooltip"), QStringLiteral("No engine is connected"));
        shown.insert(QStringLiteral("playLabel"), QStringLiteral("Play"));
        // The level last shown stays, as the widgets slider keeps its value.
        shown.insert(QStringLiteral("volume"), transport_.value(QStringLiteral("volume"), 100));
        workspace_.publishDesktopState();
    } else {
        const auto state = workspace_.transport_->state();
        workspace_.sampleLastFm(state);
        const auto playing = state.status == QStringLiteral("playing");
        const auto stopped = state.status == QStringLiteral("stopped");
        // Anything the engine could act on enables the control.
        shown.insert(QStringLiteral("canPlayPause"), !stopped || state.queue_size > 0U);
        shown.insert(QStringLiteral("canStop"), !stopped || state.queue_size > 0U);
        shown.insert(QStringLiteral("canNext"), state.queue_size > 1U);
        shown.insert(QStringLiteral("canPrevious"), state.queue_size > 1U);
        shown.insert(QStringLiteral("playing"), playing);
        shown.insert(QStringLiteral("playLabel"),
                     playing ? QStringLiteral("Pause") : QStringLiteral("Play"));
        const auto duration_ms =
            std::clamp<qint64>(state.duration_ms, 0, std::numeric_limits<int>::max());
        shown.insert(QStringLiteral("durationMs"), duration_ms);
        shown.insert(QStringLiteral("positionMs"),
                     std::clamp<qint64>(state.position_ms, 0, duration_ms));
        shown.insert(QStringLiteral("elapsed"), bench::formatTime(state.position_ms));
        shown.insert(QStringLiteral("duration"), bench::formatTime(duration_ms));
        shown.insert(QStringLiteral("seekEnabled"), duration_ms > 0);
        // Not while this window's own commands are on their way: a report
        // from before them would put back the volume just changed.
        shown.insert(QStringLiteral("settling"), workspace_.transport_->settling());
        shown.insert(QStringLiteral("volume"), state.volume_percent);
        const auto now = workspace_.nowPlaying(state);
        shown.insert(QStringLiteral("title"), now.title);
        shown.insert(QStringLiteral("context"), now.context);
        shown.insert(QStringLiteral("tooltip"), now.tooltip);
        shown.insert(QStringLiteral("windowTitle"), now.window_title);
        QString cover_key;
        QString album;
        if (const auto* row = workspace_.playingRow(now.cover_entry); row != nullptr) {
            cover_key = bench::LocalListModel::groupKeyOf(*row);
            album = QString::fromStdString(row->album);
        }
        shown.insert(QStringLiteral("coverKey"), cover_key);
        shown.insert(QStringLiteral("album"), album);
        workspace_.followEngineState(state);
        refreshOutputControls(state);
        for (const auto& [key, value] : output_summary_.asKeyValueRange()) {
            shown.insert(key, value);
        }
        workspace_.publishDesktopState();
        refreshPlaybackCursor(false);
    }
    if (shown != transport_) {
        transport_ = std::move(shown);
        emit transportChanged();
    }
}

void QuickWorkspace::refreshOutputControls(const bench::EnginePlayback::State& state) {
    const auto outputs = workspace_.takeOutputs(state);
    output_summary_ = {
        {QStringLiteral("output"), outputs.shown},
        {QStringLiteral("outputTooltip"), outputs.tooltip},
        {QStringLiteral("outputDescription"), outputs.description},
    };
    if (!outputs.menu_changed && !output_menu_.isEmpty()) {
        return;
    }
    const auto menu = workspace_.outputMenu();
    QVariantList entries;
    if (menu.speakers_shown) {
        entries.push_back(QVariantMap{{QStringLiteral("kind"), QStringLiteral("heading")},
                                      {QStringLiteral("label"), QStringLiteral("Speakers")}});
        for (const auto& speaker : menu.speakers) {
            entries.push_back(
                QVariantMap{{QStringLiteral("kind"), QStringLiteral("speaker")},
                            {QStringLiteral("id"), QString::fromStdString(speaker.id)},
                            {QStringLiteral("label"), speaker.label},
                            {QStringLiteral("tooltip"), speaker.tooltip},
                            {QStringLiteral("checked"), speaker.checked},
                            {QStringLiteral("enabled"), true}});
        }
        entries.push_back(QVariantMap{{QStringLiteral("kind"), QStringLiteral("separator")}});
        entries.push_back(QVariantMap{{QStringLiteral("kind"), QStringLiteral("heading")},
                                      {QStringLiteral("label"), menu.devices_heading}});
    }
    for (const auto& device : menu.devices) {
        entries.push_back(
            QVariantMap{{QStringLiteral("kind"), QStringLiteral("device")},
                        {QStringLiteral("target"),
                         device.target ? QString::fromStdString(*device.target) : QString{}},
                        {QStringLiteral("label"), device.label},
                        {QStringLiteral("checked"), device.checked},
                        {QStringLiteral("enabled"), device.enabled}});
    }
    output_menu_ = std::move(entries);
    emit outputMenuChanged();
}

void QuickWorkspace::refreshPlaybackCursor(const bool jump) {
    if (workspace_.playback_.requests.active()) {
        return;
    }
    if (!jump && !followPlayback()) {
        refreshList();
        return;
    }
    auto* tab = workspace_.tabForDocument(workspace_.playback_.anchors.document);
    if (tab == nullptr) {
        refreshList();
        return;
    }
    const auto row = workspace_.resolvePlaybackRow(tab);
    const auto index = tabs_.indexOf(tab);
    if (row < 0 || index < 0) {
        refreshList();
        return;
    }
    if (!jump && (index != current_ || (followed_tab_ == tab && followed_row_ == row))) {
        refreshList();
        return;
    }
    followed_tab_ = tab;
    followed_row_ = row;
    if (jump) {
        setCurrentTab(index);
    }
    refreshList();
    emit playbackCursor(row, jump);
}

void QuickWorkspace::refreshLocalPlaybackControls() {
    const auto texts = workspace_.modeTexts();
    const auto mode = [](const bench::Workspace::ModeText& text) {
        return QVariantMap{{QStringLiteral("text"), text.text},
                           {QStringLiteral("tooltip"), text.tooltip},
                           {QStringLiteral("checked"), text.checked},
                           {QStringLiteral("oneshot"), text.oneshot}};
    };
    QVariantMap modes{
        {QStringLiteral("enabled"), workspace_.playingOnEngine()},
        {QStringLiteral("repeat"), mode(texts.repeat)},
        {QStringLiteral("random"), mode(texts.random)},
        {QStringLiteral("albumRandom"), mode(texts.album_random)},
        {QStringLiteral("single"), mode(texts.single)},
        {QStringLiteral("consume"), mode(texts.consume)},
        {QStringLiteral("replaygain"), texts.replaygain},
        {QStringLiteral("replaygainTooltip"), texts.replaygain_tooltip},
        {QStringLiteral("replaygainActive"), texts.replaygain_active},
        {QStringLiteral("replaygainMode"), workspace_.local_replaygain_},
    };
    if (modes != modes_) {
        modes_ = std::move(modes);
        emit modesChanged();
    }
}

void QuickWorkspace::openReference(const int reference) {
    QString error;
    if (!bench::openLanguageReference(reference == 1 ? bench::LanguageReference::scripts
                                                     : bench::LanguageReference::formatting,
                                      &error)) {
        showMessage(error, 6000);
    }
}

// --- Up Next ----------------------------------------------------------------


void QuickWorkspace::refreshUpNext() {
    workspace_.syncUpNextModel();
    const auto heading = workspace_.upNextHeading();
    QVariantMap up_next{
        {QStringLiteral("status"), heading.status},
        {QStringLiteral("statusTooltip"), heading.status_tooltip},
        {QStringLiteral("back"), heading.back},
        {QStringLiteral("backTooltip"), heading.back_tooltip},
        {QStringLiteral("backEnabled"), heading.back_enabled},
        {QStringLiteral("canUndo"), heading.can_undo},
        {QStringLiteral("count"), workspace_.up_next_local_model_ != nullptr
                                      ? workspace_.up_next_local_model_->rowCount()
                                      : 0},
    };
    QList<QVariantMap> rows;
    if (auto* model = workspace_.up_next_local_model_; model != nullptr) {
        for (int row = 0; row < model->rowCount(); ++row) {
            rows.push_back(QVariantMap{
                {QStringLiteral("title"),
                 model->index(row, bench::local_title_column).data().toString()},
                {QStringLiteral("artist"),
                 model->index(row, bench::local_artist_column).data().toString()},
                {QStringLiteral("album"),
                 model->index(row, bench::local_album_column).data().toString()},
                {QStringLiteral("length"),
                 model->index(row, bench::local_length_column).data().toString()},
                {QStringLiteral("coverKey"),
                 model->index(row, 0).data(ui::track_album_artwork_key_role).toString()},
            });
        }
    }
    // Each track by its entry; should the two ever be out of step, by place.
    auto keys = workspace_.up_next_display_ids_;
    if (keys.size() != static_cast<std::size_t>(rows.size())) {
        keys.clear();
        for (qsizetype row = 0; row < rows.size(); ++row) {
            keys.push_back(static_cast<std::uint64_t>(row));
        }
    }
    up_next_rows_.update(keys, rows);
    if (up_next != up_next_) {
        up_next_ = std::move(up_next);
        emit upNextChanged();
    }
}

void QuickWorkspace::editUpNext(const int operation, const int row, const int destination) {
    workspace_.editUpNext(operation, row, destination);
}

void QuickWorkspace::editUpNextRows(const QVariantList& rows, const int operation,
                                    const int destination) {
    const auto count = workspace_.up_next_local_model_ != nullptr
                           ? workspace_.up_next_local_model_->rowCount()
                           : 0;
    std::vector<bool> selected(static_cast<std::size_t>(count), false);
    for (const auto& row : rows) {
        if (const auto at = row.toInt(); at >= 0 && at < count) {
            selected[static_cast<std::size_t>(at)] = true;
        }
    }
    workspace_.editUpNextRows(std::move(selected), operation, destination);
}

void QuickWorkspace::playUpNextRow(const int row) { workspace_.playUpNextRow(row); }

void QuickWorkspace::returnToList() { workspace_.returnToList(); }

void QuickWorkspace::queueSelection(const bool next) { queueOf(shownPicked(), next); }

void QuickWorkspace::queueOf(const Picked& picked, const bool next) {
    if (picked.model == nullptr || picked.rows.empty()) {
        return;
    }
    std::vector<bench::LocalTrackRow> tracks;
    for (const auto row : picked.rows) {
        if (row >= 0 && row < picked.model->rowCount()) {
            tracks.push_back(picked.model->rows()[static_cast<std::size_t>(row)]);
        }
    }
    workspace_.enqueueLocalRequests(std::move(tracks), next ? 0 : -1, picked.engine);
    refreshUpNext();
}

// --- Closing ----------------------------------------------------------------

void QuickWorkspace::closeWindow() {
    transport_timer_.stop();
    workspace_.persistNow(true);
    emit quitRequested();
}

void QuickWorkspace::quitAndStopEngine() {
    transport_timer_.stop();
    workspace_.persistNow(true);
    workspace_.retireEngines();
    emit quitRequested();
}

QString QuickWorkspace::formatTime(const qint64 milliseconds) {
    return bench::formatTime(milliseconds);
}

QImage QuickWorkspace::cover(const QString& key) const {
    for (const auto& tab : workspace_.list_tabs_) {
        if (tab->model->hasArtwork(key)) {
            return tab->model->artwork(key);
        }
    }
    return workspace_.artwork_cache_.value(key);
}

// --- bench::WorkspaceView -----------------------------------------------------

void QuickWorkspace::workspaceRestored(const bool restored) {
    if (restored) {
        addLibrary(workspace_.localEngine());
    }
    refreshList();
    refreshUpNext();
}

void QuickWorkspace::showMessage(const QString& text, const int timeout_ms) {
    emit message(text, timeout_ms);
}

void QuickWorkspace::listAdded(ListTab& tab, const bool select) {
    tab.view_layout = workspace_.restoredTrackViewLayout(tab);
    const auto row = tabs_.insert(tab);
    if (current_ >= row) {
        ++current_;
    }
    connect(tab.model, &bench::LocalListModel::historyRowsRestored, this,
            [this, model = tab.model](const QList<int>& rows) {
                if (listModel() != model) {
                    return;
                }
                QVariantList restored;
                for (const auto at : rows) {
                    restored.push_back(at);
                }
                rows_.selectRows(restored);
                emit rowsRestored(restored);
            });
    for (const auto signal :
         {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved}) {
        connect(tab.model, signal, this, [this] { refreshHistory(); });
    }
    connect(tab.model, &QAbstractItemModel::modelReset, this, [this] { refreshHistory(); });
    if (select || current_ < 0) {
        current_ = -1;
        setCurrentTab(row);
    } else {
        emit currentTabChanged();
    }
}

void QuickWorkspace::showList(ListTab& tab) {
    if (const auto index = tabs_.indexOf(&tab); index >= 0) {
        setCurrentTab(index);
    }
}

bench::Workspace::ListTab* QuickWorkspace::currentList() { return currentTabPointer(); }

std::vector<bench::Workspace::ListTab*> QuickWorkspace::listsInOrder() { return tabs_.tabs(); }

ui::TrackViewLayout QuickWorkspace::captureTrackViewLayout(const ListTab& tab) const {
    return tab.view_layout;
}

void QuickWorkspace::refreshTabChrome(ListTab& tab) {
    tabs_.refresh(tab);
    if (&tab == currentTabPointer()) {
        refreshList();
    }
}

void QuickWorkspace::refreshListHistoryActions() { refreshHistory(); }

void QuickWorkspace::refreshSelectionStatus() {
    auto* tab = currentTabPointer();
    const auto selected = tab != nullptr ? rows_.selectedRows() : std::vector<int>{};
    const auto summary = workspace_.selectionSummary(tab, selected);
    QVariantMap selection{{QStringLiteral("text"), summary.text},
                          {QStringLiteral("tooltip"), summary.tooltip},
                          {QStringLiteral("count"), static_cast<int>(selected.size())}};
    if (selection != selection_) {
        selection_ = std::move(selection);
        emit selectionChanged();
    }
}

void QuickWorkspace::engineConnected(EngineLink& engine, bool) {
    addLibrary(engine);
    emit currentTabChanged();
}

// Now answering, the engine is called as it says -- not by the address
// Settings gave -- in the sources and on its lists' tabs.
void QuickWorkspace::engineAttached(EngineLink& engine) {
    for (auto* tab : tabs_.tabs()) {
        if (bench::EngineKey::of(tab->document) == engine.key) {
            tabs_.refresh(*tab);
        }
    }
    emit sourcesChanged();
    refreshList();
}

void QuickWorkspace::engineRekeyed(EngineLink& engine, const bench::EngineKey& from, bool) {
    for (auto& library : libraries_) {
        if (library.engine == from) {
            library.engine = engine.key;
            library.browser->setEngine(engine.key);
        }
    }
    emit sourcesChanged();
    tabs_.layoutChanged();
    refreshList();
}

void QuickWorkspace::engineRemoving(EngineLink& engine) {
    const auto found = std::ranges::find(libraries_, engine.key, &Library::engine);
    if (found == libraries_.end()) {
        return;
    }
    auto* browser = found->browser;
    libraries_.erase(found);
    browser->stop();
    browser->deleteLater();
    selectPreferredSource();
}

void QuickWorkspace::engineRemoved() { refreshList(); }

void QuickWorkspace::enginesSynced() {}

void QuickWorkspace::engineRatingsChanged(const bench::EngineKey&,
                                          const QHash<QString, unsigned>&) {}

void QuickWorkspace::engineInterruptionsChanged(bool) {}

void QuickWorkspace::artworkLoaded(const QString&) {
    ++cover_revision_;
    emit coverRevisionChanged();
}

} // namespace trackknife::quick
