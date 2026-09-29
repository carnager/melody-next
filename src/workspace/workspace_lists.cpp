// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/workspace.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "trackknife/metadata/flac_mapping.hpp"
#include "uicommon/list_persistence_service.hpp"
#include "uicommon/debug_log.hpp"
#include "workspace/workspace_view.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

namespace trackknife::bench {

namespace {

// A stored list's rows, as a tab shows them: what was cached with each is
// taken for what the file says until it is read again.
std::vector<LocalTrackRow> rowsOfDocument(const persistence::ListDocument& document) {
    std::vector<LocalTrackRow> rows;
    rows.reserve(document.items.size());
    for (const auto& item : document.items) {
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

} // namespace


Workspace::ListTab* Workspace::addList(persistence::ListDocument document, const bool select) {
    auto* model = new LocalListModel(view_->modelParent());
    model->replaceRows(rowsOfDocument(document));
    model->setListeningHistoryService(persistence_);
    auto tab = std::make_unique<ListTab>();
    tab->document = std::move(document);
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
        QStringLiteral("list.get"), protocol::Json{{"id", id.toStdString()}},
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
                list_sync_->opened(*document, answer->value("revision", std::uint64_t{0}));
                static_cast<void>(addList(std::move(*document), true));
                schedulePersist();
            }
            if (then) {
                then();
            }
        });
}


std::vector<persistence::ListDocument> Workspace::collectDocuments() {
    std::vector<persistence::ListDocument> documents;
    const auto shown = view_->listsInOrder();
    documents.reserve(shown.size());
    for (auto* tab : shown) {
        auto document = tab->document;
        document.items.clear();
        document.items.reserve(tab->model->rows().size());
        for (const auto& row : tab->model->rows()) {
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
        documents.push_back(std::move(document));
    }
    return documents;
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
    auto documents = collectDocuments();
    if (list_sync_ != nullptr) {
        list_sync_->update(documents);
    }
    auto view_layouts = collectTrackViewLayouts();
    if (wait) {
        const auto error =
            persistence_->saveWorkspaceAndWait(std::move(documents), std::move(view_layouts));
        if (!error.isEmpty()) {
            view_->showMessage(QStringLiteral("List save failed: %1").arg(error), 5'000);
        }
        return;
    }
    persistence_->saveWorkspace(
        std::move(documents), std::move(view_layouts), [this](QString error) {
            if (!error.isEmpty()) {
                view_->showMessage(QStringLiteral("List save failed: %1").arg(error), 5'000);
            }
        });
}


void Workspace::restoreLists(std::vector<persistence::ListDocument> documents) {
    lists_restored_ = true;
    qCDebug(tkDebug) << "restoring" << static_cast<int>(documents.size()) << "list documents";
    for (auto& document : documents) {
        qCDebug(tkDebug) << "  list" << displayText(document.name) << "kind"
                         << static_cast<int>(document.kind) << "items"
                         << static_cast<int>(document.items.size());
        // Documents of the retired MPD backend name server URIs, not files;
        // opened as local lists they would be rows that cannot play.
        if (document.kind == persistence::ListKind::mpd) {
            continue;
        }
        static_cast<void>(addList(std::move(document), false));
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
    view_->flushEngineRelocations();
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

} // namespace trackknife::bench
