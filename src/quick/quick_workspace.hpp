// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "quick/list_tabs_model.hpp"
#include "quick/quick_convert.hpp"
#include "quick/quick_open_list.hpp"
#include "quick/quick_replaygain.hpp"
#include "quick/quick_settings.hpp"
#include "quick/quick_tagger.hpp"
#include "quick/track_rows_model.hpp"
#include "workspace/folder_browser.hpp"
#include "workspace/library_browser.hpp"
#include "workspace/list_edit_job.hpp"
#include "workspace/playlist_transfer.hpp"
#include "workspace/workspace.hpp"
#include "workspace/workspace_view.hpp"

#include <QAbstractItemModel>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QUrl>
#include <QVariantMap>
#include <QJSEngine>
#include <QQmlEngine>
#include <QtQml/qqmlregistration.h>

#include <vector>

namespace trackknife::bench {
class MprisService;
class DesktopNotifier;
} // namespace trackknife::bench

namespace trackknife::quick {

// ADR-0220: the Qt Quick window's side of the workspace. The workspace does
// everything -- engines, lists, playback, Up Next -- and asks this, as it
// asks the widgets window, to show it; this hands QML what to draw and takes
// back what the user does. Nothing here decides anything the widgets window
// decides for itself: what both say is the workspace's.
class QuickWorkspace final : public QObject, public bench::WorkspaceView {
    Q_OBJECT
    QML_NAMED_ELEMENT(Tk)
    QML_SINGLETON

    Q_PROPERTY(QAbstractItemModel* tabs READ tabs CONSTANT)
    Q_PROPERTY(int currentTab READ currentTab WRITE setCurrentTab NOTIFY currentTabChanged)
    Q_PROPERTY(QAbstractItemModel* listModel READ listModel NOTIFY currentTabChanged)
    Q_PROPERTY(trackknife::quick::TrackRowsModel* rows READ rows CONSTANT)
    Q_PROPERTY(trackknife::bench::ListEditJob* edit READ edit CONSTANT)
    Q_PROPERTY(QString lastFmState READ lastFmState NOTIFY lastFmStateChanged)
    Q_PROPERTY(trackknife::bench::FolderBrowser* folders READ folders CONSTANT)
    Q_PROPERTY(QVariantList sources READ sources NOTIFY sourcesChanged)
    Q_PROPERTY(int currentSource READ currentSource NOTIFY sourcesChanged)
    Q_PROPERTY(trackknife::bench::LibraryBrowser* library READ library NOTIFY sourcesChanged)
    Q_PROPERTY(QVariantMap list READ list NOTIFY listChanged)
    Q_PROPERTY(QVariantMap selection READ selection NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap transport READ transport NOTIFY transportChanged)
    Q_PROPERTY(QVariantMap modes READ modes NOTIFY modesChanged)
    Q_PROPERTY(QVariantList outputMenu READ outputMenu NOTIFY outputMenuChanged)
    Q_PROPERTY(QString bufferProfile READ bufferProfile NOTIFY bufferProfileChanged)
    Q_PROPERTY(QAbstractItemModel* upNextModel READ upNextModel CONSTANT)
    Q_PROPERTY(QVariantMap upNext READ upNext NOTIFY upNextChanged)
    Q_PROPERTY(QVariantMap history READ history NOTIFY historyChanged)
    Q_PROPERTY(int coverRevision READ coverRevision NOTIFY coverRevisionChanged)
    Q_PROPERTY(bool notifications READ notifications WRITE setNotifications NOTIFY desktopChanged)
    Q_PROPERTY(bool followPlayback READ followPlayback WRITE setFollowPlayback NOTIFY desktopChanged)
    Q_PROPERTY(trackknife::bench::LibraryBrowser* localLibrary READ localLibrary NOTIFY sourcesChanged)
    Q_PROPERTY(int shortcutRevision READ shortcutRevision NOTIFY shortcutsChanged)
    // The M3U8 import or export running, or done and not yet closed.
    Q_PROPERTY(QVariantMap transfer READ transfer NOTIFY transferChanged)

  public:
    using ListTab = bench::Workspace::ListTab;
    using EngineLink = bench::Workspace::EngineLink;

    explicit QuickWorkspace(QObject* parent);
    ~QuickWorkspace() override;

    // QML's singleton is this one, made before the engine loads anything.
    static QuickWorkspace* create(QQmlEngine*, QJSEngine*);
    static void setInstance(QuickWorkspace* instance);

