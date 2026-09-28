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

namespace {

constexpr std::array<std::string_view, 12> default_metadata_fields{
    "Title",        "Artist",      "Album Artist", "Album", "Date",     "Track Number",
    "Total Tracks", "Disc Number", "Total Discs",  "Genre", "Composer", "Comment",
};

[[nodiscard]] persistence::LocalMetadataRefresh
metadata_refresh(const operations::MetadataCommitResult& result) {
    return persistence::LocalMetadataRefresh{
        .operation_id = result.journal_id,
        .source_reference = result.source_raw_path,
        .previous_revision = result.previous_revision,
        .published_revision = result.published_revision,
        .document = result.document,
    };
}

} // namespace

void BenchMainWindow::showConvertDialog() {
    auto* tab = currentListTab();
    showConvertForView(tab ? tab->view : nullptr);
}

namespace {

constexpr auto pending_relocations_key = "lists/pending-relocations";

} // namespace

void BenchMainWindow::queueEngineRelocation(const std::string& from, const std::string& to) {
    if (from.empty() || to.empty() || from == to) {
        return;
    }
    pending_relocations_.push_back(PendingRelocation{.from = from, .to = to, .done = {}});
    storePendingRelocations();
    flushEngineRelocations();
}

void BenchMainWindow::storePendingRelocations() const {
    auto pending = protocol::Json::array();
    for (const auto& move : pending_relocations_) {
        pending.push_back(protocol::Json{{"from", protocol::encode_raw_path(move.from)},
                                         {"to", protocol::encode_raw_path(move.to)},
                                         {"done", [&move] {
                                              auto done = protocol::Json::array();
                                              for (const auto& engine : move.done) {
                                                  done.push_back(engine.toStdString());
                                              }
                                              return done;
                                          }()}});
    }
    QSettings settings;
    if (pending_relocations_.empty()) {
        settings.remove(QLatin1String(pending_relocations_key));
    } else {
        settings.setValue(QLatin1String(pending_relocations_key),
                          QString::fromStdString(pending.dump()));
    }
}

void BenchMainWindow::loadPendingRelocations() {
    const auto stored =
        QSettings{}.value(QLatin1String(pending_relocations_key)).toString().toStdString();
    const auto parsed = protocol::Json::parse(stored, nullptr, false);
    if (!parsed.is_array()) {
        return;
    }
    for (const auto& move : parsed) {
        auto from = protocol::decode_raw_path(move.value("from", std::string{}));
        auto to = protocol::decode_raw_path(move.value("to", std::string{}));
        if (from && to) {
            PendingRelocation pending{.from = std::move(*from), .to = std::move(*to), .done = {}};
            for (const auto& engine : move.value("done", protocol::Json::array())) {
                if (engine.is_string()) {
                    pending.done.insert(QString::fromStdString(engine.get<std::string>()));
                }
            }
            // An older release said only whether this computer's and the
            // remote had it.
            if (move.value("local", false)) {
                pending.done.insert(EngineKey::local().text());
            }
            if (move.value("remote", false)) {
                pending.done.insert(QStringLiteral("*"));
            }
            pending_relocations_.push_back(std::move(pending));
        }
    }
}

void BenchMainWindow::flushEngineRelocations() {
    // What an engine calls a path here: this computer's the same; another's
    // through its mount, or the same when its music is at the same place
    // here. A file outside its mount is not its, and there is nothing to
    // tell it.
    const auto path_for = [this](const EngineLink& engine) {
        return [mount = mountOf(engine), local = engine.key.isLocal()](
                   const std::string& path) -> std::optional<std::string> {
            if (local || mount.local_folder.empty()) {
                return path;
            }
            if (!path_within(path, mount.local_folder)) {
                return std::nullopt;
            }
            return mount.remote_folder + path.substr(mount.local_folder.size());
        };
    };
    for (auto& move : pending_relocations_) {
        for (const auto& engine : engines_) {
            const auto mapped = path_for(*engine);
            if (!engine->key.isLocal() && !mapped(move.from) && !mapped(move.to)) {
                move.done.insert(engine->key.text());
            }
        }
    }
    // Done once every engine this window reaches has it.
    const auto finished = [this](const PendingRelocation& move) {
        return std::ranges::all_of(
            engines_, [&move](const auto& engine) { return move.doneFor(engine->key); });
    };
    std::erase_if(pending_relocations_, finished);
    storePendingRelocations();
    for (const auto& engine : engines_) {
        auto* playback = engine->playback;
        if (playback == nullptr || !playback->active() || engine->relocating) {
            continue;
        }
        const auto mapped = path_for(*engine);
        auto moves = protocol::Json::array();
        std::vector<std::pair<std::string, std::string>> sent;
        for (const auto& move : pending_relocations_) {
            if (move.doneFor(engine->key)) {
                continue;
            }
            const auto from = mapped(move.from);
            const auto to = mapped(move.to);
            if (!from || !to) {
                continue;
            }
            moves.push_back(protocol::Json{{"from", protocol::encode_raw_path(*from)},
                                           {"to", protocol::encode_raw_path(*to)}});
            sent.emplace_back(move.from, move.to);
        }
        if (sent.empty()) {
            continue;
        }
        engine->relocating = true;
        playback->request(
            QStringLiteral("list.relocate"), protocol::Json{{"moves", std::move(moves)}},
            [this, key = engine->key, sent, finished](const core::Result<protocol::Json>& answer) {
                if (auto* relocated = link(key); relocated != nullptr) {
                    relocated->relocating = false;
                }
                // An engine that predates lists has nothing to follow: done.
                if (!answer && answer.error().code != core::ErrorCode::unsupported) {
                    return;
                }
                for (auto& move : pending_relocations_) {
                    if (std::ranges::find(sent, std::pair{move.from, move.to}) != sent.end()) {
                        move.done.insert(key.text());
                    }
                }
                std::erase_if(pending_relocations_, finished);
                storePendingRelocations();
                flushEngineRelocations();
            });
    }
}

