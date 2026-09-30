// SPDX-License-Identifier: GPL-3.0-only

// What happens to a library's entries when they are asked for: appended to,
// inserted into or replacing a list, a new list, Up Next -- the same for
// every window. This computer's files are read here (discovery); an engine
// elsewhere's are rows from its index (ADR-0227).

#include "workspace/workspace.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "workspace/library_browser.hpp"
#include "workspace/workspace_view.hpp"

#include <QPersistentModelIndex>
#include <QPointer>

#include <algorithm>
#include <ranges>
#include <utility>
#include <vector>

namespace trackknife::bench {

void Workspace::attachLibrary(LibraryBrowser* browser) {
    const QPointer<LibraryBrowser> guard{browser};
    connect(browser, &LibraryBrowser::actionRequested, this,
            [this, guard](std::vector<persistence::LibraryEntry> entries,
                          const LocalLibraryAction action) {
                if (guard) {
                    libraryAction(*guard, std::move(entries), action);
                }
            });
    connect(browser, &LibraryBrowser::addToListRequested, this,
            [this, guard](std::vector<persistence::LibraryEntry> entries, const QString& id) {
                if (guard) {
                    libraryAddToList(*guard, std::move(entries), id);
                }
            });
    connect(browser, &LibraryBrowser::searchCommitted, this,
            [this, guard](const QString& query, std::vector<LocalTrackRow> rows) {
                if (guard) {
                    librarySearchCommitted(*guard, query, std::move(rows));
                }
            });
    // Ratings set in a library are stored on its engine, and read from it.
    connect(browser, &LibraryBrowser::ratingsChanged, this, &Workspace::refreshRatings);
}

// Where "Insert next" puts rows: after the playing row in the list playing,
// else after the list's cursor, else at the top.
int Workspace::insertionForNext(const ListTab& target) {
    if (playback_.anchors.document == target.document.id) {
        return playback_.row + 1;
    }
    const auto current = view_->currentRow(target);
    return current >= 0 ? current + 1 : 0;
}

void Workspace::libraryAction(LibraryBrowser& browser, std::vector<persistence::LibraryEntry> entries,
                              const LocalLibraryAction action) {
    if (entries.empty()) {
        return;
    }
    const auto engine = browser.engine();
    const QPointer<LibraryBrowser> guard{&browser};
    const auto queue = action == LocalLibraryAction::request_next ||
                       action == LocalLibraryAction::request_end;
    const auto position = action == LocalLibraryAction::request_next ? 0 : -1;
    if (engine.isLocal()) {
        if (queue) {
            browser.resolveEntries(
                std::move(entries), [this, position](std::vector<std::string> paths) {
                    std::vector<LocalTrackRow> rows;
                    for (auto& path : paths) {
                        LocalTrackRow row;
                        row.raw_path = std::move(path);
                        row.title = core::display_raw_path(
                            row.raw_path.substr(row.raw_path.find_last_of('/') + 1));
                        rows.push_back(std::move(row));
                    }
                    enqueueLocalRequests(std::move(rows), position);
                    view_->refreshUpNext();
                });
            return;
        }
        // This computer's library goes into a local list: the one on screen,
        // or else the first there is (ADR-0227).
        auto* target = view_->currentList();
        if (target != nullptr && !EngineKey::of(target->document).isLocal()) {
            const auto local = std::ranges::find_if(list_tabs_, [](const auto& tab) {
                return EngineKey::of(tab->document).isLocal();
            });
            target = local != list_tabs_.end() ? local->get() : nullptr;
        }
        if (target == nullptr) {
            return;
        }
        const auto id = document_text(target->document.id);
        const auto name = entries.size() == 1U ? entries.front().label : "Library selection";
        const auto insertion = action == LocalLibraryAction::next ? insertionForNext(*target) : -1;
        const QPersistentModelIndex anchor{target->model->index(insertion, 0)};
        const bool anchored = anchor.isValid();
        browser.resolveEntries(
            std::move(entries), [this, id, name, action, insertion, anchor,
                                 anchored](std::vector<std::string> paths) {
                auto* destination = tabForDocument(id);
                if (destination == nullptr || (anchored && !anchor.isValid())) {
                    return;
                }
                if (discovery_running_) {
                    view_->showMessage(QStringLiteral("A file intake is already running"), 3'000);
                    return;
                }
                if (action == LocalLibraryAction::new_list) {
                    destination = addList(
                        persistence::ListDocument{.id = core::StableId::random(),
                                                  .kind = persistence::ListKind::scratch,
                                                  .name = name,
                                                  .pinned = false,
                                                  .dirty = false,
                                                  .items = {}},
                        true);
                    schedulePersist();
                }
                startDiscovery(std::move(paths), document_text(destination->document.id),
                               anchored ? anchor.row() : insertion,
                               action == LocalLibraryAction::replace);
            });
        return;
    }
    auto* link = this->link(engine);
    if (link == nullptr) {
        return;
    }
    if (queue) {
        browser.resolveEntryRows(std::move(entries),
                                 [this, position, engine](std::vector<LocalTrackRow> rows) {
                                     enqueueLocalRequests(std::move(rows), position, engine);
                                     view_->refreshUpNext();
                                 });
        return;
    }
    // Into the remote list on screen, or the remote's own: a remote file
    // never lands in a local list.
    auto* target = view_->currentList();
    if (target == nullptr || EngineKey::of(target->document).isLocal()) {
        target = engineTab(*link);
    }
    if (target == nullptr) {
        return;
    }
    if (action == LocalLibraryAction::new_list) {
        target = addList(
            persistence::ListDocument{.id = core::StableId::random(),
                                      .kind = persistence::ListKind::scratch,
                                      .name = entries.size() == 1U ? entries.front().label
                                                                   : "Library selection",
                                      .pinned = false,
                                      .dirty = false,
                                      .items = {},
                                      .engine = engine.stored()},
            true);
        schedulePersist();
    }
    const auto insertion = action == LocalLibraryAction::next ? insertionForNext(*target) : -1;
    const auto id = document_text(target->document.id);
    browser.resolveEntryRows(
        std::move(entries), [this, id, action, insertion](std::vector<LocalTrackRow> rows) {
            auto* destination = tabForDocument(id);
            if (destination == nullptr || rows.empty()) {
                return;
            }
            if (action == LocalLibraryAction::replace) {
                destination->model->replaceRows(std::move(rows), true);
            } else {
                destination->model->appendRows(std::move(rows), insertion);
            }
            markTabDirty(*destination);
            syncArtwork(*destination);
            schedulePersist();
            view_->showList(*destination);
            // "Replace list and play", as this computer's library does.
            if (action == LocalLibraryAction::replace && destination->model->rowCount() > 0) {
                playRow(*destination, 0);
            }
        });
}

void Workspace::libraryAddToList(LibraryBrowser& browser,
                                 std::vector<persistence::LibraryEntry> entries, const QString& id) {
    if (tabForDocument(id) == nullptr || entries.empty()) {
        return;
    }
    if (browser.engine().isLocal()) {
        browser.resolveEntries(std::move(entries), [this, id](std::vector<std::string> paths) {
            if (tabForDocument(id) == nullptr) {
                return;
            }
            if (discovery_running_) {
                view_->showMessage(QStringLiteral("A file intake is already running"), 3'000);
                return;
            }
            startDiscovery(std::move(paths), id, -1, false);
        });
        return;
    }
    browser.resolveEntryRows(std::move(entries), [this, id](std::vector<LocalTrackRow> rows) {
        auto* destination = tabForDocument(id);
        if (destination == nullptr || rows.empty()) {
            return;
        }
        const auto count = rows.size();
        destination->model->appendRows(std::move(rows));
        markTabDirty(*destination);
        syncArtwork(*destination);
        schedulePersist();
        view_->showMessage(QStringLiteral("Added %1 to “%2”")
                               .arg(count == 1U ? QStringLiteral("1 track")
                                                : QStringLiteral("%1 tracks").arg(count),
                                    displayText(destination->document.name)),
                           4'000);
    });
}

// ADR-0140: a search kept as an ordinary working list of its engine.
void Workspace::librarySearchCommitted(LibraryBrowser& browser, const QString& query,
                                       std::vector<LocalTrackRow> rows) {
    persistence::ListDocument document{
        .id = core::StableId::random(),
        .kind = persistence::ListKind::scratch,
        .name = utf8Bytes(QStringLiteral("Search: %1").arg(query)),
        .pinned = false,
        .dirty = false,
        .items = {},
    };
    if (!browser.engine().isLocal()) {
        document.engine = browser.engine().stored();
    }
    auto* destination = addList(std::move(document), true);
    destination->model->appendRows(std::move(rows));
    markTabDirty(*destination);
    syncArtwork(*destination);
}

} // namespace trackknife::bench
