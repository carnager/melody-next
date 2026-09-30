// SPDX-License-Identifier: GPL-3.0-only

// Drag and drop (ADR-0233): what a drop on a list, a new list, Up Next or
// a list in the lists pane does, for both windows alike.

#include "workspace/workspace.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "workspace/library_browser.hpp"
#include "workspace/workspace_view.hpp"

#include <QFile>
#include <QFileInfo>
#include <QPersistentModelIndex>
#include <QPointer>

#include <algorithm>
#include <utility>

namespace trackknife::bench {
namespace {

void resolved(const Workspace::Dragged& dragged,
              std::function<void(std::vector<std::string>)> done) {
    if (!dragged.paths.empty()) {
        done(dragged.paths);
    } else if (dragged.resolve) {
        dragged.resolve(std::move(done));
    }
}

} // namespace

void Workspace::addDroppedPaths(const QString& id, const EngineKey& files_engine,
                                std::vector<std::string> paths, const int row) {
    auto* destination = tabForDocument(id);
    if (destination == nullptr) {
        return;
    }
    const auto into = EngineKey::of(destination->document);
    // From another engine's library: as this list's engine sees those files,
    // where it can.
    if (files_engine != into) {
        paths = crossEnginePaths(std::move(paths), files_engine, into);
    }
    if (paths.empty()) {
        return;
    }
    // ADR-0227: a remote's paths are its machine's; looking for them here,
    // as discovery does, would find nothing without a mount.
    if (!into.isLocal()) {
        insertRemotePaths(*destination, std::move(paths), row);
    } else {
        startDiscovery(std::move(paths), id, row);
    }
}

bool Workspace::dropOnList(Dragged dragged, ListTab& target, const int insertion_row,
                           const bool copy) {
    const auto id = document_text(target.document.id);
    if (dragged.isRows()) {
        if (dragged.from_model == target.model && !copy) {
            target.model->reorderRows(std::move(dragged.rows), insertion_row);
            markTabDirty(target);
            return true;
        }
        return transferRows(dragged.from_tab, dragged.from_model, dragged.engine, dragged.dynamic,
                            dragged.rows, id, !copy && !dragged.dynamic, insertion_row);
    }
    if (!dragged.isFiles() || discovery_running_) {
        return false;
    }
    // The row dropped before, kept while the files are found.
    const QPersistentModelIndex anchor{target.model->index(insertion_row, 0)};
    const bool anchored = anchor.isValid();
    const QPointer self{this};
    resolved(dragged, [self, id, insertion_row, anchor, anchored,
                       engine = dragged.engine](std::vector<std::string> paths) {
        if (!self || (anchored && !anchor.isValid())) {
            return;
        }
        self->addDroppedPaths(id, engine, std::move(paths), anchored ? anchor.row() : insertion_row);
    });
    return true;
}

bool Workspace::dropOnNewList(Dragged dragged, const bool copy) {
    if (dragged.isRows()) {
        auto* destination = transferRowsToNewList(dragged.from_tab, dragged.from_model,
                                                  dragged.engine, dragged.dynamic, dragged.rows,
                                                  !copy && !dragged.dynamic, tr("Selection"));
        if (destination != nullptr) {
            view_->showList(*destination);
        }
        return destination != nullptr;
    }
    if (!dragged.isFiles()) {
        return false;
    }
    // Named after the folder or file when it is one.
    auto name = tr("Dropped files");
    if (dragged.paths.size() == 1U) {
        name = QFileInfo{QFile::decodeName(QByteArray::fromStdString(dragged.paths.front()))}
                   .fileName();
    } else if (dragged.paths.empty()) {
        name = tr("Library selection");
    }
    auto* target = addList(persistence::ListDocument{.id = core::StableId::random(),
                                                     .kind = persistence::ListKind::scratch,
                                                     .name = utf8Bytes(name),
                                                     .pinned = false,
                                                     .dirty = false,
                                                     .items = {},
                                                     .engine = dragged.engine.stored()},
                           true);
    schedulePersist();
    view_->showList(*target);
    const auto id = document_text(target->document.id);
    const QPointer self{this};
    resolved(dragged, [self, id, engine = dragged.engine](std::vector<std::string> paths) {
        if (self) {
            self->addDroppedPaths(id, engine, std::move(paths));
        }
    });
    return true;
}

bool Workspace::dropOnUpNext(Dragged dragged, const int position) {
    if (dragged.isRows()) {
        std::ranges::sort(dragged.rows);
        std::vector<LocalTrackRow> tracks;
        for (const auto row : dragged.rows) {
            if (row >= 0 && row < dragged.from_model->rowCount()) {
                tracks.push_back(dragged.from_model->rows()[static_cast<std::size_t>(row)]);
            }
        }
        enqueueLocalRequests(std::move(tracks), position, dragged.engine);
        view_->refreshUpNext();
        return true;
    }
    // A library's albums and tracks; not folders, nor files from elsewhere.
    if (dragged.library == nullptr || dragged.entries.empty()) {
        return false;
    }
    const QPointer self{this};
    dragged.library->resolveEntryRows(
        std::move(dragged.entries),
        [self, position, engine = dragged.engine](std::vector<LocalTrackRow> rows) {
            if (self) {
                self->enqueueLocalRequests(std::move(rows), position, engine);
                self->view_->refreshUpNext();
            }
        });
    return true;
}

bool Workspace::dropOnEngineList(Dragged dragged, const EngineKey& engine, const QString& id) {
    if (auto* tab = tabForDocument(id); tab != nullptr) {
        if (tab->model == dragged.from_model) {
            return false;
        }
        const bool taken = dropOnList(std::move(dragged), *tab, -1, false);
        if (taken) {
            view_->showList(*tab);
        }
        return taken;
    }
    if (!dragged.isRows() && !dragged.isFiles()) {
        return false;
    }
    // Rows of another list are copied: the list they go to is not in sight
    // to see them arrive, so nothing leaves the one that is.
    const QPointer self{this};
    if (dragged.isRows()) {
        const QPointer<LocalListModel> model{dragged.from_model};
        const auto from_id = dragged.from_tab != nullptr
                                 ? document_text(dragged.from_tab->document.id)
                                 : QString{};
        openEngineList(engine, id, [self, model, from_id, dragged, id] {
            if (!self || !model) {
                return;
            }
            static_cast<void>(self->transferRows(
                from_id.isEmpty() ? nullptr : self->tabForDocument(from_id), model,
                dragged.engine, dragged.dynamic, dragged.rows, id, false, -1));
        });
        return true;
    }
    resolved(dragged, [self, engine, id, files = dragged.engine](std::vector<std::string> paths) {
        if (!self) {
            return;
        }
        self->openEngineList(engine, id, [self, id, files, paths = std::move(paths)] {
            if (self) {
                self->addDroppedPaths(id, files, paths);
            }
        });
    });
    return true;
}

} // namespace trackknife::bench
