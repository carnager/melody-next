// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/local_list_model.hpp"
#include "trackknife/persistence/local_library.hpp"

#include <QCache>
#include <QFutureWatcher>
#include <QIcon>
#include <QImage>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QSet>
#include <QThreadPool>
#include <QWidget>

#include <deque>
#include <functional>
#include <memory>

class QCheckBox;
class QDialog;
class QLabel;
class QLineEdit;
class QListWidget;
class QStandardItem;
class QStandardItemModel;
class QTimer;
class QToolButton;
class QTreeView;

namespace trackknife::bench {

enum class LocalLibraryAction { append, next, replace, new_list, request_next, request_end };

class LocalLibraryPanel final : public QWidget {
    Q_OBJECT
  public:
    explicit LocalLibraryPanel(std::filesystem::path database_path, QWidget* parent = nullptr);
    ~LocalLibraryPanel() override;
    void addRoot(std::string raw_path);
    QWidget* createFoldersWidget(QWidget* parent);
    // Reload committed index records; filesystem scans require the Refresh button.
    void refreshLibrary();
    void stop();
    void resolveEntries(std::vector<persistence::LibraryEntry> entries,
                        std::function<void(std::vector<std::string>)> completion);
    // ADR-0140: resolves the full result set of the current search text
    // (matching albums' tracks first, then remaining matching tracks,
    // deduplicated by path) and emits searchCommitted. Enter triggers it.
    void commitSearch();
    void locatePath(std::string raw_path, bool album);
    // ADR-0179: content-identity rating I/O on the library's worker queue.
    // ready receives one 0-10 value per requested hash, in order.
    void requestRatings(std::vector<std::string> hashes,
                        std::function<void(std::vector<unsigned>)> ready);
    void storeRating(std::string hash, bool album, unsigned rating);

  signals:
    void actionRequested(std::vector<persistence::LibraryEntry> entries, LocalLibraryAction action);
    void searchCommitted(QString query, std::vector<LocalTrackRow> rows);
    void ratingsChanged();
    void manageFoldersRequested();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    struct Outcome {
        persistence::LibraryPage page;
        std::vector<persistence::LibraryRoot> roots;
        std::vector<std::string> paths;
        std::vector<LocalTrackRow> rows;
        std::vector<unsigned> ratings;
        QString error;
        std::size_t unavailable{0};
    };
    struct Task {
        std::function<Outcome(persistence::LocalLibrary&)> work;
        std::function<void(Outcome)> done;
        bool view_query{false};
    };
    struct ScanOutcome {
        persistence::LibraryScanResult result;
        QString error;
    };

    void enqueue(Task task);
    void pump();
    void reloadTree();
    void loadChildren(const QPersistentModelIndex& parent, persistence::LibraryQuery query);
    void loadFilterChildren(const QPersistentModelIndex& parent,
                            std::shared_ptr<const query::CompiledTkq> compiled);
    void activate(const QModelIndex& index);
    void requestAction(const QModelIndex& index, LocalLibraryAction action);
    void showContextMenu(const QPoint& position);
    void showFolders();
    void loadRoots();
    void startScan();
    void updateProgress();
    void updateArtwork();
    void invalidateArtwork();
    [[nodiscard]] QModelIndexList visibleAlbums() const;

    std::filesystem::path database_path_;
    QThreadPool pool_;
    QFutureWatcher<Outcome> query_watcher_;
    QFutureWatcher<ScanOutcome> scan_watcher_;
    QThreadPool artwork_pool_;
    QFutureWatcher<QImage> artwork_watcher_;
    core::CancellationSource artwork_cancellation_;
    QCache<QByteArray, QIcon> artwork_cache_{256};
    QByteArray artwork_key_;
    std::size_t artwork_generation_{0};
    std::size_t artwork_job_generation_{0};
    bool artwork_running_{false};
    std::deque<Task> tasks_;
    std::function<void(Outcome)> completion_;
    core::CancellationSource lifetime_cancellation_;
    core::CancellationSource view_cancellation_;
    core::CancellationSource scan_cancellation_;
    std::shared_ptr<persistence::LibraryScanProgress> progress_;
    QLineEdit* search_{nullptr};
    QCheckBox* query_toggle_{nullptr};
    QLabel* query_error_{nullptr};
    QTreeView* tree_{nullptr};
    QStandardItemModel* model_{nullptr};
    QLabel* status_{nullptr};
    QToolButton* scan_button_{nullptr};
    QTimer* search_timer_{nullptr};
    QTimer* poll_timer_{nullptr};
    QTimer* change_timer_{nullptr};
    QTimer* artwork_timer_{nullptr};
    QPointer<QDialog> folders_dialog_;
    QPointer<QWidget> folders_widget_;
    QListWidget* roots_list_{nullptr};
    QLabel* roots_error_{nullptr};
    std::size_t generation_{0};
    QSet<QByteArray> expanded_entries_;
    QByteArray current_entry_;
    QString previous_search_;
    std::optional<persistence::LibraryEntry> locate_target_;
    std::string locate_artist_;
    bool querying_{false};
    bool scanning_{false};
    bool stopped_{false};
};

} // namespace trackknife::bench
