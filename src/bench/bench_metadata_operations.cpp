// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "bench/convert_dialog.hpp"
#include "bench/local_library_panel.hpp"
#include "bench/metadata_properties_dialog.hpp"
#include "bench/musicbrainz_fetch_service.hpp"
#include "bench/preparation_feedback_dialog.hpp"
#include "bench/replaygain_dialog.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/operations/file_publication_apply.hpp"
#include "trackknife/operations/metadata_commit.hpp"
#include "trackknife/persistence/file_publication_journal.hpp"
#include "trackknife/persistence/operation_journal.hpp"
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

struct MetadataOperationJobOutcome {
    std::optional<core::Error> error;
    std::vector<operations::MetadataRecoveryResult> recovery;
    std::vector<operations::MetadataBackupMaintenanceResult> maintenance;
    std::vector<operations::MetadataOperationBackupRecord> backups;
    std::vector<operations::MetadataOperationJournalRecord> reconciliation;
    std::vector<operations::MetadataCommitResult> refreshed_sources;
    std::vector<operations::FilePublicationRecoveryResult> file_recovery;
    std::vector<operations::FilePublicationJournalRecord> file_publications;
    std::vector<operations::FilePublicationCommitResult> relocated_sources;
    std::vector<std::pair<operations::FilePublicationCommitResult, metadata::MetadataDocument>>
        refreshed_relocations;
};

namespace {

// Undo backups exist only as part of the proven atomic commit protocol; there
// is no user-facing undo surface, so retention keeps nothing across restarts.
constexpr operations::MetadataBackupRetentionPolicy metadata_retention_policy{
    .maximum_age_seconds = 0,
    .maximum_entries = 0U,
    .maximum_total_bytes = 0U,
};

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

[[nodiscard]] core::Result<std::optional<metadata::MetadataDocument>>
published_relocation_document(const operations::FilePublicationCommitResult& result,
                              const core::CancellationToken& cancellation = {}) {
    if (result.content != operations::FilePublicationContentKind::prepared_destination_artifact) {
        return std::nullopt;
    }
    auto read = metadata::read_local_metadata(result.target_raw_path, cancellation);
    if (!read) {
        return std::unexpected(std::move(read.error()));
    }
    if (read->source_revision != result.target_revision) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "Published destination artifact changed before metadata reconciliation",
            .context = {},
        });
    }
    return std::move(read->document);
}

