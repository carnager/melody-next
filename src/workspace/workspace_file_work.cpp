// SPDX-License-Identifier: GPL-3.0-only

// ADR-0237: what the file tools are given -- the engine that does the work,
// the stores their choices are kept in -- and how this workspace follows
// what they changed. Both windows' tag editors, ReplayGain and converters
// are opened with it.

#include "workspace/workspace.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "bench/post_back.hpp"
#include "bench/remote_mount.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/operations/file_publication_apply.hpp"
#include "trackknife/operations/metadata_commit.hpp"
#include "uicommon/list_persistence_service.hpp"
#include "workspace/library_browser.hpp"
#include "workspace/workspace_view.hpp"

#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
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

std::span<const std::string_view> Workspace::taggerFields() {
    return std::span{default_metadata_fields};
}

void Workspace::refreshLocalLibrary() {
    if (auto* browser = localEngine().browser.data()) {
        browser->refreshLibrary();
    }
}

std::shared_ptr<engine::RemoteFileWork> Workspace::fileWorkOf(const EngineKey& engine) const {
    const auto* engine_link = link(engine);
    if (engine_link == nullptr || !engine_link->does_file_work) {
        return nullptr;
    }
    return engine_link->file_work;
}

Workspace::TaggerOpening Workspace::taggerServices(std::shared_ptr<engine::RemoteFileWork> work) {
    auto* const persistence_service = persistence_;
    // ADR-0237: the engine holding the files does all the work on them. One
    // elsewhere moves files to its own destinations, and its moves are
    // followed here through its mount.
    const auto* work_link = linkOfWork(work.get());
    const bool elsewhere = work_link == nullptr || !work_link->key.isLocal();
    const auto work_engine = work_link != nullptr ? work_link->key : EngineKey::local();
    const auto mount = work_link != nullptr && elsewhere ? mountOf(*work_link) : RemoteMount{};
    auto mounted = std::make_shared<MountedMoves>();
    const auto unavailable = QStringLiteral("Trackknife persistence is unavailable");
    TaggerOpening opening{
        .services =
            TaggerServices{
                .plan_applier_factory = engineMetadataPlanApplierFactory(work),
                .apply_observer = metadataApplyObserver(),
                .transformation_store = MetadataTransformationStore{
                    .load =
                        [persistence_service,
                         unavailable](MetadataTransformationStore::LoadCompletion completion) {
                            if (!persistence_service) {
                                completion({}, unavailable);
                                return;
                            }
                            persistence_service->loadMetadataTransformationChains(
                                std::move(completion));
                        },
                    .save =
                        [persistence_service,
                         unavailable](persistence::SavedMetadataTransformationChain chain,
                                      MetadataTransformationStore::Completion completion) {
                            if (!persistence_service) {
                                completion(unavailable);
                                return;
                            }
                            persistence_service->saveMetadataTransformationChain(
                                std::move(chain), std::move(completion));
                        },
                    .remove =
                        [persistence_service, unavailable](
                            core::StableId id, MetadataTransformationStore::Completion completion) {
                            if (!persistence_service) {
                                completion(unavailable);
                                return;
                            }
                            persistence_service->removeMetadataTransformationChain(
                                id, std::move(completion));
                        },
                },
                .output_profile_store = buildOutputProfileStore(work_engine),
                .file_plan_applier_factory =
                    enginePublicationPlanApplierFactory(work, elsewhere, mount, mounted),
                .file_apply_observer =
                    [this, elsewhere, work_engine,
                     mounted](const operations::FilePublicationApplyResult& result) {
                        auto committed = false;
                        for (const auto& source : result.sources) {
                            if (source.metadata_commit) {
                                applyCommittedMetadata(*source.metadata_commit);
                                committed = true;
                            }
                            if (source.commit && elsewhere) {
                                const auto here = std::ranges::find(
                                    *mounted, source.commit->journal_id,
                                    &operations::FilePublicationCommitResult::journal_id);
                                applyEngineRelocation(work_engine, *source.commit,
                                                      here == mounted->end() ? nullptr : &*here);
                                if (source.published_metadata) {
                                    applyCommittedPublicationMetadata(*source.commit,
                                                                      *source.published_metadata);
                                    if (here != mounted->end()) {
                                        applyCommittedPublicationMetadata(
                                            *here, *source.published_metadata);
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
                .layout_store = MetadataDialogLayoutStore{
                    .load =
                        [persistence_service, unavailable](
                            QString key, MetadataDialogLayoutStore::LoadCompletion completion) {
                            if (!persistence_service) {
                                completion({}, unavailable);
                                return;
                            }
                            persistence_service->loadUiState(std::move(key), std::move(completion));
                        },
                    .save =
                        [persistence_service,
                         unavailable](QString key, QByteArray value,
                                      MetadataDialogLayoutStore::Completion completion) {
                            if (!persistence_service) {
                                if (completion) {
                                    completion(unavailable);
                                }
                                return;
                            }
                            persistence_service->saveUiState(std::move(key), std::move(value),
                                                             std::move(completion));
                        },
                },
                .musicbrainz = engineLookupService(work, this),
                .tools = engineFileWorkTools(work),
            },
        .artwork_applier = engineArtworkPlanApplierFactory(work),
        .artwork_observer =
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
            },
        .engine = work_engine,
    };
    return opening;
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

MetadataPropertiesSourceReader
Workspace::selectionSourceReader(ListTab& tab, std::vector<QPersistentModelIndex> rows) {
    return selectionSourceReader(tab.model, std::move(rows));
}

MetadataPropertiesSourceReader
Workspace::selectionSourceReader(LocalListModel* source_model,
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

MetadataApplyObserver Workspace::metadataApplyObserver() {
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

ArtworkWritePlanApplierFactory
Workspace::engineArtworkPlanApplierFactory(std::shared_ptr<engine::RemoteFileWork> work) {
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
Workspace::engineMetadataPlanApplierFactory(std::shared_ptr<engine::RemoteFileWork> work) {
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

const Workspace::EngineLink* Workspace::linkOfWork(const engine::RemoteFileWork* const work) const {
    for (const auto& candidate : engines_) {
        if (candidate->file_work.get() == work) {
            return candidate.get();
        }
    }
    return nullptr;
}

FilePublicationPlanApplierFactory
Workspace::enginePublicationPlanApplierFactory(std::shared_ptr<engine::RemoteFileWork> work,
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

void Workspace::applyEngineRelocation(const EngineKey& engine,
                                      const operations::FilePublicationCommitResult& result,
                                      const operations::FilePublicationCommitResult* here) {
    if (here != nullptr) {
        queueEngineRelocation(here->source_raw_path, here->target_raw_path);
        refreshLocalLibrary();
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
            view_->showMessage(QStringLiteral("File-path view refresh needs attention: %1")
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
        view_->refreshUpNext();
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

OutputProfileStore Workspace::buildOutputProfileStore(const EngineKey& destinations_of) {
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
                [this, work](std::string path, EngineFolderListingCompletion done) {
                    offThread(
                        this, [work, path] { return work->folders(path); },
                        [done = std::move(done)](auto listed) {
                            if (!listed) {
                                done(std::unexpected(std::move(listed.error())));
                                return;
                            }
                            done(EngineFolderListing{.path = std::move(listed->path),
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
                    id, [window, id, completion = std::move(completion)](QString error) {
                        if (window && error.isEmpty()) {
                            window->pushLayouts({id});
                        }
                        completion(error);
                    });
            },
        .save_destination = destinations.save,
        .remove_destination = destinations.remove,
        .destinations_on = elsewhere ? destinations.name : QString{},
        .destinations_key = destinations.key,
        .places = std::move(places),
    };
}

void Workspace::applyCommittedMetadata(const operations::MetadataCommitResult& result) {
    // The same commit boundary carries text-only and embedded-artwork writes.
    // Cached misses and previous covers must not survive either path.
    invalidateArtwork(result.source_raw_path);
    refreshLocalLibrary();
    for (auto& tab : list_tabs_) {
        auto applied = tab->model->applyCommittedMetadata(result.source_raw_path, result.document,
                                                          result.published_revision);
        if (!applied) {
            view_->showMessage(QStringLiteral("Metadata view refresh needs attention: %1")
                                   .arg(displayText(applied.error().message)),
                               8'000);
            continue;
        }
        if (*applied > 0U) {
            syncArtwork(*tab);
        }
    }
}

void Workspace::applyCommittedCueReplayGain(const operations::CueReplayGainCommitResult& result) {
    refreshLocalLibrary();
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

void Workspace::applyCommittedLoudnessSidecar(
    const operations::LoudnessSidecarCommitResult& result) {
    refreshLocalLibrary();
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

void Workspace::applyCommittedRelocation(const operations::FilePublicationCommitResult& result) {
    queueEngineRelocation(result.source_raw_path, result.target_raw_path);
    refreshLocalLibrary();
    for (auto& tab : list_tabs_) {
        auto applied =
            tab->model->applyCommittedRelocation(result.source_raw_path, result.target_raw_path,
                                                 result.source_revision, result.target_revision);
        if (!applied) {
            view_->showMessage(QStringLiteral("File-path view refresh needs attention: %1")
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
    view_->refreshUpNext();
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

void Workspace::applyCommittedPublicationMetadata(
    const operations::FilePublicationCommitResult& result,
    const metadata::MetadataDocument& document) {
    for (auto& tab : list_tabs_) {
        auto applied = tab->model->applyCommittedMetadata(result.target_raw_path, document,
                                                          result.target_revision);
        if (!applied) {
            view_->showMessage(QStringLiteral("Published metadata view refresh needs attention: %1")
                                   .arg(displayText(applied.error().message)),
                               8'000);
            continue;
        }
        if (*applied > 0U) {
            syncArtwork(*tab);
        }
    }
}
} // namespace trackknife::bench