void BenchMainWindow::showConvertForView(QTableView* view) {
    auto* model = view ? qobject_cast<LocalListModel*>(view->model()) : nullptr;
    if (!model || !view->selectionModel()) {
        return;
    }
    const auto engine = engineOfView(view);
    if (const auto* engine_link = link(engine); !engine.isLocal() && engine_link != nullptr) {
        // A remote tab's files are converted where this computer has them --
        // through the mount -- and otherwise fetched from their engine first
        // (ADR-0237 stage 6), as the phone downloads them.
        const auto work = engine_link->does_file_work ? engine_link->file_work : nullptr;
        const auto mount = mountOf(*engine_link);
        auto selected = view->selectionModel()->selectedRows();
        std::ranges::sort(selected, {}, &QModelIndex::row);
        std::vector<ConvertDialogItem> items;
        std::size_t unreachable = 0U;
        for (const auto& index : selected) {
            const auto position = static_cast<std::size_t>(index.row());
            if (position >= model->rows().size()) {
                continue;
            }
            const auto& row = model->rows()[position];
            auto label = displayText(row.title.empty() ? row.raw_path : row.title);
            if (!row.artist.empty()) {
                label = QStringLiteral("%1 — %2").arg(displayText(row.artist), label);
            }
            ConvertDialogItem item{.raw_path = row.raw_path,
                                   .selection = row.selection,
                                   .segment = row.segment,
                                   .source_revision = row.source_revision,
                                   .metadata = row.metadata,
                                   .label = std::move(label),
                                   .fetch = {}};
            if (auto here = mount.to_local(row.raw_path)) {
                item.raw_path = std::move(*here);
                // What was known of it came from the remote; read afresh here.
                item.source_revision.reset();
            } else if (work) {
                item.fetch = [work,
                              remote = row.raw_path](const std::filesystem::path& to,
                                                     const core::CancellationToken& cancellation) {
                    return work->download_original(remote, to, cancellation);
                };
            } else {
                ++unreachable;
                continue;
            }
            items.push_back(std::move(item));
        }
        if (unreachable > 0U) {
            statusBar()->showMessage(
                QStringLiteral("%1 of %2 tracks are not reachable on this computer and were left "
                               "out. Where that engine's music is reachable here is set in "
                               "Settings → Engine.")
                    .arg(unreachable)
                    .arg(selected.size()),
                10'000);
        }
        if (!items.empty()) {
            openConvertItems(std::move(items));
        }
        return;
    }
    auto selected = view->selectionModel()->selectedRows();
    std::ranges::sort(selected, {}, &QModelIndex::row);
    if (selected.empty()) {
        return;
    }
    std::vector<ConvertDialogItem> items;
    items.reserve(static_cast<std::size_t>(selected.size()));
    for (const auto& index : selected) {
        const auto row_index = index.row();
        if (row_index < 0 || row_index >= static_cast<int>(model->rows().size())) {
            continue;
        }
        const auto& row = model->rows()[static_cast<std::size_t>(row_index)];
        auto label = model->index(row_index, local_title_column).data().toString();
        if (!row.artist.empty()) {
            label = QStringLiteral("%1 — %2").arg(displayText(row.artist), label);
        }
        items.push_back(ConvertDialogItem{
            .raw_path = row.raw_path,
            .selection = row.selection,
            .segment = row.segment,
            .source_revision = row.source_revision,
            .metadata = row.metadata,
            .label = std::move(label),
            .fetch = {},
        });
    }
    if (items.empty()) {
        return;
    }
    openConvertItems(std::move(items));
}

void BenchMainWindow::openConvertItems(std::vector<ConvertDialogItem> items) {
    auto* const persistence_service = persistence_;
    auto* dialog = new ConvertDialog(
        std::move(items),
        [persistence_service](
            std::function<void(std::vector<persistence::SavedOutputLayoutProfile>,
                               std::vector<persistence::SavedDestinationProfile>, QString)>
                completion) {
            if (persistence_service == nullptr) {
                completion({}, {}, QStringLiteral("Trackknife persistence is unavailable"));
                return;
            }
            persistence_service->loadOutputProfiles(std::move(completion));
        },
        ConvertPresetStore{
            .load =
                [persistence_service](ConvertPresetStore::LoadCompletion completion) {
                    if (persistence_service == nullptr) {
                        completion({}, QStringLiteral("Trackknife persistence is unavailable"));
                        return;
                    }
                    persistence_service->loadEncoderPresets(std::move(completion));
                },
            .save =
                [persistence_service](persistence::SavedEncoderPreset preset,
                                      ConvertPresetStore::Completion completion) {
                    if (persistence_service == nullptr) {
                        completion(QStringLiteral("Trackknife persistence is unavailable"));
                        return;
                    }
                    persistence_service->saveEncoderPreset(std::move(preset),
                                                           std::move(completion));
                },
            .remove =
                [persistence_service](core::StableId id,
                                      ConvertPresetStore::Completion completion) {
                    if (persistence_service == nullptr) {
                        completion(QStringLiteral("Trackknife persistence is unavailable"));
                        return;
                    }
                    persistence_service->removeEncoderPreset(id, std::move(completion));
                },
        },
        this);
    connect(dialog, &ConvertDialog::filesConverted, this, [this] {
        if (localLibrary() != nullptr) {
            localLibrary()->refreshLibrary();
        }
    });
    dialog->show();
}

MetadataPropertiesSourceReader
BenchMainWindow::selectionSourceReader(ListTab& tab, std::vector<QPersistentModelIndex> rows) {
    return selectionSourceReader(tab.model, std::move(rows));
}

MetadataPropertiesSourceReader
BenchMainWindow::selectionSourceReader(LocalListModel* source_model,
                                       std::vector<QPersistentModelIndex> rows,
                                       std::optional<std::vector<LocalTrackRow>> snapshot) {
    const QPointer model{source_model};
    std::shared_ptr<const std::vector<LocalTrackRow>> frozen;
    if (snapshot) {
        // Given, not read from the list: a remote tab's rows as this computer
        // sees their files.
        frozen = std::make_shared<const std::vector<LocalTrackRow>>(std::move(*snapshot));
    } else if (source_model && source_model->property("definition-owned").toBool()) {
        auto rows_now = std::make_shared<std::vector<LocalTrackRow>>();
        for (const auto& index : rows) {
            if (!index.isValid())
                return {};
            rows_now->push_back(source_model->rows().at(static_cast<std::size_t>(index.row())));
        }
        frozen = std::move(rows_now);
    }
    auto selected_rows = std::move(rows);
    return [model, frozen, selected_rows = std::move(selected_rows)](
               const std::size_t selected_index) -> std::optional<MetadataPropertiesSource> {
        if (selected_index >= (frozen ? frozen->size() : selected_rows.size()) ||
            (!frozen && (model == nullptr || !selected_rows[selected_index].isValid()))) {
            return std::nullopt;
        }
        const auto row_index =
            frozen ? static_cast<int>(selected_index) : selected_rows[selected_index].row();
        if (!frozen && (row_index < 0 || row_index >= static_cast<int>(model->rows().size()))) {
            return std::nullopt;
        }
        const auto& row =
            frozen ? (*frozen)[selected_index] : model->rows()[static_cast<std::size_t>(row_index)];
        auto label = frozen ? displayText(row.title.empty() ? row.raw_path : row.title)
                            : model->index(row_index, local_title_column).data().toString();
        if (!row.artist.empty()) {
            label = QStringLiteral("%1 — %2").arg(displayText(row.artist), label);
        }
        // ADR-0139: CUE-bound occurrences capture their sheet identity
        // and revision so ReplayGain drafts can resolve to a sheet
        // rewrite instead of blocked whole-file tags.
        auto cue_binding = [&row]() -> std::optional<metadata::StagedCueSheetBinding> {
            if (!row.logical_reference) {
                return std::nullopt;
            }
            auto parts = parse_cue_logical_reference(*row.logical_reference);
            if (!parts) {
                return std::nullopt;
            }
            auto revision = core::observe_local_source_revision(parts->raw_cue_path);
            return metadata::StagedCueSheetBinding{
                .raw_cue_path = std::move(parts->raw_cue_path),
                .cue_revision = revision ? std::optional{*revision} : std::nullopt,
                .file_index = parts->file_index,
                .track_index = parts->track_index,
            };
        }();
        const auto logical = row.logical_reference.has_value() || row.segment ||
                             row.selection.stream_index || row.selection.subsong_index;
        // ADR-0141: non-CUE logical occurrences carry their in-file
        // identity so loudness drafts can resolve to the sidecar.
        auto logical_identity =
            logical ? std::optional{metadata::StagedLogicalIdentity{
                          .stream_index = row.selection.stream_index,
                          .subsong_index = row.selection.subsong_index,
                          .start_sample =
                              row.segment ? std::optional{row.segment->start_sample} : std::nullopt,
                          .end_sample = row.segment ? row.segment->end_sample : std::nullopt,
                      }}
                    : std::nullopt;
        return MetadataPropertiesSource{
            .source =
                metadata::StagedMetadataSource{
                    .raw_path = row.raw_path,
                    .source_revision = row.source_revision,
                    .baseline = row.metadata,
                    .logical_track = logical,
                    .cue_sheet = std::move(cue_binding),
                    .logical_identity = logical_identity,
                    .needs_metadata_capture = !row.source_revision && row.probed && !logical,
                },
            .track_label = std::move(label),
            .audio = {.selection = row.selection, .range = row.segment},
        };
    };
}

MetadataApplyObserver BenchMainWindow::metadataApplyObserver() {
    return [this](const operations::MetadataApplyResult& result) {
        auto committed = false;
        for (const auto& source : result.sources) {
            if (!source.commit) {
                continue;
            }
            applyCommittedMetadata(*source.commit);
            committed = true;
        }
        for (const auto& sheet : result.cue_sheets) {
            if (!sheet.commit) {
                continue;
            }
            applyCommittedCueReplayGain(*sheet.commit);
            committed = true;
        }
        for (const auto& sidecar : result.sidecars) {
            if (!sidecar.commit) {
                continue;
            }
            applyCommittedLoudnessSidecar(*sidecar.commit);
            committed = true;
        }
        if (committed) {
            schedulePersist();
        }
    };
}

void BenchMainWindow::watchFileWork(EngineLink& link) {
    if (link.playback == nullptr) {
        return;
    }
    const auto probe = [this, &link] {
        if (!link.catalogue || !link.catalogue->endpoint()) {
            return;
        }
        // A fresh connection each time the engine connects: one restarted as
        // a newer engine is asked again, not remembered as it was.
        auto work = std::make_shared<engine::RemoteFileWork>(*link.catalogue->endpoint());
        link.file_work = work;
        link.does_file_work = false;
        const QPointer window{this};
        static_cast<void>(QtConcurrent::run([window, work] {
            const bool does = work->supported();
            std::vector<EngineInterruption> interrupted;
            std::size_t recovered = 0U;
            if (does) {
                // ADR-0237: the engine looks things up with the key kept in
                // Settings; handed over whenever it connects.
                const auto key = QSettings{}
                                     .value(QLatin1String(SettingsDialog::acoustid_client_key))
                                     .toString()
                                     .trimmed();
                if (!key.isEmpty()) {
                    static_cast<void>(work->set_acoustid_key(key.toStdString()));
                }
                // Once chosen in Settings, whether ratings go into the files
                // is the same on every engine this window reaches.
                const QSettings chosen;
                if (chosen.contains(QLatin1String(SettingsDialog::ratings_in_tags_key))) {
                    static_cast<void>(work->set_rating_tags(
                        chosen.value(QLatin1String(SettingsDialog::ratings_in_tags_key)).toBool()));
                }
                if (chosen.contains(QLatin1String(SettingsDialog::rating_tag_scale_key))) {
                    static_cast<void>(work->set_rating_scale(
                        chosen.value(QLatin1String(SettingsDialog::rating_tag_scale_key))
                            .toString()
                            .toStdString()));
                }
                if (auto answer = work->interrupted()) {
                    // What it finished or rolled back at its start, itself.
                    recovered = answer->value("recovered", std::size_t{0U});
                    for (const auto& entry :
                         answer->value("interrupted", protocol::Json::array())) {
                        auto id = core::StableId::parse(entry.value("id", std::string{}));
                        auto path = protocol::decode_raw_path(entry.value("path", std::string{}));
                        if (!id || !path) {
                            continue;
                        }
                        const auto message = entry.find("message");
                        const bool has_message = message != entry.end() && message->is_string();
                        // A move names where it was going, as this window's
                        // own interrupted moves do.
                        std::optional<std::string> target;
                        if (const auto encoded = entry.value("target", std::string{});
                            !encoded.empty()) {
                            if (auto decoded = protocol::decode_raw_path(encoded)) {
                                target = std::move(*decoded);
                            }
                        }
                        QString detail;
                        if (has_message) {
                            detail = QString::fromStdString(message->get<std::string>());
                        } else if (target) {
                            detail = QStringLiteral("An interrupted move could not be finished or "
                                                    "safely rolled back");
                        } else {
                            detail = QStringLiteral("An interrupted tag write could not be "
                                                    "finished or safely rolled back; the file "
                                                    "was left untouched");
                        }
                        if (target) {
                            detail +=
                                QStringLiteral(" · planned target %1")
                                    .arg(QString::fromStdString(core::display_raw_path(*target)));
                        }
                        interrupted.push_back(EngineInterruption{.id = *id,
                                                                 .raw_path = std::move(*path),
                                                                 .detail = std::move(detail),
                                                                 .move = target.has_value()});
                    }
                }
            }
            postBack(
                window, [window, work, does, recovered, interrupted = std::move(interrupted)] {
                    if (!window) {
                        return;
                    }
                    if (recovered > 0U) {
                        window->statusBar()->showMessage(
                            QStringLiteral("Recovered %1 interrupted file operation%2")
                                .arg(recovered)
                                .arg(recovered == 1U ? QString{} : QStringLiteral("s")),
                            5'000);
                    }
                    for (const auto& reported : interrupted) {
                        if (std::ranges::none_of(window->engine_interruptions_,
                                                 [&reported](const auto& known) {
                                                     return known.id == reported.id;
                                                 })) {
                            window->engine_interruptions_.push_back(reported);
                        }
                    }
                    // Observable for tests: what engines could not settle.
                    const auto moves =
                        std::ranges::count_if(window->engine_interruptions_,
                                              [](const auto& known) { return known.move; });
                    window->setProperty("trackknife-file-reconciliation-count",
                                        static_cast<qulonglong>(moves));
                    window->setProperty(
                        "trackknife-metadata-reconciliation-count",
                        static_cast<qulonglong>(window->engine_interruptions_.size()) -
                            static_cast<qulonglong>(moves));
                    if (!interrupted.empty()) {
                        window->presentInterruptedOperations();
                    }
                    // By the connection, not the key: an engine's key changes
                    // when it first says its id.
                    for (const auto& candidate : window->engines_) {
                        if (candidate->file_work == work) {
                            candidate->does_file_work = does;
                        }
                    }
                    if (does) {
                        // ADR-0237: it names files with this window's layouts.
                        window->pushLayouts();
                    }
                });
        }));
    };
    connect(link.playback, &EnginePlayback::connected, this, probe);
    if (link.playback->active()) {
        probe();
    }
}

std::shared_ptr<engine::RemoteFileWork> BenchMainWindow::fileWorkOf(QTableView* view) const {
    const auto* engine_link = link(engineOfView(view));
    if (engine_link == nullptr || !engine_link->does_file_work) {
        return nullptr;
    }
    return engine_link->file_work;
}

ArtworkWritePlanApplierFactory
BenchMainWindow::engineArtworkPlanApplierFactory(std::shared_ptr<engine::RemoteFileWork> work) {
    auto* const persistence_service = persistence_;
    return [this, persistence_service, work = std::move(work)] {
        auto documents = collectDocuments();
        auto view_layouts = collectTrackViewLayouts();
        return ArtworkWritePlanApplier{[persistence_service, work, documents = std::move(documents),
                                        view_layouts = std::move(view_layouts)](
                                           const metadata::ArtworkWritePlan& plan,
                                           const operations::ArtworkApplyProgressCallback& progress,
                                           const core::CancellationToken& cancellation) mutable
                                           -> core::Result<operations::ArtworkApplyResult> {
            if (!persistence_service) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::cancelled,
                    .message = "Trackknife closed during artwork Apply",
                    .context = {},
                });
            }
            const auto persistence_error = persistence_service->saveWorkspaceAndWait(
                std::move(documents), std::move(view_layouts));
            if (!persistence_error.isEmpty()) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = utf8Bytes(persistence_error),
                    .context = {},
                });
            }
            // ADR-0237: the engine writes the pictures and journals them.
            auto applied = work->artwork_apply(plan, progress, cancellation);
            if (!applied) {
                return applied;
            }
            for (const auto& source : applied->sources) {
                if (source.commit) {
                    static_cast<void>(persistence_service->refreshLocalMetadataAndWait(
                        metadata_refresh(*source.commit)));
                }
            }
            return applied;
        }};
    };
}

