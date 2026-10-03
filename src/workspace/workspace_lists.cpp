// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/workspace.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "trackknife/metadata/flac_mapping.hpp"
#include "uicommon/list_persistence_service.hpp"
#include "uicommon/track_view_layout.hpp"
#include "uicommon/debug_log.hpp"
#include "workspace/workspace_view.hpp"

#include <QFile>
#include <QSettings>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <ranges>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace trackknife::bench {

namespace {

// A stored list's rows, as a tab shows them: what was cached with each is
// taken for what the file says until it is read again.
std::vector<LocalTrackRow> rowsOfItems(const std::span<const persistence::ListItem> items) {
    std::vector<LocalTrackRow> rows;
    rows.reserve(items.size());
    for (const auto& item : items) {
        if (item.source != persistence::ListSource::local) {
            continue;
        }
        LocalTrackRow row;
        row.entry_id = item.entry_id;
        row.raw_path = item.source_reference;
        row.logical_reference = item.logical_reference;
        if (item.source_selection) {
            row.selection = formats::AudioSourceSelection{
                .stream_index = item.source_selection->audio_stream_index,
                .subsong_index = item.source_selection->subsong_index,
            };
        }
        if (item.segment) {
            row.segment = formats::SampleRange{.start_sample = item.segment->start_sample,
                                               .end_sample = item.segment->end_sample};
        }
        row.duration_ms = item.duration_ms;
        row.source_revision = item.source_revision;
        for (const auto& field : item.fields) {
            const auto canonical_name =
                field.name.empty()
                    ? metadata::resolve_text_property_identity(field.native_name).canonical_name
                    : field.name;
            if (!canonical_name.empty()) {
                row.metadata.fields.push_back(metadata::MetadataField{
                    .canonical_name = canonical_name,
                    .native_name = field.native_name.empty() ? field.name : field.native_name,
                    .values = {field.value},
                    .qualifier =
                        metadata::FieldQualifier{
                            .language = field.language,
                            .description = field.description,
                        },
                    .provenance = field.provenance,
                });
            }
        }
        remove_shadowed_probed_metadata(row.metadata);
        project_display_metadata(row);
        row.probed = row.selection.stream_index.has_value() ||
                     row.selection.subsong_index.has_value() || row.segment.has_value() ||
                     row.duration_ms.has_value() || !item.fields.empty();
        rows.push_back(std::move(row));
    }
    return rows;
}

// A big list in parts, on a pool as wide as the machine: each row's tags are
// rebuilt and its content identity hashed, and 66,841 rows took 2.8 s on one
// thread -- on the window's own, at every start.
std::vector<LocalTrackRow> rowsOfDocument(const persistence::ListDocument& document) {
    constexpr std::size_t part = 2'048U;
    const std::span<const persistence::ListItem> items{document.items};
    if (items.size() <= part) {
        return rowsOfItems(items);
    }
    std::vector<std::vector<LocalTrackRow>> parts((items.size() + part - 1U) / part);
    {
        QThreadPool pool;
        pool.setMaxThreadCount(std::max(1, QThread::idealThreadCount()));
        for (std::size_t index = 0; index < parts.size(); ++index) {
            pool.start([&parts, items, index, part] {
                const auto first = index * part;
                parts[index] = rowsOfItems(items.subspan(first, std::min(part, items.size() - first)));
            });
        }
        pool.waitForDone();
    }
    std::vector<LocalTrackRow> rows;
    rows.reserve(items.size());
    for (auto& rows_of_part : parts) {
        std::ranges::move(rows_of_part, std::back_inserter(rows));
    }
    return rows;
}

} // namespace

std::vector<Workspace::PreparedList>
Workspace::prepareLists(std::vector<persistence::ListDocument> documents) {
    std::vector<PreparedList> prepared;
    prepared.reserve(documents.size());
    for (auto& document : documents) {
        auto rows = rowsOfDocument(document);
        // The rows are the list now; the stored items would be a second copy.
        document.items = {};
        prepared.push_back({.document = std::move(document), .rows = std::move(rows)});
    }
    return prepared;
}


Workspace::ListTab* Workspace::addList(persistence::ListDocument document, const bool select) {
    auto rows = rowsOfDocument(document);
    document.items = {};
    return addList(PreparedList{.document = std::move(document), .rows = std::move(rows)}, select);
}

