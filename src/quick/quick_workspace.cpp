// SPDX-License-Identifier: GPL-3.0-only

#include "quick/quick_workspace.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "bench/desktop_notifier.hpp"
#include "bench/mpris_service.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "uicommon/track_row_roles.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
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

QuickWorkspace::QuickWorkspace(QObject* parent) : QObject(parent) {
    workspace_.setView(this);
    connect(&rows_, &TrackRowsModel::selectionChanged, this, &QuickWorkspace::selectionEdited);
    transport_timer_.setInterval(transport_refresh_ms);
    connect(&transport_timer_, &QTimer::timeout, this, &QuickWorkspace::refreshTransport);
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

void QuickWorkspace::start() {
    // ADR-0226: this window plays nothing itself. The engine owns playback,
    // and the buffer shown here is the one it reports.
    workspace_.selected_buffer_profile_ = bench::Workspace::loadPlaybackBufferPreference().profile;
    follow_playback_ = QSettings{}.value(QStringLiteral("workspace/follow-playback"), false).toBool();
    workspace_.startLastFm();
    workspace_.start();
    transport_timer_.start();
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
        list.insert(QStringLiteral("accessibleName"),
                    workspace_.tabChrome(*tab).accessible_name);
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
    } else {
        rows_.setSource(nullptr, {});
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
    if (auto* tab = currentTabPointer(); tab != nullptr && row >= 0 && row < tab->model->rowCount()) {
        workspace_.playRow(*tab, row);
    }
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
        modes.push_back(QVariantMap{{QStringLiteral("label"), label},
                                    {QStringLiteral("value"), value},
                                    {QStringLiteral("checked"),
                                     value == workspace_.local_replaygain_}});
    }
    return modes;
}

void QuickWorkspace::selectOutput(const QString& id) { workspace_.selectOutput(id.toStdString()); }

void QuickWorkspace::setOutputDevice(const QVariant& target) {
    const auto name = target.toString();
    workspace_.setOutputDevice(name.isEmpty() ? std::nullopt
                                              : std::optional{name.toStdString()});
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
            {QStringLiteral("tooltip"),
             QStringLiteral("%1 ms capacity; playback starts at %2 ms")
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
        if (const auto* row = workspace_.playingRow(now.cover_entry); row != nullptr) {
            cover_key = bench::LocalListModel::groupKeyOf(*row);
        }
        shown.insert(QStringLiteral("coverKey"), cover_key);
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
            entries.push_back(QVariantMap{{QStringLiteral("kind"), QStringLiteral("speaker")},
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
        entries.push_back(QVariantMap{
            {QStringLiteral("kind"), QStringLiteral("device")},
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

// --- Up Next ----------------------------------------------------------------

QAbstractItemModel* QuickWorkspace::upNextModel() const { return workspace_.up_next_local_model_; }

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
        {QStringLiteral("count"),
         workspace_.up_next_local_model_ != nullptr ? workspace_.up_next_local_model_->rowCount()
                                                    : 0},
    };
    QVariantList rows;
    if (auto* model = workspace_.up_next_local_model_; model != nullptr) {
        for (int row = 0; row < model->rowCount(); ++row) {
            rows.push_back(QVariantMap{
                {QStringLiteral("title"), model->index(row, bench::local_title_column).data()},
                {QStringLiteral("artist"), model->index(row, bench::local_artist_column).data()},
                {QStringLiteral("length"), model->index(row, bench::local_length_column).data()},
                {QStringLiteral("coverKey"),
                 model->index(row, 0).data(ui::track_album_artwork_key_role)},
            });
        }
    }
    up_next.insert(QStringLiteral("rows"), rows);
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
    const auto count =
        workspace_.up_next_local_model_ != nullptr ? workspace_.up_next_local_model_->rowCount() : 0;
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

void QuickWorkspace::queueSelection(const bool next) {
    if (auto* tab = currentTabPointer(); tab != nullptr && rows_.selectedCount() > 0) {
        workspace_.enqueueRows(*tab, rows_.selectedRows(), next ? 0 : -1);
    }
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

void QuickWorkspace::workspaceRestored(const bool) {
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
    for (const auto signal : {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved}) {
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

void QuickWorkspace::engineConnected(EngineLink&, bool) { emit currentTabChanged(); }

void QuickWorkspace::engineAttached(EngineLink&) { refreshList(); }

void QuickWorkspace::engineRekeyed(EngineLink&, const bench::EngineKey&, bool) {
    tabs_.layoutChanged();
    refreshList();
}

void QuickWorkspace::engineRemoving(EngineLink&) {}

void QuickWorkspace::engineRemoved() { refreshList(); }

void QuickWorkspace::enginesSynced() {}

void QuickWorkspace::engineRatingsChanged(const bench::EngineKey&, const QHash<QString, unsigned>&) {}

void QuickWorkspace::engineInterruptionsChanged(bool) {}

void QuickWorkspace::artworkLoaded(const QString&) {
    ++cover_revision_;
    emit coverRevisionChanged();
}

} // namespace trackknife::quick