    // Opens the workspace: engines, saved lists, Up Next.
    void start();
    [[nodiscard]] bench::Workspace& workspace() { return workspace_; }

    [[nodiscard]] QAbstractItemModel* tabs() { return &tabs_; }
    [[nodiscard]] int currentTab() const { return current_; }
    void setCurrentTab(int index);
    [[nodiscard]] QAbstractItemModel* listModel() const;
    [[nodiscard]] TrackRowsModel* rows() { return &rows_; }
    [[nodiscard]] bench::ListEditJob* edit() { return &edit_job_; }
    [[nodiscard]] QString lastFmState() const { return lastfm_state_; }
    [[nodiscard]] bench::FolderBrowser* folders() { return &folders_; }
    [[nodiscard]] QVariantList sources() const;
    [[nodiscard]] int currentSource() const { return current_source_; }
    // The library of the source shown; none on Folders.
    [[nodiscard]] bench::LibraryBrowser* library() const;
    // This computer's library, whichever source is shown.
    [[nodiscard]] bench::LibraryBrowser* localLibrary() const;
    [[nodiscard]] int shortcutRevision() const { return shortcut_revision_; }
    [[nodiscard]] QImage libraryCover(const QString& engine, const QString& album_key) const;

    // The Sources panel: a source chosen (remembered when the user chose
    // it); a file or folder opened, into this computer's list; a list's
    // track found in its engine's library; the lists a library can add to.
    Q_INVOKABLE void selectSource(int index, bool chosen);
    Q_INVOKABLE void openFolderEntry(const QModelIndex& index);
    Q_INVOKABLE void locate(bool album);
    Q_INVOKABLE QVariantList libraryListTargets() const;
    Q_INVOKABLE void focusLibrarySearch();
    [[nodiscard]] QVariantMap list() const { return list_; }
    [[nodiscard]] QVariantMap selection() const { return selection_; }
    [[nodiscard]] QVariantMap transport() const { return transport_; }
    [[nodiscard]] QVariantMap modes() const { return modes_; }
    [[nodiscard]] QVariantList outputMenu() const { return output_menu_; }
    [[nodiscard]] QString bufferProfile() const { return workspace_.selected_buffer_profile_; }
    [[nodiscard]] QAbstractItemModel* upNextModel() const;
    [[nodiscard]] QVariantMap upNext() const { return up_next_; }
    [[nodiscard]] QVariantMap history() const { return history_; }
    [[nodiscard]] int coverRevision() const { return cover_revision_; }
    [[nodiscard]] bool notifications() const;
    void setNotifications(bool on);
    [[nodiscard]] bool followPlayback() const { return follow_playback_; }
    void setFollowPlayback(bool on);

    // Lists.
    // A tab's name and state, to ask about it before closing it.
    Q_INVOKABLE QVariantMap tabAt(int index) const;
    Q_INVOKABLE void closeTab(int index);
    Q_INVOKABLE void moveTab(int from, int to);
    // The selection changed in the list on show.
    Q_INVOKABLE void selectionEdited();
    Q_INVOKABLE void activateRow(int row);
    Q_INVOKABLE void removeSelectedRows();
    Q_INVOKABLE void undoListEdit();
    Q_INVOKABLE void redoListEdit();
    Q_INVOKABLE void jumpToPlaying();
    // The selection into another list, or a new one named `name`.
    Q_INVOKABLE void transferSelection(const QString& target_id, bool move);
    Q_INVOKABLE void transferSelectionToNewTab(const QString& name, bool move);
    // The other lists of the list shown, by id and name, for "Copy to list".
    Q_INVOKABLE QVariantList otherLists() const;
    // What the Rate menus offer for the selection: whether each can be
    // used, and the rating all of it shares (-1 for none).
    Q_INVOKABLE QVariantMap ratingState() const;
    Q_INVOKABLE void rateSelection(bool album, int rating);
    Q_INVOKABLE static QString ratingLabel(int rating);
    // Last.fm, for the one track selected: whether it is loved (asked now,
    // told in lastFmState), and loving it or not.
    Q_INVOKABLE QVariantMap lastFmTrack() const;
    Q_INVOKABLE void askLastFm();
    Q_INVOKABLE void loveOnLastFm(bool love);
    // The track view's columns: which, how wide, in what order.
    Q_INVOKABLE QVariantList columns() const;
    Q_INVOKABLE void setColumnWidth(const QString& id, int width);
    Q_INVOKABLE void setPresentation(const QString& presentation);
    Q_INVOKABLE void setColumnVisible(const QString& id, bool visible);
    Q_INVOKABLE void resetLayout();
    Q_INVOKABLE void copyLayoutToAll();
    // The tab shown: made, copied, pinned, saved (a working list under
    // `name`) and renamed.
    Q_INVOKABLE void newList(const QString& name);
    Q_INVOKABLE void duplicateTab();
    Q_INVOKABLE void togglePinned();
    Q_INVOKABLE void saveTab(const QString& name);
    Q_INVOKABLE void renameTab(const QString& name);
    // Files and folders, from a dialog or a drop, into a list of this
    // computer's.
    Q_INVOKABLE void openUrls(const QList<QUrl>& urls);