MetadataWritePlanApplierFactory
BenchMainWindow::engineMetadataPlanApplierFactory(std::shared_ptr<engine::RemoteFileWork> work) {
    auto* const persistence_service = persistence_;
    return [this, persistence_service, work = std::move(work)] {
        auto documents = collectDocuments();
        auto view_layouts = collectTrackViewLayouts();
        return MetadataWritePlanApplier{
            [persistence_service, work, documents = std::move(documents),
             view_layouts =
                 std::move(view_layouts)](const metadata::MetadataWritePlan& plan,
                                          const operations::MetadataApplyProgressCallback& progress,
                                          const core::CancellationToken& cancellation) mutable
                -> core::Result<operations::MetadataApplyResult> {
                if (!persistence_service) {
                    return std::unexpected(core::Error{
                        .code = core::ErrorCode::cancelled,
                        .message = "Trackknife closed during metadata Apply",
                        .context = {},
                    });
                }
                // As before the engine wrote: the lists are saved first, so
                // what they cache of a written file can follow it.
                const auto persistence_error = persistence_service->saveWorkspaceAndWait(
                    std::move(documents), std::move(view_layouts));
                if (!persistence_error.isEmpty()) {
                    return std::unexpected(core::Error{
                        .code = core::ErrorCode::database,
                        .message = utf8Bytes(persistence_error),
                        .context = {},
                    });
                }
                // ADR-0237: the engine writes, journals, and refreshes its
                // library in the same commit.
                auto applied = work->apply(plan, progress, cancellation);
                if (!applied) {
                    return applied;
                }
                // What this window's lists cache of each written file.
                for (const auto& source : applied->sources) {
                    if (source.commit &&
                        source.commit->content_kind ==
                            operations::MetadataOperationContentKind::text_fields) {
                        static_cast<void>(persistence_service->refreshLocalMetadataAndWait(
                            metadata_refresh(*source.commit)));
                    }
                }
                return applied;
            }};
    };
}

