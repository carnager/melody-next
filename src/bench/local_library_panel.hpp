// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/engine_key.hpp"

#include "bench/catalogue_source.hpp"
#include "bench/local_list_model.hpp"
#include "trackknife/persistence/local_library.hpp"
#include "workspace/library_browser.hpp"

#include <QPointer>
#include <QWidget>

#include <functional>
#include <memory>

class QCheckBox;
class QDialog;
class QLabel;
class QLineEdit;
class QListWidget;
class QTimer;
class QToolButton;
class QTreeView;

namespace trackknife::bench {

// One engine's library in the widgets window's Sources panel: a
// LibraryBrowser, drawn as a search row, a tree and a footer.
class LocalLibraryPanel final : public QWidget {
    Q_OBJECT
  public:
    // `engine`: whose library this is (ADR-0234).
    explicit LocalLibraryPanel(const CatalogueSource& catalogues,
                               EngineKey engine = EngineKey::local(), QWidget* parent = nullptr);
    ~LocalLibraryPanel() override;
    [[nodiscard]] LibraryBrowser& browser() { return *browser_; }
    void addRoot(std::string raw_path) { browser_->addRoot(std::move(raw_path)); }
    QWidget* createFoldersWidget(QWidget* parent);
    [[nodiscard]] const EngineKey& engine() const noexcept { return browser_->engine(); }
    // The same engine under the key it is known by now (ADR-0234).
    void setEngine(EngineKey engine) { browser_->setEngine(std::move(engine)); }
    // Reload committed index records; filesystem scans require the Refresh button.
    void refreshLibrary() { browser_->refreshLibrary(); }
    // Puts the cursor in the search field, its text selected to type over.
    void focusSearch();
    void stop();
    void resolveEntries(std::vector<persistence::LibraryEntry> entries,
                        std::function<void(std::vector<std::string>)> completion) {
        browser_->resolveEntries(std::move(entries), std::move(completion));
    }
    // The same selection as rows built from the engine's index rather than
    // by reading the files (ADR-0227).
    void resolveEntryRows(std::vector<persistence::LibraryEntry> entries,
                          std::function<void(std::vector<LocalTrackRow>)> completion) {
        browser_->resolveEntryRows(std::move(entries), std::move(completion));
    }
    // ADR-0140: the full result set of the current search as a list.
    void commitSearch() { browser_->commitSearch(); }
    void locatePath(std::string raw_path, bool album) {
        browser_->locatePath(std::move(raw_path), album);
    }
    // ADR-0179: content-identity rating I/O on the library's worker queue.
    void requestRatings(std::vector<std::string> hashes,
                        std::function<void(std::vector<unsigned>)> ready) {
        browser_->requestRatings(std::move(hashes), std::move(ready));
    }
    void storeRating(std::string hash, bool album, unsigned rating) {
        browser_->storeRating(std::move(hash), album, rating);
    }

    // The lists the context menu offers to add to, by id and name, asked for
    // each time it opens: this library's engine's lists open in the window.
    using ListTargets = std::function<std::vector<std::pair<QString, QString>>()>;
    void setListTargets(ListTargets targets) { list_targets_ = std::move(targets); }

  signals:
    void actionRequested(std::vector<persistence::LibraryEntry> entries, LocalLibraryAction action);
    // "Add to list": the entries, appended to the list with this id.
    void addToListRequested(std::vector<persistence::LibraryEntry> entries, const QString& id);
    void searchCommitted(QString query, std::vector<LocalTrackRow> rows);
    void ratingsChanged();
    void libraryContentChanged();
    void manageFoldersRequested();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void showContextMenu(const QPoint& position);
    void showFolders();
    void refreshRoots();
    void refreshScanButton();
    void requestCovers();

    ListTargets list_targets_;
    LibraryBrowser* browser_{nullptr};
    QLabel* source_label_{nullptr};
    QLineEdit* search_{nullptr};
    QCheckBox* query_toggle_{nullptr};
    QLabel* query_error_{nullptr};
    QTreeView* tree_{nullptr};
    QLabel* status_{nullptr};
    QToolButton* newest_toggle_{nullptr};
    QToolButton* scan_button_{nullptr};
    QTimer* artwork_timer_{nullptr};
    QPointer<QDialog> folders_dialog_;
    QPointer<QWidget> folders_widget_;
    QListWidget* roots_list_{nullptr};
    QLabel* roots_error_{nullptr};
};

} // namespace trackknife::bench