[[nodiscard]] std::shared_ptr<MetadataOperationJobOutcome>
run_metadata_operation_job(const std::filesystem::path& database_path,
                           ui::ListPersistenceService* const persistence_service,
                           audio::LocalAuditionService* player_service,
                           const core::CancellationToken& cancellation) {
    auto outcome = std::make_shared<MetadataOperationJobOutcome>();
    auto metadata_opened = persistence::SqliteMetadataOperationJournal::open(database_path);
    if (!metadata_opened) {
        outcome->error = std::move(metadata_opened.error());
        return outcome;
    }
    auto file_opened = persistence::SqliteFilePublicationJournal::open(database_path);
    if (!file_opened) {
        outcome->error = std::move(file_opened.error());
        return outcome;
    }
    auto metadata_journal = std::move(*metadata_opened);
    auto file_journal = std::move(*file_opened);
    const auto remember_error = [outcome](core::Error error) {
        if (!outcome->error) {
            outcome->error = std::move(error);
        }
    };
    const auto metadata_dependent =
        [persistence_service,
         outcome](const operations::MetadataCommitResult& result) -> core::Result<void> {
        if (!persistence_service) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::cancelled,
                .message = "Trackknife closed during metadata recovery",
                .context = {},
            });
        }
        // ADR-0145: recovered carrier publications carry no tag document;
        // the files are the source of truth and re-project on probing.
        if (result.content_kind != operations::MetadataOperationContentKind::text_fields) {
            return {};
        }
        auto refreshed = persistence_service->refreshLocalMetadataAndWait(metadata_refresh(result));
        if (!refreshed) {
            return std::unexpected(std::move(refreshed.error()));
        }
        outcome->refreshed_sources.push_back(result);
        return {};
    };
    const auto file_dependent =
        [persistence_service, player_service, cancellation,
         outcome](const operations::FilePublicationCommitResult& result) -> core::Result<void> {
        if (!persistence_service) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::cancelled,
                .message = "Trackknife closed during source reconciliation",
                .context = {},
            });
        }
        auto published_document = published_relocation_document(result, cancellation);
        if (!published_document) {
            return std::unexpected(std::move(published_document.error()));
        }
        const auto durable = [persistence_service, result,
                              published_document]() -> core::Result<void> {
            if (!persistence_service) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::cancelled,
                    .message = "Trackknife closed during source reconciliation",
                    .context = {},
                });
            }
            auto relocated =
                persistence_service->relocateLocalSourceAndWait(persistence::LocalSourceRelocation{
                    .operation_id = result.journal_id,
                    .source_reference = result.source_raw_path,
                    .target_reference = result.target_raw_path,
                    .previous_revision = result.source_revision,
                    .published_revision = result.target_revision,
                    .published_document = *published_document,
                });
            return relocated ? core::Result<void>{} : std::unexpected(std::move(relocated.error()));
        };
        if (player_service != nullptr) {
            auto relocated = player_service->commit_source_relocation_and_wait(
                audio::LocalAuditionSourceRelocation{
                    .source_raw_path = result.source_raw_path,
                    .target_raw_path = result.target_raw_path,
                    .source_revision = result.source_revision,
                    .target_revision = result.target_revision,
                },
                durable);
            if (!relocated) {
                return std::unexpected(std::move(relocated.error()));
            }
        } else if (auto relocated = durable(); !relocated) {
            return std::unexpected(std::move(relocated.error()));
        }
        outcome->relocated_sources.push_back(result);
        if (*published_document) {
            outcome->refreshed_relocations.emplace_back(result, std::move(**published_document));
        }
        return {};
    };

    auto recovered =
        operations::recover_metadata_operations(metadata_journal, metadata_dependent, cancellation);
    if (!recovered) {
        remember_error(std::move(recovered.error()));
    } else {
        outcome->recovery = std::move(*recovered);
        auto maintained = operations::maintain_metadata_backups(
            metadata_journal, metadata_retention_policy,
            static_cast<std::int64_t>(std::time(nullptr)), cancellation);
        if (!maintained) {
            remember_error(std::move(maintained.error()));
        } else {
            outcome->maintenance = std::move(*maintained);
        }
    }

    auto same = operations::recover_same_filesystem_publications(file_journal, file_dependent,
                                                                 cancellation);
    if (!same) {
        remember_error(std::move(same.error()));
    } else {
        outcome->file_recovery.insert(outcome->file_recovery.end(),
                                      std::make_move_iterator(same->begin()),
                                      std::make_move_iterator(same->end()));
    }
    auto cross = operations::recover_cross_filesystem_publications(file_journal, file_dependent,
                                                                   cancellation);
    if (!cross) {
        remember_error(std::move(cross.error()));
    } else {
        outcome->file_recovery.insert(outcome->file_recovery.end(),
                                      std::make_move_iterator(cross->begin()),
                                      std::make_move_iterator(cross->end()));
    }

    auto backups = metadata_journal.load_backups();
    auto incomplete = metadata_journal.load_incomplete();
    auto file_publications = file_journal.load_recent();
    if (!backups && !outcome->error) {
        outcome->error = std::move(backups.error());
    }
    if (!incomplete && !outcome->error) {
        outcome->error = std::move(incomplete.error());
    }
    if (!file_publications && !outcome->error) {
        outcome->error = std::move(file_publications.error());
    }
    if (backups) {
        outcome->backups = std::move(*backups);
    }
    if (incomplete) {
        for (auto& record : *incomplete) {
            if (record.state == operations::MetadataOperationJournalState::needs_reconciliation) {
                outcome->reconciliation.push_back(std::move(record));
            }
        }
    }
    if (file_publications) {
        outcome->file_publications = std::move(*file_publications);
    }
    return outcome;
}

} // namespace