const BenchMainWindow::EngineLink*
BenchMainWindow::linkOfWork(const engine::RemoteFileWork* const work) const {
    for (const auto& candidate : engines_) {
        if (candidate->file_work.get() == work) {
            return candidate.get();
        }
    }
    return nullptr;
}

FilePublicationPlanApplierFactory
BenchMainWindow::enginePublicationPlanApplierFactory(std::shared_ptr<engine::RemoteFileWork> work,
                                                     const bool elsewhere, RemoteMount mount,
                                                     std::shared_ptr<MountedMoves> mounted) {
    auto* const persistence_service = persistence_;
    return [this, persistence_service, work = std::move(work), elsewhere, mount = std::move(mount),
            mounted = std::move(mounted)] {
        auto documents = collectDocuments();
        auto view_layouts = collectTrackViewLayouts();
        return FilePublicationPlanApplier{
            [persistence_service, work, elsewhere, mount, mounted, documents = std::move(documents),
             view_layouts = std::move(view_layouts)](
                const operations::PreparationPlan& plan,
                const operations::FilePublicationApplyProgressCallback& progress,
                const core::CancellationToken& cancellation) mutable
                -> core::Result<operations::FilePublicationApplyResult> {
                if (!persistence_service) {
                    return std::unexpected(core::Error{
                        .code = core::ErrorCode::cancelled,
                        .message = "Trackknife closed during file publication",
                        .context = {},
                    });
                }
                // What this computer's lists last saw of each file, for a
                // move elsewhere to be followed here under its own revision.
                std::map<std::string, core::LocalSourceRevision> seen_here;
                if (elsewhere) {
                    for (const auto& document : documents) {
                        for (const auto& item : document.items) {
                            if (item.source == persistence::ListSource::local &&
                                item.source_revision) {
                                seen_here.emplace(item.source_reference, *item.source_revision);
                            }
                        }
                    }
                }
                const auto persistence_error = persistence_service->saveWorkspaceAndWait(
                    std::move(documents), std::move(view_layouts));
                if (!persistence_error.isEmpty()) {
                    return std::unexpected(core::Error{
                        .code = core::ErrorCode::database,
                        .message = utf8Bytes(persistence_error),
                        .context = {},
                    });
                }
                // ADR-0237: the engine moves the files, and its lists, queue
                // and library follow in the same commit.
                auto applied = work->publish(plan, progress, cancellation);
                if (!applied) {
                    return applied;
                }
                const auto relocate =
                    [persistence_service](
                        const operations::FilePublicationCommitResult& done,
                        const std::optional<metadata::MetadataDocument>& published) {
                        return persistence_service->relocateLocalSourceAndWait(
                            persistence::LocalSourceRelocation{
                                .operation_id = done.journal_id,
                                .source_reference = done.source_raw_path,
                                .target_reference = done.target_raw_path,
                                .previous_revision = done.source_revision,
                                .published_revision = done.target_revision,
                                .published_document = published,
                            });
                    };
                mounted->clear();
                for (const auto& source : applied->sources) {
                    if (source.metadata_commit &&
                        source.metadata_commit->content_kind ==
                            operations::MetadataOperationContentKind::text_fields) {
                        static_cast<void>(persistence_service->refreshLocalMetadataAndWait(
                            metadata_refresh(*source.metadata_commit)));
                    }
                    if (!source.commit) {
                        continue;
                    }
                    if (!elsewhere) {
                        // This computer's files: this workspace's lists
                        // follow as they did when Trackknife moved them (a
                        // replay where they share the engine's database).
                        static_cast<void>(relocate(*source.commit, source.published_metadata));
                        continue;
                    }
                    const auto from = mount.local_path_of(source.commit->source_raw_path);
                    const auto to = mount.local_path_of(source.commit->target_raw_path);
                    if (!from || !to) {
                        continue;
                    }
                    auto observed = core::observe_local_source_revision(*to);
                    if (!observed) {
                        continue;
                    }
                    auto here = *source.commit;
                    here.source_raw_path = *from;
                    here.target_raw_path = *to;
                    here.target_revision = *observed;
                    if (const auto seen = seen_here.find(*from); seen != seen_here.end()) {
                        here.source_revision = seen->second;
                    }
                    static_cast<void>(relocate(here, source.published_metadata));
                    mounted->push_back(std::move(here));
                }
                return applied;
            }};
    };
}