    // The transport and the modes.
    Q_INVOKABLE void playPause();
    Q_INVOKABLE void stop() { workspace_.stop(); }
    Q_INVOKABLE void next() { workspace_.next(); }
    Q_INVOKABLE void previous() { workspace_.previous(); }
    Q_INVOKABLE void seek(qint64 position_ms) { workspace_.seekToMs(position_ms); }
    Q_INVOKABLE void setVolume(int percent);
    Q_INVOKABLE void toggleMute();
    Q_INVOKABLE void setRepeat(bool on) { workspace_.setRepeat(on); }
    Q_INVOKABLE void setRandom(bool on) { workspace_.setRandom(on); }
    Q_INVOKABLE void setAlbumRandom(bool on) { workspace_.setAlbumRandom(on); }
    Q_INVOKABLE void cycleSingle() { workspace_.cycleSingle(); }
    Q_INVOKABLE void cycleConsume() { workspace_.cycleConsume(); }
    Q_INVOKABLE void setReplayGain(const QString& mode) { workspace_.setReplayGain(mode); }
    Q_INVOKABLE QVariantList replayGainModes() const;
    Q_INVOKABLE void selectOutput(const QString& id);
    // An empty target is the system default.
    Q_INVOKABLE void setOutputDevice(const QVariant& target);
    Q_INVOKABLE void refreshOutputs() { workspace_.refreshOutputs(); }
    Q_INVOKABLE QVariantList bufferProfiles() const;
    Q_INVOKABLE void setBufferProfile(const QString& profile);

    // Up Next.
    Q_INVOKABLE void editUpNext(int operation, int row = -1, int destination = -1);
    Q_INVOKABLE void editUpNextRows(const QVariantList& rows, int operation, int destination = -1);
    Q_INVOKABLE void playUpNextRow(int row);
    Q_INVOKABLE void undoUpNext() { workspace_.undoUpNext(); }
    Q_INVOKABLE void returnToList();
    Q_INVOKABLE void queueSelection(bool next);
    // "Edit tags": the selected rows in a tag editor of their own.
    Q_INVOKABLE void editTags();
    // "ReplayGain…": the selected rows measured and tagged.
    Q_INVOKABLE void replayGain();
    // "Convert files…": the selected rows converted below a folder.
    Q_INVOKABLE void convertFiles();
    // Settings: one window's draft, written on its Save.
    Q_INVOKABLE trackknife::quick::QuickSettings* openSettings();
    // A command's key: the one saved in Settings, or `default_key`.
    Q_INVOKABLE static QString shortcut(const QString& id, const QString& default_key);
    // File: an M3U8 playlist imported into a new list, the list shown
    // exported as one; the transfer cancelled or its bar closed.
    Q_INVOKABLE void importPlaylist(const QUrl& file);
    Q_INVOKABLE void exportPlaylist(const QUrl& file);
    Q_INVOKABLE void dismissTransfer() { transfer_.dismiss(); }
    [[nodiscard]] QVariantMap transfer() const;
    // The workspace database backed up; or restored at the next start.
    Q_INVOKABLE void backupWorkspace(const QUrl& file);
    Q_INVOKABLE static void scheduleWorkspaceRestore(const QUrl& file);
    // Every engine's lists, one to open.
    Q_INVOKABLE trackknife::quick::QuickOpenList* openList();
    // A folder bookmarked, and shown under Folders.
    Q_INVOKABLE void bookmarkFolder(const QUrl& folder);

    // The workspace closing: saved at once, and quitting stops this
    // computer's engine.
    Q_INVOKABLE void closeWindow();
    Q_INVOKABLE void quitAndStopEngine();

    // m:ss, or h:mm:ss, as every time in the window reads.
    Q_INVOKABLE static QString formatTime(qint64 milliseconds);

    [[nodiscard]] QImage cover(const QString& key) const;