void BenchMainWindow::showConvertDialog() {
    auto* tab = currentListTab();
    showConvertForView(tab ? tab->view : nullptr);
}

void BenchMainWindow::showConvertForView(QTableView* view) {
    auto* model = view ? qobject_cast<LocalListModel*>(view->model()) : nullptr;
    if (!model || !view->selectionModel()) {
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
        if (local_library_ != nullptr) {
            local_library_->refreshLibrary();
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
                                       std::vector<QPersistentModelIndex> rows) {
    const QPointer model{source_model};
    std::shared_ptr<const std::vector<LocalTrackRow>> frozen;
    if (source_model && source_model->property("definition-owned").toBool()) {
        auto snapshot = std::make_shared<std::vector<LocalTrackRow>>();
        for (const auto& index : rows) {
            if (!index.isValid())
                return {};
            snapshot->push_back(source_model->rows().at(static_cast<std::size_t>(index.row())));
        }
        frozen = std::move(snapshot);
    }
    auto selected_rows = std::move(rows);
    return [model, frozen, selected_rows = std::move(selected_rows)](
               const std::size_t selected_index) -> std::optional<MetadataPropertiesSource> {
        if (selected_index >= selected_rows.size() ||
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

MetadataWritePlanApplierFactory BenchMainWindow::metadataPlanApplierFactory() {
    auto* const persistence_service = persistence_;
    const auto database_path = database_path_;
    return [this, database_path, persistence_service] {
        auto documents = collectDocuments();
        auto view_layouts = collectTrackViewLayouts();
        return MetadataWritePlanApplier{
            [database_path, persistence_service, documents = std::move(documents),
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
                const auto persistence_error = persistence_service->saveWorkspaceAndWait(
                    std::move(documents), std::move(view_layouts));
                if (!persistence_error.isEmpty()) {
                    return std::unexpected(core::Error{
                        .code = core::ErrorCode::database,
                        .message = utf8Bytes(persistence_error),
                        .context = {},
                    });
                }
                auto opened = persistence::SqliteMetadataOperationJournal::open(database_path);
                if (!opened) {
                    return std::unexpected(std::move(opened.error()));
                }
                auto journal = std::move(*opened);
                const auto dependent =
                    [persistence_service](
                        const operations::MetadataCommitResult& result) -> core::Result<void> {
                    if (!persistence_service) {
                        return std::unexpected(core::Error{
                            .code = core::ErrorCode::cancelled,
                            .message = "Trackknife closed during metadata Apply",
                            .context = {},
                        });
                    }
                    // ADR-0145: carrier commits have no tag document;
                    // their rows refresh through the apply observers.
                    if (result.content_kind !=
                        operations::MetadataOperationContentKind::text_fields) {
                        return {};
                    }
                    auto refreshed =
                        persistence_service->refreshLocalMetadataAndWait(metadata_refresh(result));
                    return refreshed ? core::Result<void>{}
                                     : std::unexpected(std::move(refreshed.error()));
                };
                return operations::apply_metadata_write_plan(
                    plan,
                    [&journal, &dependent](const metadata::MetadataWritePlanSource& source,
                                           const core::CancellationToken& source_cancellation) {
                        return operations::commit_flac_metadata_source(source, journal, dependent,
                                                                       source_cancellation);
                    },
                    [&journal, &dependent](const metadata::MetadataWritePlanCueSheet& sheet,
                                           const core::CancellationToken& sheet_cancellation) {
                        return operations::commit_cue_replay_gain_sheet(sheet, journal, dependent,
                                                                        sheet_cancellation);
                    },
                    [&journal, &dependent](const metadata::MetadataWritePlanSidecar& sidecar,
                                           const core::CancellationToken& sidecar_cancellation) {
                        return operations::commit_loudness_sidecar(sidecar, journal, dependent,
                                                                   sidecar_cancellation);
                    },
                    progress, cancellation,
                    operations::MetadataApplyOptions{.maximum_parallelism = 2U});
            }};
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

void BenchMainWindow::showReplayGainDialog() {
    auto* tab = currentListTab();
    showReplayGainForView(tab ? tab->view : nullptr);
}

void BenchMainWindow::showReplayGainForView(QTableView* view) {
    auto* model = view ? qobject_cast<LocalListModel*>(view->model()) : nullptr;
    if (!model || !view->selectionModel()) {
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
                             metadataPlanApplierFactory(), metadataApplyObserver(), this);
    dialog->show();
}

OutputProfileStore BenchMainWindow::buildOutputProfileStore() {
    auto* const persistence_service = persistence_;
    return OutputProfileStore{
        .load =
            [persistence_service](OutputProfileStore::LoadCompletion completion) {
                if (!persistence_service) {
                    completion({}, {}, QStringLiteral("Trackknife persistence is unavailable"));
                    return;
                }
                persistence_service->loadOutputProfiles(std::move(completion));
            },
        .save_layout =
            [persistence_service](persistence::SavedOutputLayoutProfile profile,
                                  OutputProfileStore::Completion completion) {
                if (!persistence_service) {
                    completion(QStringLiteral("Trackknife persistence is unavailable"));
                    return;
                }
                persistence_service->saveOutputLayoutProfile(std::move(profile),
                                                             std::move(completion));
            },
        .remove_layout =
            [persistence_service](core::StableId id, OutputProfileStore::Completion completion) {
                if (!persistence_service) {
                    completion(QStringLiteral("Trackknife persistence is unavailable"));
                    return;
                }
                persistence_service->removeOutputLayoutProfile(id, std::move(completion));
            },
        .save_destination =
            [persistence_service](persistence::SavedDestinationProfile profile,
                                  OutputProfileStore::Completion completion) {
                if (!persistence_service) {
                    completion(QStringLiteral("Trackknife persistence is unavailable"));
                    return;
                }
                persistence_service->saveDestinationProfile(std::move(profile),
                                                            std::move(completion));
            },
        .remove_destination =
            [persistence_service](core::StableId id, OutputProfileStore::Completion completion) {
                if (!persistence_service) {
                    completion(QStringLiteral("Trackknife persistence is unavailable"));
                    return;
                }
                persistence_service->removeDestinationProfile(id, std::move(completion));
            },
    };
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
                           selectionSourceReader(model, std::move(selected_rows)));
}

void BenchMainWindow::openMetadataProperties(const std::size_t selected_row_count,
                                             MetadataPropertiesSourceReader reader) {
    auto* const persistence_service = persistence_;
    const auto database_path = database_path_;
    auto* properties = new MetadataPropertiesDialog(
        selected_row_count, std::move(reader), std::span{default_metadata_fields},
        metadataPlanApplierFactory(), metadataApplyObserver(),
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
        buildOutputProfileStore(),
        [this, database_path, persistence_service] {
            auto documents = collectDocuments();
            auto view_layouts = collectTrackViewLayouts();
            const auto recovery_was_running = metadata_operation_running_;
            return FilePublicationPlanApplier{
                [this, database_path, persistence_service, documents = std::move(documents),
                 view_layouts = std::move(view_layouts), recovery_was_running](
                    const operations::PreparationPlan& plan,
                    const operations::FilePublicationApplyProgressCallback& progress,
                    const core::CancellationToken& cancellation) mutable
                    -> core::Result<operations::FilePublicationApplyResult> {
                    if (recovery_was_running) {
                        return std::unexpected(core::Error{
                            .code = core::ErrorCode::conflict,
                            .message = "Startup operation recovery is still running; preview "
                                       "again after it finishes",
                            .context = {},
                        });
                    }
                    if (!persistence_service) {
                        return std::unexpected(core::Error{
                            .code = core::ErrorCode::cancelled,
                            .message = "Trackknife closed during file publication",
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
                    auto file_opened =
                        persistence::SqliteFilePublicationJournal::open(database_path);
                    if (!file_opened) {
                        return std::unexpected(std::move(file_opened.error()));
                    }
                    auto metadata_opened =
                        persistence::SqliteMetadataOperationJournal::open(database_path);
                    if (!metadata_opened) {
                        return std::unexpected(std::move(metadata_opened.error()));
                    }
                    auto file_journal = std::move(*file_opened);
                    auto metadata_journal = std::move(*metadata_opened);
                    const auto metadata_dependent =
                        [persistence_service](
                            const operations::MetadataCommitResult& result) -> core::Result<void> {
                        if (!persistence_service) {
                            return std::unexpected(core::Error{
                                .code = core::ErrorCode::cancelled,
                                .message = "Trackknife closed during metadata reconciliation",
                                .context = {},
                            });
                        }
                        auto refreshed = persistence_service->refreshLocalMetadataAndWait(
                            metadata_refresh(result));
                        return refreshed ? core::Result<void>{}
                                         : std::unexpected(std::move(refreshed.error()));
                    };
                    const auto relocate =
                        [this, persistence_service](
                            const operations::FilePublicationCommitResult& result,
                            const metadata::MetadataDocument* document) -> core::Result<void> {
                        if (!persistence_service) {
                            return std::unexpected(core::Error{
                                .code = core::ErrorCode::cancelled,
                                .message = "Trackknife closed during source reconciliation",
                                .context = {},
                            });
                        }
                        const auto durable = [persistence_service, result,
                                              document]() -> core::Result<void> {
                            if (!persistence_service) {
                                return std::unexpected(core::Error{
                                    .code = core::ErrorCode::cancelled,
                                    .message = "Trackknife closed during source reconciliation",
                                    .context = {},
                                });
                            }
                            auto relocated = persistence_service->relocateLocalSourceAndWait(
                                persistence::LocalSourceRelocation{
                                    .operation_id = result.journal_id,
                                    .source_reference = result.source_raw_path,
                                    .target_reference = result.target_raw_path,
                                    .previous_revision = result.source_revision,
                                    .published_revision = result.target_revision,
                                    .published_document = document == nullptr
                                                              ? std::nullopt
                                                              : std::optional{*document},
                                });
                            return relocated ? core::Result<void>{}
                                             : std::unexpected(std::move(relocated.error()));
                        };
                        if (player_ == nullptr) {
                            return durable();
                        }
                        auto relocated = player_->commit_source_relocation_and_wait(
                            audio::LocalAuditionSourceRelocation{
                                .source_raw_path = result.source_raw_path,
                                .target_raw_path = result.target_raw_path,
                                .source_revision = result.source_revision,
                                .target_revision = result.target_revision,
                            },
                            durable);
                        return relocated ? core::Result<void>{}
                                         : std::unexpected(std::move(relocated.error()));
                    };
                    const auto file_dependent =
                        [&relocate](const operations::FilePublicationCommitResult& result) {
                            return relocate(result, nullptr);
                        };
                    const auto artifact_dependent =
                        [&relocate](const operations::FilePublicationCommitResult& result,
                                    const metadata::MetadataDocument& document) {
                            return relocate(result, &document);
                        };
                    return operations::apply_preparation_publications(
                        plan, file_journal, metadata_journal, metadata_dependent, file_dependent,
                        artifact_dependent, progress, cancellation,
                        operations::FilePublicationApplyOptions{.maximum_parallelism = 2U});
                }};
        },
        [this](const operations::FilePublicationApplyResult& result) {
            auto committed = false;
            for (const auto& source : result.sources) {
                if (source.metadata_commit) {
                    applyCommittedMetadata(*source.metadata_commit);
                    committed = true;
                }
                if (source.commit) {
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
        musicBrainzLookupService());
    properties->setArtworkMutationServices(
        [this, database_path, persistence_service] {
            auto documents = collectDocuments();
            auto view_layouts = collectTrackViewLayouts();
            return ArtworkWritePlanApplier{
                [database_path, persistence_service, documents = std::move(documents),
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
                    auto opened = persistence::SqliteMetadataOperationJournal::open(database_path);
                    if (!opened) {
                        return std::unexpected(std::move(opened.error()));
                    }
                    auto journal = std::move(*opened);
                    const auto dependent =
                        [persistence_service](
                            const operations::MetadataCommitResult& result) -> core::Result<void> {
                        if (!persistence_service) {
                            return std::unexpected(core::Error{
                                .code = core::ErrorCode::cancelled,
                                .message = "Trackknife closed during artwork reconciliation",
                                .context = {},
                            });
                        }
                        auto refreshed = persistence_service->refreshLocalMetadataAndWait(
                            metadata_refresh(result));
                        return refreshed ? core::Result<void>{}
                                         : std::unexpected(std::move(refreshed.error()));
                    };
                    return operations::apply_artwork_write_plan(
                        plan,
                        [&journal, &dependent](const metadata::ArtworkWritePlanSource& source,
                                               const core::CancellationToken& source_cancellation) {
                            return operations::commit_artwork_source(source, journal, dependent,
                                                                     source_cancellation);
                        },
                        progress, cancellation,
                        operations::ArtworkApplyOptions{.maximum_parallelism = 2U});
                }};
        },
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
    connect(properties, &MetadataPropertiesDialog::openSettingsRequested, this,
            [this](const SettingsDialog::Page page) { showSettingsDialog(page); });
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
                        .name = "Scratch",
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

MusicBrainzLookupService BenchMainWindow::musicBrainzLookupService() {
    if (database_path_.empty()) {
        return {};
    }
    if (musicbrainz_service_ == nullptr) {
        musicbrainz_service_ = new MusicBrainzFetchService(database_path_, this);
    }
    return musicbrainz_service_->lookupService();
}

void BenchMainWindow::startMetadataOperationRecovery() {
    if (metadata_recovery_started_ || metadata_operation_running_ || persistence_ == nullptr ||
        database_path_.empty()) {
        return;
    }
    metadata_recovery_started_ = true;
    metadata_operation_running_ = true;
    metadata_operation_cancellation_ = core::CancellationSource{};
    setProperty("trackknife-metadata-operation-running", true);
    auto* const persistence_service = persistence_;
    const auto database_path = database_path_;
    const auto cancellation = metadata_operation_cancellation_.token();
    metadata_operation_watcher_.setFuture(QtConcurrent::run([database_path, persistence_service,
                                                             player = player_, cancellation] {
        return run_metadata_operation_job(database_path, persistence_service, player, cancellation);
    }));
}

void BenchMainWindow::applyCommittedMetadata(const operations::MetadataCommitResult& result) {
    // The same commit boundary carries text-only and embedded-artwork writes.
    // Cached misses and previous covers must not survive either path.
    invalidateArtwork(result.source_raw_path);
    if (local_library_ != nullptr) {
        local_library_->refreshLibrary();
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
    if (local_library_ != nullptr) {
        local_library_->refreshLibrary();
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
    if (local_library_ != nullptr) {
        local_library_->refreshLibrary();
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
    if (local_library_ != nullptr) {
        local_library_->refreshLibrary();
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
    if (requested_request_)
        update_request(requested_request_->source);
    if (queued_request_)
        update_request(queued_request_->source);
    persistUpNext();
    refreshUpNext();
    if (playback_.anchors.source.raw_path == result.source_raw_path) {
        playback_.anchors.source.raw_path = result.target_raw_path;
    }
    if (playback_.last_requested_next &&
        playback_.last_requested_next->raw_path == result.source_raw_path) {
        playback_.last_requested_next->raw_path = result.target_raw_path;
    }
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

void BenchMainWindow::finishMetadataOperationJob() {
    metadata_operation_running_ = false;
    setProperty("trackknife-metadata-operation-running", false);
    auto snapshot = metadata_operation_watcher_.result();
    if (!snapshot) {
        statusBar()->showMessage(QStringLiteral("Metadata operation returned no result"), 8'000);
        return;
    }
    for (const auto& refreshed : snapshot->refreshed_sources) {
        applyCommittedMetadata(refreshed);
    }
    for (const auto& relocated : snapshot->relocated_sources) {
        applyCommittedRelocation(relocated);
    }
    for (const auto& [relocated, document] : snapshot->refreshed_relocations) {
        applyCommittedPublicationMetadata(relocated, document);
    }
    if (!snapshot->refreshed_sources.empty() || !snapshot->relocated_sources.empty() ||
        !snapshot->refreshed_relocations.empty()) {
        schedulePersist();
    }
    metadata_operation_snapshot_ = std::move(snapshot);
    const auto file_attention =
        std::ranges::count(metadata_operation_snapshot_->file_publications,
                           operations::FilePublicationJournalState::needs_reconciliation,
                           &operations::FilePublicationJournalRecord::state);
    const auto metadata_attention = metadata_operation_snapshot_->reconciliation.size();
    setProperty("trackknife-metadata-reconciliation-count",
                static_cast<qulonglong>(metadata_attention));
    setProperty("trackknife-file-reconciliation-count", static_cast<qulonglong>(file_attention));

    if (metadata_operation_snapshot_->error) {
        statusBar()->showMessage(
            QStringLiteral("Preparation operation failed: %1")
                .arg(displayText(metadata_operation_snapshot_->error->message)),
            10'000);
    } else if (!metadata_operation_snapshot_->recovery.empty() ||
               !metadata_operation_snapshot_->file_recovery.empty()) {
        const auto metadata_recovered =
            std::ranges::count_if(metadata_operation_snapshot_->recovery, [](const auto& result) {
                return result.outcome != operations::MetadataRecoveryOutcome::needs_reconciliation;
            });
        const auto file_recovered = std::ranges::count_if(
            metadata_operation_snapshot_->file_recovery, [](const auto& result) {
                return result.outcome !=
                       operations::FilePublicationRecoveryOutcome::needs_reconciliation;
            });
        const auto recovered = metadata_recovered + file_recovered;
        if (recovered > 0) {
            statusBar()->showMessage(QStringLiteral("Recovered %1 interrupted file operation%2")
                                         .arg(recovered)
                                         .arg(recovered == 1 ? QString{} : QStringLiteral("s")),
                                     5'000);
        }
    }
    presentInterruptedOperations();
}

// Crash recovery is silent when it succeeds. Only operations recovery could
// neither finish nor safely roll back are surfaced, each exactly once: shown
// journal ids are remembered so a known incident does not reopen the window on
// every start.
void BenchMainWindow::presentInterruptedOperations() {
    if (!metadata_operation_snapshot_) {
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
            .file = QString::fromStdString(core::escape_raw_path(raw_path)),
            .detail = std::move(detail),
        });
    };
    for (const auto& record : metadata_operation_snapshot_->reconciliation) {
        collect(record.id, record.source_raw_path,
                record.failure
                    ? displayText(record.failure->message)
                    : QStringLiteral("An interrupted tag write could not be finished or safely "
                                     "rolled back; the file was left untouched"));
    }
    for (const auto& record : metadata_operation_snapshot_->file_publications) {
        if (record.state != operations::FilePublicationJournalState::needs_reconciliation) {
            continue;
        }
        auto detail = record.failure
                          ? displayText(record.failure->message)
                          : QStringLiteral("An interrupted move could not be finished or safely "
                                           "rolled back");
        detail += QStringLiteral(" · planned target %1")
                      .arg(QString::fromStdString(core::escape_raw_path(record.target_raw_path)));
        collect(record.id, record.source_raw_path, std::move(detail));
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