Workspace::ListTab* Workspace::addList(PreparedList prepared, const bool select) {
    auto* model = new LocalListModel(view_->modelParent());
    model->replaceRows(std::move(prepared.rows));
    model->setListeningHistoryService(persistence_);
    auto tab = std::make_unique<ListTab>();
    // Without its items: nothing reads them once the rows are made, and on a
    // big list they were the window's second copy of every tag.
    tab->document = std::move(prepared.document);
    tab->document.items = {};
    tab->model = model;
    auto* raw_tab = tab.get();
    list_tabs_.push_back(std::move(tab));
    view_->listAdded(*raw_tab, select);
    enqueueUnprobedRows(*raw_tab);
    syncArtwork(*raw_tab);
    return raw_tab;
}

// ADR-0233: another client's version of a list open here. A row that is the
// same entry of the same file keeps what this window already knows of it --
// tags read, cover found -- and only what is new is read again.
void Workspace::adoptEngineList(const persistence::ListDocument& document) {
    auto* tab = tabForDocument(document.id);
    if (tab == nullptr) {
        return;
    }
    std::unordered_map<std::string, const LocalTrackRow*> current;
    for (const auto& row : tab->model->rows()) {
        current.emplace(row.entry_id.to_string(), &row);
    }
    auto rows = rowsOfDocument(document);
    for (auto& row : rows) {
        const auto known = current.find(row.entry_id.to_string());
        if (known != current.end() && known->second->raw_path == row.raw_path &&
            known->second->segment == row.segment && known->second->selection == row.selection) {
            row = *known->second;
        }
    }
    tab->model->replaceRows(std::move(rows));
    tab->document.name = document.name;
    tab->document.kind = document.kind;
    tab->document.dirty = false;
    view_->refreshTabChrome(*tab);
    enqueueUnprobedRows(*tab);
    syncArtwork(*tab);
    schedulePersist();
}


std::vector<std::pair<QString, QString>>
Workspace::listTargets(const EngineKey& engine) const {
    std::vector<std::pair<QString, QString>> targets;
    for (auto* tab : view_->listsInOrder()) {
        if (EngineKey::of(tab->document) == engine) {
            targets.emplace_back(QString::fromStdString(tab->document.id.to_string()),
                                 displayText(tab->document.name));
        }
    }
    return targets;
}