void BenchMainWindow::applyEngineRelocation(const EngineKey& engine,
                                            const operations::FilePublicationCommitResult& result,
                                            const operations::FilePublicationCommitResult* here) {
    if (here != nullptr) {
        queueEngineRelocation(here->source_raw_path, here->target_raw_path);
        if (localLibrary() != nullptr) {
            localLibrary()->refreshLibrary();
        }
    }
    for (auto& tab : list_tabs_) {
        const auto* seen = EngineKey::of(tab->document) == engine ? &result : here;
        if (seen == nullptr) {
            continue;
        }
        auto applied =
            tab->model->applyCommittedRelocation(seen->source_raw_path, seen->target_raw_path,
                                                 seen->source_revision, seen->target_revision);
        if (!applied) {
            statusBar()->showMessage(QStringLiteral("File-path view refresh needs attention: %1")
                                         .arg(displayText(applied.error().message)),
                                     8'000);
            continue;
        }
        if (*applied > 0U) {
            syncArtwork(*tab);
        }
    }
    if (here != nullptr) {
        playback_.requests.updateSources([here](LocalTrackRow& row) {
            if (row.raw_path == here->source_raw_path) {
                row.raw_path = here->target_raw_path;
                row.source_revision = here->target_revision;
            }
        });
        persistUpNext();
        refreshUpNext();
        if (playback_.anchors.source.raw_path == here->source_raw_path) {
            playback_.anchors.source.raw_path = here->target_raw_path;
        }
    }
    // Sent again with the new paths, as after a move here.
    engine_queue_.clear();
    engine_requests_.clear();
    syncEngineQueue();
    syncEngineRequests();
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

namespace {

// Blocking engine work off the UI thread, answered on `context`'s.
template <typename Work, typename Done> void offThread(QObject* context, Work work, Done done) {
    const QPointer guard{context};
    static_cast<void>(
        QtConcurrent::run([guard, work = std::move(work), done = std::move(done)]() mutable {
            auto result = work();
            postBack(guard, [done = std::move(done), result = std::move(result)]() mutable {
                done(std::move(result));
            });
        }));
}

[[nodiscard]] QString failure(const core::Result<void>& result) {
    return result ? QString{} : QString::fromStdString(result.error().message);
}

} // namespace

OutputProfileStore BenchMainWindow::buildOutputProfileStore(const EngineKey& destinations_of) {
    auto* const persistence_service = persistence_;
    const auto unavailable = QStringLiteral("Trackknife persistence is unavailable");
    // This computer's destinations are in this workspace, which its engine
    // shares; an engine elsewhere is asked for its own.
    const auto place_of = [this, persistence_service,
                           unavailable](const EngineLink& engine) -> DestinationPlace {
        if (engine.key.isLocal()) {
            return DestinationPlace{
                .key = engine.key.text(),
                .name = engineName(engine.key),
                .load =
                    [persistence_service, unavailable](DestinationPlace::LoadCompletion done) {
                        if (!persistence_service) {
                            done({}, unavailable);
                            return;
                        }
                        persistence_service->loadOutputProfiles(
                            [done = std::move(done)](auto, auto destinations, QString error) {
                                done(std::move(destinations), error);
                            });
                    },
                .save =
                    [persistence_service, unavailable](persistence::SavedDestinationProfile profile,
                                                       DestinationPlace::Completion done) {
                        if (!persistence_service) {
                            done(unavailable);
                            return;
                        }
                        persistence_service->saveDestinationProfile(std::move(profile),
                                                                    std::move(done));
                    },
                .remove =
                    [persistence_service, unavailable](core::StableId id,
                                                       DestinationPlace::Completion done) {
                        if (!persistence_service) {
                            done(unavailable);
                            return;
                        }
                        persistence_service->removeDestinationProfile(id, std::move(done));
                    },
                .folders = {},
                .copyable = {},
            };
        }
        const auto work = engine.file_work;
        const auto mount = mountOf(engine);
        return DestinationPlace{
            .key = engine.key.text(),
            .name = engineName(engine.key),
            .load =
                [this, work](DestinationPlace::LoadCompletion done) {
                    offThread(
                        this, [work] { return work->destinations(); },
                        [done = std::move(done)](auto listed) {
                            if (!listed) {
                                done({}, QString::fromStdString(listed.error().message));
                                return;
                            }
                            done(std::move(*listed), {});
                        });
                },
            .save =
                [this, work](persistence::SavedDestinationProfile profile,
                             DestinationPlace::Completion done) {
                    offThread(
                        this, [work, profile] { return work->save_destination(profile); },
                        [done = std::move(done)](auto saved) { done(failure(saved)); });
                },
            .remove =
                [this, work](core::StableId id, DestinationPlace::Completion done) {
                    offThread(
                        this, [work, id] { return work->remove_destination(id); },
                        [done = std::move(done)](auto removed) { done(failure(removed)); });
                },
            .folders =
                [this, work](std::string path, EngineFolderDialog::ListingCompletion done) {
                    offThread(
                        this, [work, path] { return work->folders(path); },
                        [done = std::move(done)](auto listed) {
                            if (!listed) {
                                done(std::unexpected(std::move(listed.error())));
                                return;
                            }
                            done(EngineFolderDialog::Listing{.path = std::move(listed->path),
                                                             .parent = std::move(listed->parent),
                                                             .folders =
                                                                 std::move(listed->folders)});
                        });
                },
            // Only through a mount: a folder of this computer is that
            // engine's only where the mount says it is.
            .copyable = mount.local_folder.empty() || mount.remote_folder.empty()
                            ? std::function<std::vector<persistence::SavedDestinationProfile>()>{}
                            : [this, mount] {
                                  std::vector<persistence::SavedDestinationProfile> there;
                                  for (auto destination : local_destinations_) {
                                      if (!path_within(destination.profile.root_raw_path,
                                                       mount.local_folder)) {
                                          continue;
                                      }
                                      if (auto remote = mount.remote_path_of(
                                              destination.profile.root_raw_path)) {
                                          destination.profile.root_raw_path = std::move(*remote);
                                          there.push_back(std::move(destination));
                                      }
                                  }
                                  return there;
                              },
        };
    };

    std::vector<DestinationPlace> places;
    for (const auto& engine : engines_) {
        if (engine->key.isLocal() || (engine->does_file_work && engine->file_work)) {
            places.push_back(place_of(*engine));
        }
    }
    const auto* chosen = link(destinations_of);
    auto destinations = chosen != nullptr && (chosen->key.isLocal() ||
                                              (chosen->does_file_work && chosen->file_work))
                            ? place_of(*chosen)
                            : place_of(localEngine());
    const bool elsewhere =
        !destinations.key.isEmpty() && destinations.key != EngineKey::local().text();
    return OutputProfileStore{
        .load =
            [this, persistence_service, unavailable, elsewhere,
             load_destinations = destinations.load](OutputProfileStore::LoadCompletion completion) {
                if (!persistence_service) {
                    completion({}, {}, unavailable);
                    return;
                }
                const QPointer window{this};
                persistence_service->loadOutputProfiles(
                    [window, elsewhere, load_destinations, completion = std::move(completion)](
                        std::vector<persistence::SavedOutputLayoutProfile> layouts,
                        std::vector<persistence::SavedDestinationProfile> here,
                        QString error) mutable {
                        if (window) {
                            window->local_destinations_ = here;
                        }
                        if (!error.isEmpty() || !elsewhere) {
                            completion(std::move(layouts), std::move(here), error);
                            return;
                        }
                        load_destinations(
                            [layouts = std::move(layouts), completion = std::move(completion)](
                                std::vector<persistence::SavedDestinationProfile> there,
                                QString failed) mutable {
                                completion(std::move(layouts), std::move(there), failed);
                            });
                    });
            },
        .save_layout =
            [this, persistence_service, unavailable](persistence::SavedOutputLayoutProfile profile,
                                                     OutputProfileStore::Completion completion) {
                if (!persistence_service) {
                    completion(unavailable);
                    return;
                }
                const QPointer window{this};
                persistence_service->saveOutputLayoutProfile(
                    std::move(profile),
                    [window, completion = std::move(completion)](QString error) {
                        if (window && error.isEmpty()) {
                            window->pushLayouts();
                        }
                        completion(error);
                    });
            },
        .remove_layout =
            [this, persistence_service, unavailable](core::StableId id,
                                                     OutputProfileStore::Completion completion) {
                if (!persistence_service) {
                    completion(unavailable);
                    return;
                }
                const QPointer window{this};
                persistence_service->removeOutputLayoutProfile(
                    id, [window, completion = std::move(completion)](QString error) {
                        if (window && error.isEmpty()) {
                            window->pushLayouts();
                        }
                        completion(error);
                    });
            },
        .save_destination = destinations.save,
        .remove_destination = destinations.remove,
        .destinations_on = elsewhere ? destinations.name : QString{},
        .places = std::move(places),
    };
}

void BenchMainWindow::pushLayouts() {
    if (persistence_ == nullptr) {
        return;
    }
    const QPointer window{this};
    persistence_->loadOutputProfiles(
        [window](std::vector<persistence::SavedOutputLayoutProfile> layouts, auto, QString error) {
            if (!window || !error.isEmpty()) {
                return;
            }
            for (const auto& engine : window->engines_) {
                // This computer's engine keeps its layouts in this workspace:
                // they are these, and a copy sent to it could only be older.
                if (engine->key.isLocal() || !engine->does_file_work || !engine->file_work) {
                    continue;
                }
                // In the order they were made: one thread, so an older set
                // never lands after a newer one.
                static_cast<void>(
                    QtConcurrent::run(&window->layout_pushes_, [work = engine->file_work, layouts] {
                        static_cast<void>(work->set_layouts(layouts));
                    }));
            }
        });
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
    auto* const persistence_service = persistence_;
    // ADR-0237: the engine holding the files does all the work on them. One
    // elsewhere moves files to its own destinations, and its moves are
    // followed here through its mount.
    const auto* work_link = linkOfWork(work.get());
    const bool elsewhere = work_link == nullptr || !work_link->key.isLocal();
    const auto work_engine = work_link != nullptr ? work_link->key : EngineKey::local();
    const auto mount = work_link != nullptr && elsewhere ? mountOf(*work_link) : RemoteMount{};
    auto mounted = std::make_shared<MountedMoves>();
    auto* properties = new MetadataPropertiesDialog(
        selected_row_count, std::move(reader), std::span{default_metadata_fields},
        engineMetadataPlanApplierFactory(work), metadataApplyObserver(),
        MetadataTransformationStore{
            .load =
                [persistence_service](MetadataTransformationStore::LoadCompletion completion) {
                    if (!persistence_service) {
                        completion({}, QStringLiteral("Trackknife persistence is unavailable"));
                        return;
                    }
                    persistence_service->loadMetadataTransformationChains(std::move(completion));
                },
            .save =
                [persistence_service](persistence::SavedMetadataTransformationChain chain,
                                      MetadataTransformationStore::Completion completion) {
                    if (!persistence_service) {
                        completion(QStringLiteral("Trackknife persistence is unavailable"));
                        return;
                    }
                    persistence_service->saveMetadataTransformationChain(std::move(chain),
                                                                         std::move(completion));
                },
            .remove =
                [persistence_service](core::StableId id,
                                      MetadataTransformationStore::Completion completion) {
                    if (!persistence_service) {
                        completion(QStringLiteral("Trackknife persistence is unavailable"));
                        return;
                    }
                    persistence_service->removeMetadataTransformationChain(id,
                                                                           std::move(completion));
                },
        },
        buildOutputProfileStore(work_engine),
        enginePublicationPlanApplierFactory(work, elsewhere, mount, mounted),
        [this, elsewhere, work_engine,
         mounted](const operations::FilePublicationApplyResult& result) {
            auto committed = false;
            for (const auto& source : result.sources) {
                if (source.metadata_commit) {
                    applyCommittedMetadata(*source.metadata_commit);
                    committed = true;
                }
                if (source.commit && elsewhere) {
                    const auto here =
                        std::ranges::find(*mounted, source.commit->journal_id,
                                          &operations::FilePublicationCommitResult::journal_id);
                    applyEngineRelocation(work_engine, *source.commit,
                                          here == mounted->end() ? nullptr : &*here);
                    if (source.published_metadata) {
                        applyCommittedPublicationMetadata(*source.commit,
                                                          *source.published_metadata);
                        if (here != mounted->end()) {
                            applyCommittedPublicationMetadata(*here, *source.published_metadata);
                        }
                    }
                    committed = true;
                } else if (source.commit) {
                    applyCommittedRelocation(*source.commit);
                    if (source.published_metadata) {
                        applyCommittedPublicationMetadata(*source.commit,
                                                          *source.published_metadata);
                    }
                    committed = true;
                }
            }
            if (committed) {
                schedulePersist();
            }
        },
        tabs_,
        MetadataDialogLayoutStore{
            .load =
                [persistence_service](QString key,
                                      MetadataDialogLayoutStore::LoadCompletion completion) {
                    if (!persistence_service) {
                        completion({}, QStringLiteral("Trackknife persistence is unavailable"));
                        return;
                    }
                    persistence_service->loadUiState(std::move(key), std::move(completion));
                },
            .save =
                [persistence_service](QString key, QByteArray value,
                                      MetadataDialogLayoutStore::Completion completion) {
                    if (!persistence_service) {
                        if (completion) {
                            completion(QStringLiteral("Trackknife persistence is unavailable"));
                        }
                        return;
                    }
                    persistence_service->saveUiState(std::move(key), std::move(value),
                                                     std::move(completion));
                },
        },
        engineLookupService(work, this), engineFileWorkTools(work));
    // Observable for tests and diagnostics: which did the work.
    properties->setProperty("trackknife-file-work", QStringLiteral("engine"));
    properties->setArtworkMutationServices(engineArtworkPlanApplierFactory(work),
                                           [this](const operations::ArtworkApplyResult& result) {
                                               auto committed = false;
                                               for (const auto& source : result.sources) {
                                                   if (!source.commit) {
                                                       continue;
                                                   }
                                                   applyCommittedMetadata(*source.commit);
                                                   committed = true;
                                               }
                                               if (committed) {
                                                   schedulePersist();
                                               }
                                           });
    connect(properties, &MetadataPropertiesDialog::statusMessage, this,
            [this](const QString& message) { statusBar()->showMessage(message, 12'000); });
    // Its "Edit…" opens the destinations of the engine its tracks are on.
    connect(properties, &MetadataPropertiesDialog::openSettingsRequested, this,
            [this, work_engine](const SettingsDialog::Page page) {
                auto* settings = showSettingsDialog(page);
                if (settings != nullptr && page == SettingsDialog::Page::naming) {
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

void BenchMainWindow::applyCommittedMetadata(const operations::MetadataCommitResult& result) {
    // The same commit boundary carries text-only and embedded-artwork writes.
    // Cached misses and previous covers must not survive either path.
    invalidateArtwork(result.source_raw_path);
    if (localLibrary() != nullptr) {
        localLibrary()->refreshLibrary();
    }
    for (auto& tab : list_tabs_) {
        auto applied = tab->model->applyCommittedMetadata(result.source_raw_path, result.document,
                                                          result.published_revision);
        if (!applied) {
            statusBar()->showMessage(QStringLiteral("Metadata view refresh needs attention: %1")
                                         .arg(displayText(applied.error().message)),
                                     8'000);
            continue;
        }
        if (*applied > 0U) {
            syncArtwork(*tab);
        }
    }
}

void BenchMainWindow::applyCommittedCueReplayGain(
    const operations::CueReplayGainCommitResult& result) {
    if (localLibrary() != nullptr) {
        localLibrary()->refreshLibrary();
    }
    const auto to_updates = [](const std::vector<operations::CueReplayGainAppliedField>& fields) {
        std::vector<LocalListModel::CueReplayGainFieldUpdate> updates;
        updates.reserve(fields.size());
        for (const auto& field : fields) {
            updates.push_back({.display_name = field.display_name,
                               .canonical_name = field.canonical_name,
                               .value = field.value});
        }
        return updates;
    };
    // Album REMs live in the sheet header and project onto every logical
    // track of the sheet, planned or not.
    std::string sheet_prefix{"cue-v1"};
    sheet_prefix.push_back('\0');
    sheet_prefix += result.raw_cue_path;
    sheet_prefix.push_back('\0');
    const auto album_updates = to_updates(result.album_fields);
    for (auto& tab : list_tabs_) {
        if (!album_updates.empty()) {
            static_cast<void>(tab->model->applyCueReplayGain(sheet_prefix, true, album_updates));
        }
        for (const auto& track : result.tracks) {
            const auto track_updates = to_updates(track.fields);
            if (track_updates.empty()) {
                continue;
            }
            static_cast<void>(tab->model->applyCueReplayGain(
                cue_track_logical_reference(result.raw_cue_path, track.file_index,
                                            track.track_index),
                false, track_updates));
        }
    }
}

void BenchMainWindow::applyCommittedLoudnessSidecar(
    const operations::LoudnessSidecarCommitResult& result) {
    if (localLibrary() != nullptr) {
        localLibrary()->refreshLibrary();
    }
    for (const auto& entry : result.entries) {
        std::vector<LocalListModel::CueReplayGainFieldUpdate> updates;
        updates.reserve(entry.fields.size());
        for (const auto& field : entry.fields) {
            updates.push_back({.display_name = field.display_name,
                               .canonical_name = field.canonical_name,
                               .value = field.value});
        }
        if (updates.empty()) {
            continue;
        }
        const LocalListModel::SidecarRowIdentity identity{
            .stream_index = entry.identity.stream_index,
            .subsong_index = entry.identity.subsong_index,
            .start_sample = entry.identity.start_sample,
            .end_sample = entry.identity.end_sample,
        };
        for (auto& tab : list_tabs_) {
            static_cast<void>(
                tab->model->applySidecarLoudness(result.raw_audio_path, identity, updates));
        }
    }
}

void BenchMainWindow::applyCommittedRelocation(
    const operations::FilePublicationCommitResult& result) {
    queueEngineRelocation(result.source_raw_path, result.target_raw_path);
    if (localLibrary() != nullptr) {
        localLibrary()->refreshLibrary();
    }
    for (auto& tab : list_tabs_) {
        auto applied =
            tab->model->applyCommittedRelocation(result.source_raw_path, result.target_raw_path,
                                                 result.source_revision, result.target_revision);
        if (!applied) {
            statusBar()->showMessage(QStringLiteral("File-path view refresh needs attention: %1")
                                         .arg(displayText(applied.error().message)),
                                     8'000);
            continue;
        }
        if (*applied > 0U) {
            syncArtwork(*tab);
        }
    }
    const auto update_request = [&](LocalTrackRow& row) {
        if (row.raw_path == result.source_raw_path) {
            row.raw_path = result.target_raw_path;
            row.source_revision = result.target_revision;
        }
    };
    playback_.requests.updateSources(update_request);
    persistUpNext();
    refreshUpNext();
    if (playback_.anchors.source.raw_path == result.source_raw_path) {
        playback_.anchors.source.raw_path = result.target_raw_path;
    }
    // The engine's queue names files by path. Identities are unchanged by a
    // move, so the ordinary sync would see nothing new: it is told to send
    // the list again, carrying the new paths.
    engine_queue_.clear();
    engine_requests_.clear();
    syncEngineQueue();
    syncEngineRequests();
}

void BenchMainWindow::applyCommittedPublicationMetadata(
    const operations::FilePublicationCommitResult& result,
    const metadata::MetadataDocument& document) {
    for (auto& tab : list_tabs_) {
        auto applied = tab->model->applyCommittedMetadata(result.target_raw_path, document,
                                                          result.target_revision);
        if (!applied) {
            statusBar()->showMessage(
                QStringLiteral("Published metadata view refresh needs attention: %1")
                    .arg(displayText(applied.error().message)),
                8'000);
            continue;
        }
        if (*applied > 0U) {
            syncArtwork(*tab);
        }
    }
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