    // bench::WorkspaceView
    void workspaceRestored(bool restored) override;
    void showMessage(const QString& text, int timeout_ms) override;
    void listAdded(ListTab& tab, bool select) override;
    void showList(ListTab& tab) override;
    [[nodiscard]] ListTab* currentList() override;
    [[nodiscard]] std::vector<ListTab*> listsInOrder() override;
    [[nodiscard]] ui::TrackViewLayout captureTrackViewLayout(const ListTab& tab) const override;
    [[nodiscard]] QObject* modelParent() override { return this; }
    [[nodiscard]] int currentRow(const ListTab& tab) override {
        return &tab == currentTabPointer() ? rows_.currentRow() : -1;
    }
    void refreshTabChrome(ListTab& tab) override;
    void refreshListHistoryActions() override;
    void refreshSelectionStatus() override;
    void refreshTransport() override;
    void refreshPlaybackCursor(bool jump) override;
    void refreshLocalPlaybackControls() override;
    void refreshPlaybackBufferChecks() override { emit bufferProfileChanged(); }
    void refreshUpNext() override;
    void engineConnected(EngineLink& engine, bool first) override;
    void engineAttached(EngineLink& engine) override;
    void engineRekeyed(EngineLink& engine, const bench::EngineKey& from,
                       bool lists_follow) override;
    void engineRemoving(EngineLink& engine) override;
    void engineRemoved() override;
    void enginesSynced() override;
    void engineListsChanged() override {}
    void engineRatingsChanged(const bench::EngineKey& engine,
                              const QHash<QString, unsigned>& ratings) override;
    void engineInterruptionsChanged(bool reported_now) override;
    void artworkLoaded(const QString& key) override;

  signals:
    void currentTabChanged();
    void listChanged();
    void selectionChanged();
    void transportChanged();
    void modesChanged();
    void outputMenuChanged();
    void bufferProfileChanged();
    void upNextChanged();
    void historyChanged();
    void coverRevisionChanged();
    void desktopChanged();
    // A passing message for the status bar.
    void message(const QString& text, int timeoutMs);
    // The playing row of the list shown, to select and bring into view.
    void playbackCursor(int row, bool jump);
    // Rows the list's undo or redo put back, to select again.
    void rowsRestored(const QVariantList& rows);
    void quitRequested();
    void taggerOpened(trackknife::quick::QuickTagger* tagger);
    void replayGainOpened(trackknife::quick::QuickReplayGain* replayGain);
    void convertOpened(trackknife::quick::QuickConvert* convert);
    // Settings were saved: keys may be others now.
    void shortcutsChanged();
    void transferChanged();
    // Something to be read and acknowledged, in a message box.
    void information(const QString& title, const QString& text);
    void lastFmStateChanged();
    void sourcesChanged();
    // The library's search field wants the keyboard.
    void librarySearchFocused();

  private:
    void buildDesktopServices();
    void refreshList();
    void refreshHistory();
    void refreshOutputControls(const bench::EnginePlayback::State& state);
    [[nodiscard]] ListTab* currentTabPointer() const;

    bench::Workspace workspace_{this};
    ListTabsModel tabs_{workspace_, this};
    int current_{-1};
    // Tabs in the order they were visited, for going back to the one before
    // a closed tab (32 at most).
    std::vector<ListTab*> visited_;
    TrackRowsModel rows_{this};
    bench::ListEditJob edit_job_{this};
    QString lastfm_state_;
    bench::FolderBrowser folders_{this};
    struct Library {
        bench::EngineKey engine;
        bench::LibraryBrowser* browser{nullptr};
    };
    std::vector<Library> libraries_;
    int current_source_{0};
    int shortcut_revision_{0};
    bench::PlaylistTransfer transfer_{this};
    void addLibrary(EngineLink& engine);
    void selectPreferredSource();
    [[nodiscard]] int sourceIndexOf(const bench::EngineKey& engine) const;
    QMetaObject::Connection lastfm_answer_;
    QVariantMap list_;
    QVariantMap selection_;
    QVariantMap transport_;
    QVariantMap output_summary_;
    QVariantMap modes_;
    QVariantList output_menu_;
    QVariantMap up_next_;
    QVariantMap history_;
    int cover_revision_{0};
    int unmuted_volume_{100};
    // The playing row last followed, so it is selected once, not on every
    // refresh.
    const ListTab* followed_tab_{nullptr};
    int followed_row_{-1};
    bool follow_playback_{false};
    QTimer transport_timer_;
    bench::MprisService* mpris_{nullptr};
    bench::DesktopNotifier* notifier_{nullptr};
};

} // namespace trackknife::quick