void Workspace::openEngineList(const EngineKey& key, const QString& id,
                                     std::function<void()> then) {
    // Already open here: shown, not opened twice.
    if (auto* tab = tabForDocument(id); tab != nullptr) {
        view_->showList(*tab);
        if (then) {
            then();
        }
        return;
    }
    auto* engine = playbackOf(key);
    if (engine == nullptr) {
        return;
    }
    engine->request(
        QStringLiteral("list.get"), protocol::Json{{"id", id.toStdString()}, {"describe", true}},
        [this, key, then = std::move(then)](const core::Result<protocol::Json>& answer) {
            if (!answer) {
                view_->showMessage(QStringLiteral("Could not open the list: %1")
                                             .arg(QString::fromStdString(answer.error().message)),
                                         5'000);
                return;
            }
            auto document = EngineListSync::documentFromAnswer(*answer, key);
            if (!document) {
                return;
            }
            if (auto* open = tabForDocument(document->id); open != nullptr) {
                view_->showList(*open);
            } else {
                list_sync_->opened(*answer, key);
                static_cast<void>(addList(std::move(*document), true));
                schedulePersist();
            }
            if (then) {
                then();
            }
        });
}


persistence::ListDocument Workspace::headerOf(const ListTab& tab) {
    // Field by field: the tab's document can still hold the items it was
    // opened with, and copying those would be the cost being avoided.
    return persistence::ListDocument{.id = tab.document.id,
                                     .kind = tab.document.kind,
                                     .name = tab.document.name,
                                     .pinned = tab.document.pinned,
                                     .dirty = tab.document.dirty,
                                     .items = {},
                                     .engine = tab.document.engine};
}

Workspace::ListTab::Saved Workspace::savedStateOf(const ListTab& tab) {
    return ListTab::Saved{.revision = tab.model->contentRevision(),
                          .name = tab.document.name,
                          .kind = tab.document.kind,
                          .pinned = tab.document.pinned,
                          .dirty = tab.document.dirty,
                          .engine = tab.document.engine};
}

persistence::ListDocument Workspace::documentOf(const ListTab& tab) const {
    auto document = headerOf(tab);
    document.items.reserve(tab.model->rows().size());
    for (const auto& row : tab.model->rows()) {
        persistence::ListItem item{
            // Carry the row's identity rather than letting ListItem mint a
            // fresh one, which would reassign every entry on every save.
            .entry_id = row.entry_id,
            .source = persistence::ListSource::local,
            .profile_id = std::nullopt,
            .source_reference = row.raw_path,
            .logical_reference = row.logical_reference,
            .segment = row.segment ? std::optional{persistence::ListItemSegment{
                                         .start_sample = row.segment->start_sample,
                                         .end_sample = row.segment->end_sample,
                                     }}
                                   : std::nullopt,
            .source_selection = row.selection.stream_index || row.selection.subsong_index
                                    ? std::optional{persistence::ListItemSourceSelection{
                                          .audio_stream_index = row.selection.stream_index,
                                          .subsong_index = row.selection.subsong_index,
                                      }}
                                    : std::nullopt,
            .duration_ms = row.duration_ms,
            .source_revision = row.source_revision,
            .fields = {},
        };
        if (!row.metadata.fields.empty()) {
            // This remains a presentation cache, but retaining layers is
            // necessary so a verified embedded refresh cannot erase CUE,
            // chapter, or sidecar projections for the same physical file.
            for (const auto& field : row.metadata.fields) {
                if (field.canonical_name.empty()) {
                    continue;
                }
                for (const auto& value : field.values) {
                    item.fields.push_back({
                        .name = field.canonical_name,
                        .value = value,
                        .native_name = field.native_name,
                        .provenance = field.provenance,
                        .language = field.qualifier.language,
                        .description = field.qualifier.description,
                    });
                }
            }
        } else {
            if (!row.title.empty()) {
                item.fields.push_back({.name = "title", .value = row.title});
            }
            if (!row.artist.empty()) {
                item.fields.push_back({.name = "artist", .value = row.artist});
            }
            if (!row.album.empty()) {
                item.fields.push_back({.name = "album", .value = row.album});
            }
            if (!row.album_artist.empty()) {
                item.fields.push_back({.name = "albumartist", .value = row.album_artist});
            }
            if (!row.date.empty()) {
                item.fields.push_back({.name = "date", .value = row.date});
            }
            if (!row.track_number.empty()) {
                item.fields.push_back({.name = "track", .value = row.track_number});
            }
        }
        document.items.push_back(std::move(item));
    }
    return document;
}

std::vector<persistence::ListDocument> Workspace::collectDocuments() {
    std::vector<persistence::ListDocument> documents;
    const auto shown = view_->listsInOrder();
    documents.reserve(shown.size());
    for (auto* tab : shown) {
        documents.push_back(documentOf(*tab));
    }
    return documents;
}

std::vector<persistence::ListDocumentWrite> Workspace::collectWrites() {
    std::vector<persistence::ListDocumentWrite> writes;
    const auto shown = view_->listsInOrder();
    writes.reserve(shown.size());
    for (auto* tab : shown) {
        // Unchanged since it was last saved: its header only. A save costs
        // what changed, not the size of every list.
        if (tab->saved && *tab->saved == savedStateOf(*tab)) {
            writes.push_back({.document = headerOf(*tab), .items = false});
        } else {
            writes.push_back({.document = documentOf(*tab), .items = true});
        }
    }
    return writes;
}


std::vector<persistence::TrackViewPreset> Workspace::collectTrackViewLayouts() {
    std::vector<persistence::TrackViewPreset> layouts;
    layouts.reserve(list_tabs_.size());
    for (const auto& tab : list_tabs_) {
        const auto id = QString::fromStdString(tab->document.id.to_string());
        const auto bytes = tab->view_layout_persistence_protected
                               ? tab->preserved_view_layout
                               : ui::serializeTrackViewLayout(view_->captureTrackViewLayout(*tab));
        layouts.push_back(persistence::TrackViewPreset{
            .binding = utf8Bytes(QStringLiteral("local:%1").arg(id)),
            .header_state = std::string{bytes.constData(), static_cast<std::size_t>(bytes.size())},
        });
    }
    return layouts;
}


void Workspace::persistNow(const bool wait) {
    if (persistence_ == nullptr) {
        return;
    }
    auto writes = collectWrites();
    // What each list is as it is written, recorded as saved once the write
    // succeeds -- a failed save leaves it to be written again.
    std::vector<std::pair<core::StableId, ListTab::Saved>> written;
    written.reserve(writes.size());
    for (const auto& write : writes) {
        if (const auto* tab = tabForDocument(write.document.id); tab != nullptr) {
            written.emplace_back(write.document.id, savedStateOf(*tab));
        }
    }
    if (list_sync_ != nullptr) {
        list_sync_->update(writes, engineItems());
    }
    const auto settle = [this](const std::vector<std::pair<core::StableId, ListTab::Saved>>& states) {
        for (const auto& [id, state] : states) {
            if (auto* tab = tabForDocument(id); tab != nullptr) {
                tab->saved = state;
            }
        }
    };
    auto view_layouts = collectTrackViewLayouts();
    if (wait) {
        const auto error =
            persistence_->saveWorkspaceAndWait(std::move(writes), std::move(view_layouts));
        if (!error.isEmpty()) {
            view_->showMessage(QStringLiteral("List save failed: %1").arg(error), 5'000);
            return;
        }
        settle(written);
        return;
    }
    persistence_->saveWorkspace(
        std::move(writes), std::move(view_layouts),
        [this, settle, written = std::move(written)](QString error) {
            if (!error.isEmpty()) {
                view_->showMessage(QStringLiteral("List save failed: %1").arg(error), 5'000);
                return;
            }
            settle(written);
        });
}


void Workspace::restoreLists(std::vector<PreparedList> documents) {
    lists_restored_ = true;
    qCDebug(tkDebug) << "restoring" << static_cast<int>(documents.size()) << "list documents";
    for (auto& prepared : documents) {
        const auto& document = prepared.document;
        qCDebug(tkDebug) << "  list" << displayText(document.name) << "kind"
                         << static_cast<int>(document.kind) << "items"
                         << static_cast<int>(prepared.rows.size());
        // Documents of the retired MPD backend name server URIs, not files;
        // opened as local lists they would be rows that cannot play.
        if (document.kind == persistence::ListKind::mpd) {
            continue;
        }
        // As stored: saving it again writes its header, not its items.
        if (auto* tab = addList(std::move(prepared), false); tab != nullptr) {
            tab->saved = savedStateOf(*tab);
        }
    }
    if (list_tabs_.empty()) {
        static_cast<void>(addList(
            persistence::ListDocument{
                .id = core::StableId::random(),
                .kind = persistence::ListKind::scratch,
                .name = untitled_list_name,
                .pinned = false,
                .dirty = false,
                .items = {},
            },
            true));
    } else {
        view_->showList(*list_tabs_.front());
    }
    if (!pending_open_paths_.empty()) {
        auto pending = std::exchange(pending_open_paths_, std::vector<std::string>{});
        openLocalPaths(std::move(pending));
    }
    // ADR-0233: once saved, the restored lists are on their engines too --
    // which, the first time, is moving them there. And moves an engine missed
    // while this window was closed are handed to it.
    schedulePersist();
    flushEngineRelocations();
}


void Workspace::openLocalPaths(std::vector<std::string> raw_paths) {
    if (raw_paths.empty()) {
        return;
    }
    if (!lists_restored_) {
        pending_open_paths_.insert(pending_open_paths_.end(),
                                   std::make_move_iterator(raw_paths.begin()),
                                   std::make_move_iterator(raw_paths.end()));
        return;
    }
    // Files on this computer go into a local list (ADR-0227): the one on
    // screen when it is one, else the first there is, else a new one. Into a
    // remote tab they would be the remote's paths, which they are not.
    auto* tab = view_->currentList();
    if (tab == nullptr || !EngineKey::of(tab->document).isLocal()) {
        const auto local = std::ranges::find_if(list_tabs_, [](const auto& candidate) {
            return EngineKey::of(candidate->document).isLocal();
        });
        tab = local != list_tabs_.end()
                  ? local->get()
                  : addList(persistence::ListDocument{.id = core::StableId::random(),
                                                         .kind = persistence::ListKind::scratch,
                                                         .name = untitled_list_name,
                                                         .pinned = false,
                                                         .dirty = false,
                                                         .items = {},
                                                         .engine = {}},
                               true);
        view_->showList(*tab);
    }
    startDiscovery(std::move(raw_paths), QString::fromStdString(tab->document.id.to_string()), -1);
}


bool Workspace::transferRows(ListTab* source_tab, LocalListModel* source_model,
                             const EngineKey& from, const bool dynamic, std::vector<int> rows,
                             const QString& target_id, const bool move, const int insertion_row) {
    auto* target = tabForDocument(target_id);
    if (target == nullptr) {
        return false;
    }
    if (!source_model || source_tab == target || (!source_tab && !dynamic) || (dynamic && move)) {
        return false;
    }
    const auto into = EngineKey::of(target->document);
    const bool crossing = from != into;
    std::vector<LocalTrackRow> transferred;
    std::vector<int> source_rows;
    transferred.reserve(static_cast<std::size_t>(rows.size()));
    source_rows.reserve(static_cast<std::size_t>(rows.size()));
    for (const auto row_index : rows) {
        if (row_index < 0 || row_index >= static_cast<int>(source_model->rows().size())) {
            continue;
        }
        transferred.push_back(source_model->rows()[static_cast<std::size_t>(row_index)]);
        source_rows.push_back(row_index);
    }
    if (crossing) {
        // Between engines: each row as the target's engine sees the file.
        // A move leaves behind what could not cross.
        std::vector<LocalTrackRow> crossed;
        std::vector<int> crossed_rows;
        for (std::size_t index = 0; index < transferred.size(); ++index) {
            if (crossEnginePath(transferred[index].raw_path, from, into)) {
                crossed_rows.push_back(source_rows[index]);
            }
        }
        crossed = crossEngineRows(std::move(transferred), from, into);
        transferred = std::move(crossed);
        source_rows = std::move(crossed_rows);
    }
    if (transferred.empty()) {
        return false;
    }
    CrossTabMoveEdit coordinated;
    if (move) {
        coordinated.source_id = QString::fromStdString(source_tab->document.id.to_string());
        coordinated.target_id = target_id;
        coordinated.source_before = source_tab->model->rows();
        coordinated.target_before = target->model->rows();
    }
    target->model->appendRows(std::move(transferred), insertion_row, !move);
    enqueueUnprobedRows(*target);
    markTabDirty(*target);
    syncArtwork(*target);
    if (move) {
        source_tab->model->removeRowIndexes(std::move(source_rows), false);
        markTabDirty(*source_tab);
        coordinated.source_after = source_tab->model->rows();
        coordinated.target_after = target->model->rows();
        cross_tab_move_edit_ = std::move(coordinated);
    }
    view_->refreshListHistoryActions();
    return true;
}


bool Workspace::canReplayCrossTabMove(const bool undo) {
    if (!cross_tab_move_edit_ || cross_tab_move_edit_->applied != undo) {
        return false;
    }
    const auto* source = tabForDocument(cross_tab_move_edit_->source_id);
    const auto* target = tabForDocument(cross_tab_move_edit_->target_id);
    if (source == nullptr || target == nullptr) {
        return false;
    }
    const auto* current = view_->currentList();
    if (current != source && current != target) {
        return false;
    }
    return source->model->rows() ==
               (undo ? cross_tab_move_edit_->source_after : cross_tab_move_edit_->source_before) &&
           target->model->rows() ==
               (undo ? cross_tab_move_edit_->target_after : cross_tab_move_edit_->target_before);
}


bool Workspace::replayCrossTabMove(const bool undo) {
    if (!canReplayCrossTabMove(undo)) {
        return false;
    }
    auto* source = tabForDocument(cross_tab_move_edit_->source_id);
    auto* target = tabForDocument(cross_tab_move_edit_->target_id);
    source->model->replaceRows(undo ? cross_tab_move_edit_->source_before
                                    : cross_tab_move_edit_->source_after);
    target->model->replaceRows(undo ? cross_tab_move_edit_->target_before
                                    : cross_tab_move_edit_->target_after);
    cross_tab_move_edit_->applied = !undo;
    for (auto* tab : {source, target}) {
        markTabDirty(*tab);
        enqueueUnprobedRows(*tab);
        syncArtwork(*tab);
    }
    view_->refreshSelectionStatus();
    return true;
}


void Workspace::takeEngineChange(ListTab& tab) {
    tab.document.dirty = true;
    view_->refreshTabChrome(tab);
    schedulePersist();
}


void Workspace::markTabDirty(ListTab& tab) {
    takeEngineChange(tab);
    // An edit to the list that is playing is an edit to the engine's queue.
    // Without this the engine keeps playing the list as it was when play was
    // pressed, and a track removed here still plays.
    if (tab.document.id == playback_.anchors.document) {
        syncEngineQueue();
    }
}


void Workspace::setActiveLocalList(const QString& id) {
    if (active_local_list_id_ == id)
        return;
    active_local_list_id_ = id;
    for (const auto& tab : list_tabs_)
        view_->refreshTabChrome(*tab);
}


void Workspace::closeList(ListTab& tab) {
    if (playback_.requests.active() && tab.document.id == playback_.anchors.document) {
        if (detached_playback_)
            detached_playback_->model->deleteLater();
        detached_playback_ = tab;
        detached_playback_->view = nullptr;
    } else
        tab.model->deleteLater();
    std::erase_if(list_tabs_,
                  [&tab](const std::unique_ptr<ListTab>& owned) { return owned.get() == &tab; });
    if (list_tabs_.empty()) {
        static_cast<void>(addList(
            persistence::ListDocument{
                .id = core::StableId::random(),
                .kind = persistence::ListKind::scratch,
                .name = untitled_list_name,
                .pinned = false,
                .dirty = false,
                .items = {},
            },
            true));
    }
    schedulePersist();
}

ui::TrackViewLayout Workspace::defaultTrackViewLayout(const ui::TrackViewPresentation presentation) {
    std::vector<ui::TrackViewColumnLayout> columns;
    columns.reserve(track_column_specs.size());
    for (const auto& spec : track_column_specs) {
        auto width = spec.default_width;
        // Ratings stay one click away in the Columns menu rather than
        // claiming space in every default view.
        bool visible = spec.logical < local_rating_column;
        if (presentation == ui::TrackViewPresentation::albums_side_artwork &&
            (spec.logical == local_artist_column || spec.logical == local_album_column ||
             spec.logical == local_date_column)) {
            // The album's header says these; a track whose artist differs
            // says so after its title.
            visible = false;
        } else if (presentation == ui::TrackViewPresentation::albums_header_artwork &&
                   spec.logical == local_artwork_column) {
            width = 42;
        } else if (presentation == ui::TrackViewPresentation::plain_columns &&
                   spec.logical == local_artwork_column) {
            visible = false;
        } else if (presentation == ui::TrackViewPresentation::compact_queue) {
            visible = spec.logical == local_artist_column ||
                      spec.logical == local_track_number_column ||
                      spec.logical == local_title_column || spec.logical == local_album_column ||
                      spec.logical == local_length_column;
        }
        columns.push_back(ui::TrackViewColumnLayout{
            .id = QString::fromLatin1(spec.id), .width = width, .visible = visible});
    }
    return ui::TrackViewLayout{.schema_version = ui::track_view_layout_schema_version,
                               .presentation = presentation,
                               .columns = std::move(columns)};
}

ui::TrackViewLayout Workspace::restoredTrackViewLayout(ListTab& tab) {
    auto layout = defaultTrackViewLayout();
    const auto binding = QStringLiteral("local:%1").arg(document_text(tab.document.id));
    if (const auto stored = restored_track_view_layouts_.value(binding); !stored.isEmpty()) {
        QString layout_error;
        if (auto decoded = ui::deserializeTrackViewLayout(stored, trackColumnIds(), &layout_error);
            decoded) {
            layout = std::move(*decoded);
        } else {
            tab.view_layout_persistence_protected = true;
            tab.preserved_view_layout = stored;
            view_->showMessage(
                QStringLiteral("Track layout was not loaded (%1); the saved value was preserved")
                    .arg(layout_error),
                7'000);
        }
    }
    return layout;
}

void Workspace::removeRows(ListTab& tab, std::vector<int> rows) {
    if (rows.empty()) {
        return;
    }
    tab.model->removeRowIndexes(std::move(rows));
    markTabDirty(tab);
}

bool Workspace::replayListEdit(ListTab* tab, const bool undo) {
    if (tab == nullptr) {
        return false;
    }
    if (!replayCrossTabMove(undo) && !(undo ? tab->model->undo() : tab->model->redo())) {
        return false;
    }
    markTabDirty(*tab);
    enqueueUnprobedRows(*tab);
    syncArtwork(*tab);
    return true;
}

Workspace::HistoryTexts Workspace::historyTexts(const ListTab* tab) {
    const auto* model = tab == nullptr ? nullptr : tab->model;
    const auto cross_tab_undo = canReplayCrossTabMove(true);
    const auto cross_tab_redo = canReplayCrossTabMove(false);
    HistoryTexts texts;
    texts.can_undo = cross_tab_undo || (model != nullptr && model->canUndo());
    texts.can_redo = cross_tab_redo || (model != nullptr && model->canRedo());
    texts.undo = cross_tab_undo                        ? tr("Undo Move tracks between tabs")
                 : model != nullptr && model->canUndo() ? tr("Undo %1").arg(model->undoLabel())
                                                        : tr("Undo list edit");
    texts.redo = cross_tab_redo                        ? tr("Redo Move tracks between tabs")
                 : model != nullptr && model->canRedo() ? tr("Redo %1").arg(model->redoLabel())
                                                        : tr("Redo list edit");
    // Sorting, reversing, shuffling and removing duplicates need two rows.
    texts.editable = model != nullptr && model->rowCount() > 1;
    return texts;
}

Workspace::ListTab* Workspace::createList(const QString& name) {
    if (name.trimmed().isEmpty()) {
        return nullptr;
    }
    auto* tab = addList(
        persistence::ListDocument{
            .id = core::StableId::random(),
            .kind = persistence::ListKind::saved,
            .name = utf8Bytes(name.trimmed()),
            .pinned = false,
            .dirty = false,
            .items = {},
        },
        true);
    schedulePersist();
    return tab;
}

Workspace::ListTab* Workspace::duplicateList(const ListTab& tab) {
    auto duplicate = documentOf(tab);
    duplicate.id = core::StableId::random();
    duplicate.name = utf8Bytes(QStringLiteral("%1 copy").arg(displayText(tab.document.name)));
    duplicate.pinned = false;
    duplicate.dirty = true;
    const auto layout = view_->captureTrackViewLayout(tab);
    auto* duplicated = addList(std::move(duplicate), true);
    schedulePersist();
    if (duplicated != nullptr) {
        duplicated->view_layout = layout;
    }
    return duplicated;
}

void Workspace::togglePinned(ListTab& tab) {
    tab.document.pinned = !tab.document.pinned;
    view_->refreshTabChrome(tab);
    schedulePersist();
}

void Workspace::saveList(ListTab& tab, const QString& name) {
    if (tab.document.kind == persistence::ListKind::scratch) {
        if (name.trimmed().isEmpty()) {
            return;
        }
        tab.document.name = utf8Bytes(name.trimmed());
        tab.document.kind = persistence::ListKind::saved;
    }
    tab.document.dirty = false;
    view_->refreshTabChrome(tab);
    schedulePersist();
}

void Workspace::renameList(ListTab& tab, const QString& name) {
    const auto trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed == displayText(tab.document.name)) {
        return;
    }
    tab.document.name = utf8Bytes(trimmed);
    markTabDirty(tab);
}

bool Workspace::setColumnVisible(ListTab& tab, ui::TrackViewLayout layout, const QString& id,
                                 const bool visible) {
    // The last column shown stays.
    const auto visible_count =
        std::ranges::count(layout.columns, true, &ui::TrackViewColumnLayout::visible);
    const auto found = std::ranges::find(layout.columns, id, &ui::TrackViewColumnLayout::id);
    if (found == layout.columns.end() || (!visible && found->visible && visible_count == 1)) {
        return false;
    }
    found->visible = visible;
    if (visible && (id == QStringLiteral("play-count") || id == QStringLiteral("last-played"))) {
        tab.model->invalidateListeningHistory();
    }
    setTrackViewLayout(tab, std::move(layout));
    return true;
}

void Workspace::setTrackViewLayout(ListTab& tab, ui::TrackViewLayout layout) {
    tab.view_layout = std::move(layout);
    tab.view_layout_persistence_protected = false;
    tab.preserved_view_layout.clear();
    schedulePersist();
}

Workspace::ListTab* Workspace::transferRowsToNewList(ListTab* source_tab,
                                                    LocalListModel* source_model,
                                                    const EngineKey& from, const bool dynamic,
                                                    std::vector<int> rows, const bool move,
                                                    const QString& name) {
    if (source_model == nullptr || (!dynamic && source_tab == nullptr) || (dynamic && move) ||
        rows.empty() || std::ranges::any_of(rows, [source_model](const int row) {
            return row < 0 || row >= source_model->rowCount();
        })) {
        return nullptr;
    }
    // The new list is the same engine's as the rows (ADR-0227).
    auto* destination = addList(persistence::ListDocument{.id = core::StableId::random(),
                                                          .kind = persistence::ListKind::scratch,
                                                          .name = utf8Bytes(name),
                                                          .pinned = false,
                                                          .dirty = false,
                                                          .items = {},
                                                          .engine = from.stored()},
                                false);
    const auto id = document_text(destination->document.id);
    if (!transferRows(source_tab, source_model, from, dynamic, std::move(rows), id, move, -1)) {
        return nullptr;
    }
    return tabForDocument(id);
}

Workspace::ListTab* Workspace::openDynamicResult(const QString& name,
                                                std::vector<LocalTrackRow> rows,
                                                const EngineKey& engine, const bool show) {
    auto* destination = addList(persistence::ListDocument{.id = core::StableId::random(),
                                                          .kind = persistence::ListKind::scratch,
                                                          .name = utf8Bytes(name),
                                                          .pinned = false,
                                                          .dirty = false,
                                                          .items = {},
                                                          .engine = engine.stored()},
                                false);
    destination->view_layout = defaultTrackViewLayout(ui::TrackViewPresentation::plain_columns);
    destination->model->replaceRows(std::move(rows));
    markTabDirty(*destination);
    syncArtwork(*destination);
    schedulePersist();
    if (show) {
        view_->showList(*destination);
    }
    return destination;
}

void Workspace::markPlaying(LocalListModel& model, const QString& playback_context) {
    int occurrence = 0;
    if (playback_context == document_text(playback_.anchors.document)) {
        if (const auto* tab = tabForDocument(playback_.anchors.document);
            tab && tab->model->rowCount() <= 500)
            for (int i = 0; i < playback_.row && i < tab->model->rowCount(); ++i)
                if (tab->model->source(i) == playback_.anchors.source)
                    ++occurrence;
    }
    int hint = -1;
    for (int i = 0; i < model.rowCount(); ++i)
        if (model.source(i) == playback_.anchors.source && occurrence-- == 0) {
            hint = i;
            break;
        }
    model.setCurrentSource(hint >= 0 ? playback_.anchors.source : LocalTrackSource{}, hint);
}

void Workspace::addImportedList(std::vector<LocalTrackRow> rows, const QString& name) {
    auto* tab = addList({.id = core::StableId::random(),
                         .kind = persistence::ListKind::saved,
                         .name = utf8Bytes(name.isEmpty() ? tr("Imported playlist") : name),
                         .pinned = false,
                         .dirty = false,
                         .items = {}},
                        true);
    tab->model->replaceRows(std::move(rows));
    syncArtwork(*tab);
    schedulePersist();
}

void Workspace::backupWorkspace(const QString& path) {
    if (persistence_ == nullptr) {
        return;
    }
    const auto settings_path = path + QStringLiteral(".settings.ini");
    if (QFile::exists(settings_path)) {
        view_->showMessage(
            QStringLiteral("Workspace backup failed: %1 already exists").arg(settings_path), 10'000);
        return;
    }
    const auto temporary_settings = settings_path + QStringLiteral(".partial");
    if (QFile::exists(temporary_settings)) {
        view_->showMessage(
            QStringLiteral("Workspace backup failed: stale temporary settings file exists"), 10'000);
        return;
    }
    QSettings current;
    QSettings settings_backup{temporary_settings, QSettings::IniFormat};
    settings_backup.setValue(QStringLiteral("backup/format"), 1);
    for (const auto& key : current.allKeys()) {
        settings_backup.setValue(QStringLiteral("values/") + key, current.value(key));
    }
    settings_backup.sync();
    if (settings_backup.status() != QSettings::NoError) {
        QFile::remove(temporary_settings);
        view_->showMessage(QStringLiteral("Workspace settings backup failed"), 10'000);
        return;
    }
    const auto encoded = QFile::encodeName(path);
    const auto destination = std::filesystem::path{
        std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())}};
    persistNow(false);
    view_->showMessage(QStringLiteral("Backing up workspace database…"), 0);
    persistence_->backupDatabase(destination, [this, path, settings_path,
                                               temporary_settings](QString error) {
        if (error.isEmpty() && !QFile::rename(temporary_settings, settings_path)) {
            error = QStringLiteral("database saved, but settings could not be published");
        } else if (!error.isEmpty()) {
            QFile::remove(temporary_settings);
        }
        view_->showMessage(
            error.isEmpty()
                ? QStringLiteral("Workspace backed up to %1 and %2").arg(path, settings_path)
                : QStringLiteral("Workspace backup failed: %1").arg(error),
            error.isEmpty() ? 7'000 : 10'000);
    });
}

void Workspace::scheduleWorkspaceRestore(const QString& path) {
    QSettings settings;
    settings.setValue(QStringLiteral("recovery/pending-workspace-restore"), path);
    const auto settings_backup = path + QStringLiteral(".settings.ini");
    settings.setValue(QStringLiteral("recovery/pending-settings-restore"),
                      QFile::exists(settings_backup) ? settings_backup : QString{});
    settings.sync();
}

} // namespace trackknife::bench
