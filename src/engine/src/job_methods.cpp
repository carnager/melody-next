// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/job_methods.hpp"

#include "trackknife/core/local_sources.hpp"
#include "trackknife/engine/file_work_wire.hpp"
#include "trackknife/loudness/replaygain.hpp"
#include "trackknife/loudness/scan.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/write_plan.hpp"
#include "trackknife/operations/artwork_apply.hpp"
#include "trackknife/operations/cue_replay_gain_apply.hpp"
#include "trackknife/operations/file_publication_apply.hpp"
#include "trackknife/operations/loudness_sidecar_apply.hpp"
#include "trackknife/operations/metadata_apply.hpp"
#include "trackknife/operations/metadata_commit.hpp"
#include "trackknife/persistence/file_publication_journal.hpp"
#include "trackknife/persistence/operation_journal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <utility>

namespace trackknife::engine {
namespace {

using protocol::Json;

[[nodiscard]] core::Error bad_params(std::string message, std::string member) {
    return core::Error{.code = core::ErrorCode::invalid_argument,
                       .message = std::move(message),
                       .context = {{.key = "param", .value = std::move(member)}}};
}

// How often a blocking operation's progress counters are sampled. Fast enough
// to look live, slow enough that a client is not flooded by a scan that visits
// thousands of files a second.
constexpr auto progress_interval = std::chrono::milliseconds{200};

// A reviewed tag plan written as the engine writes every one: journaled in
// its database, each file re-read into its library in the same commit.
[[nodiscard]] core::Result<operations::MetadataApplyResult>
apply_metadata_plan(const metadata::MetadataWritePlan& plan, const std::filesystem::path& database,
                    LocalCatalogue& catalogue,
                    const operations::MetadataApplyProgressCallback& progress,
                    const core::CancellationToken& token) {
    auto opened = persistence::SqliteMetadataOperationJournal::open(database);
    if (!opened) {
        return std::unexpected(std::move(opened.error()));
    }
    auto journal = std::move(*opened);
    // Part of the commit: a file written is a file re-read into the library
    // before the write counts, so nothing sees it half done.
    const operations::MetadataDependentStateCommitter dependent =
        [&catalogue](const operations::MetadataCommitResult& result) -> core::Result<void> {
        auto refreshed = catalogue.refresh({result.source_raw_path});
        return refreshed ? core::Result<void>{} : std::unexpected(std::move(refreshed.error()));
    };
    return operations::apply_metadata_write_plan(
        plan,
        [&journal, &dependent](const metadata::MetadataWritePlanSource& source,
                               const core::CancellationToken& source_token) {
            return operations::commit_metadata_source(source, journal, dependent,
                                                           source_token);
        },
        [&journal, &dependent](const metadata::MetadataWritePlanCueSheet& sheet,
                               const core::CancellationToken& sheet_token) {
            return operations::commit_cue_replay_gain_sheet(sheet, journal, dependent, sheet_token);
        },
        [&journal, &dependent](const metadata::MetadataWritePlanSidecar& sidecar,
                               const core::CancellationToken& sidecar_token) {
            return operations::commit_loudness_sidecar(sidecar, journal, dependent, sidecar_token);
        },
        progress, token);
}

} // namespace

void JobCatalog::on(std::string name, Factory factory) {
    factories_.insert_or_assign(std::move(name), std::move(factory));
}

bool JobCatalog::knows(const std::string_view name) const {
    return factories_.find(name) != factories_.end();
}

core::Result<JobRegistry::Work> JobCatalog::build(const std::string_view name,
                                                  const Json& params) const {
    const auto found = factories_.find(name);
    if (found == factories_.end()) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::unsupported,
                        .message = "unknown job",
                        .context = {{.key = "job", .value = std::string{name}}}});
    }
    return found->second(params);
}

void register_job_methods(protocol::Dispatcher& dispatcher, JobRegistry& registry,
                          const JobCatalog& catalogue) {
    dispatcher.on("job.submit", [&registry, &catalogue](const Json& params) -> core::Result<Json> {
        const auto name = params.find("job");
        if (name == params.end() || !name->is_string()) {
            return std::unexpected(bad_params("a job name is required", "job"));
        }
        const auto requested = name->get<std::string>();
        // Validate before starting. A job that begins and immediately fails
        // reports through events, which is a worse place for a caller's
        // mistake than the response to their own submit.
        auto work = catalogue.build(requested, params.value("params", Json::object()));
        if (!work) {
            return std::unexpected(std::move(work.error()));
        }
        Json answer = Json::object();
        answer["job_id"] = registry.submit(requested, std::move(*work)).to_string();
        return answer;
    });

    dispatcher.on("job.cancel", [&registry](const Json& params) -> core::Result<Json> {
        const auto raw = params.find("job_id");
        if (raw == params.end() || !raw->is_string()) {
            return std::unexpected(bad_params("a job identity is required", "job_id"));
        }
        auto job_id = core::StableId::parse(raw->get<std::string>());
        if (!job_id) {
            return std::unexpected(bad_params("job_id is not an identity", "job_id"));
        }
        Json answer = Json::object();
        // Whether the identity was known, not whether the job stopped:
        // cancelling is a request, and a job that finished first finished.
        answer["accepted"] = registry.cancel(*job_id);
        return answer;
    });
}

void register_catalogue_jobs(JobCatalog& jobs, LocalCatalogue& catalogue) {
    jobs.on("catalogue.scan", [&catalogue](const Json&) -> core::Result<JobRegistry::Work> {
        return [&catalogue](const core::CancellationToken& token,
                            const JobRegistry::Reporter& report) {
            // scan() blocks and updates atomic counters, so progress is
            // sampled beside it. This is the same shape the workspace used
            // with a timer; the engine now owns the sampling because a remote
            // client has no timer to lend.
            persistence::LibraryScanProgress progress;
            std::atomic_bool running{true};
            std::thread sampler{[&]() {
                while (running.load()) {
                    report(Json{{"visited", progress.visited.load()},
                                {"indexed", progress.indexed.load()},
                                {"failed", progress.failed.load()}});
                    std::this_thread::sleep_for(progress_interval);
                }
            }};

            auto outcome = catalogue.scan(token, progress);
            running.store(false);
            sampler.join();

            Json result = Json::object();
            result["visited"] = progress.visited.load();
            result["indexed"] = progress.indexed.load();
            result["failed"] = progress.failed.load();
            if (!outcome) {
                result["error"] = outcome.error().message;
                return result;
            }
            result["cancelled"] = outcome->cancelled;
            result["incomplete"] = outcome->incomplete;
            return result;
        };
    });
}

void register_file_work_jobs(JobCatalog& jobs, std::filesystem::path database,
                             LocalCatalogue& catalogue, MoveFollower follow) {
    jobs.on("loudness.scan", [](const Json& params) -> core::Result<JobRegistry::Work> {
        const auto listed = params.find("items");
        if (listed == params.end() || !listed->is_array()) {
            return std::unexpected(bad_params("items to measure are required", "items"));
        }
        std::vector<loudness::LoudnessScanItem> items;
        items.reserve(listed->size());
        for (const auto& value : *listed) {
            auto item = wire::decode_scan_item(value);
            if (!item) {
                return std::unexpected(std::move(item.error()));
            }
            items.push_back(std::move(*item));
        }
        loudness::LoudnessScanOptions options;
        if (const auto given = params.find("options"); given != params.end()) {
            auto decoded = wire::decode_scan_options(*given);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            options = *decoded;
        }
        // The pool is the engine's own, sized by its hardware: the client's
        // idea of how many workers suit its machine says nothing about this one.
        const auto hardware = std::thread::hardware_concurrency();
        options.maximum_parallelism =
            std::min<std::size_t>(loudness::maximum_scan_parallelism,
                                  std::max<std::size_t>(1U, hardware == 0U ? 2U : hardware / 2U));
        return [items = std::move(items), options](const core::CancellationToken& token,
                                                   const JobRegistry::Reporter& report) {
            auto scanned = loudness::scan_loudness(
                items, options,
                [&report](const loudness::LoudnessScanProgress& progress) {
                    report(Json{{"item_index", progress.item_index},
                                {"completed_items", progress.completed_items},
                                {"total_items", progress.total_items}});
                },
                token);
            if (!scanned) {
                return Json{{"error", wire::encode(scanned.error())}};
            }
            return Json{{"result", wire::encode(*scanned)}};
        };
    });

    jobs.on(
        "metadata.apply",
        [database = database, &catalogue](const Json& params) -> core::Result<JobRegistry::Work> {
            const auto given = params.find("plan");
            if (given == params.end()) {
                return std::unexpected(bad_params("a plan to write is required", "plan"));
            }
            auto plan = wire::decode_write_plan(*given);
            if (!plan) {
                return std::unexpected(std::move(plan.error()));
            }
            if (!plan->ready()) {
                return std::unexpected(
                    core::Error{.code = core::ErrorCode::conflict,
                                .message = "the plan has blocking issues; nothing was written",
                                .context = {{.key = "param", .value = "plan"}}});
            }
            return [plan = std::move(*plan), database, &catalogue](
                       const core::CancellationToken& token, const JobRegistry::Reporter& report) {
                auto applied = apply_metadata_plan(
                    plan, database, catalogue,
                    [&report](const operations::MetadataApplyProgress& progress) {
                        report(wire::encode(progress));
                    },
                    token);
                if (!applied) {
                    return Json{{"error", wire::encode(applied.error())}};
                }
                // Each file that could not be written, in full: a window shows
                // a line of it, and on an unfamiliar filesystem the rest --
                // which file was read, its prepared copy -- is the clue.
                for (const auto& source : applied->sources) {
                    if (!source.issue) {
                        continue;
                    }
                    std::cerr << "melodyd: tags not written to "
                              << core::display_raw_path(source.raw_path) << ": "
                              << source.issue->message;
                    for (const auto& [key, value] : source.issue->context) {
                        std::cerr << " [" << key << ": " << value << "]";
                    }
                    std::cerr << "\n";
                }
                return Json{{"result", wire::encode(*applied)}};
            };
        });

    jobs.on(
        "artwork.apply",
        [database = database, &catalogue](const Json& params) -> core::Result<JobRegistry::Work> {
            const auto given = params.find("plan");
            if (given == params.end()) {
                return std::unexpected(bad_params("an artwork plan to write is required", "plan"));
            }
            auto plan = wire::decode_artwork_plan(*given);
            if (!plan) {
                return std::unexpected(std::move(plan.error()));
            }
            if (!plan->ready()) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::conflict,
                    .message = "the artwork plan has blocking issues; nothing was written",
                    .context = {{.key = "param", .value = "plan"}}});
            }
            return [plan = std::move(*plan), database, &catalogue](
                       const core::CancellationToken& token, const JobRegistry::Reporter& report) {
                auto opened = persistence::SqliteMetadataOperationJournal::open(database);
                if (!opened) {
                    return Json{{"error", wire::encode(opened.error())}};
                }
                auto journal = std::move(*opened);
                const operations::MetadataDependentStateCommitter dependent =
                    [&catalogue](
                        const operations::MetadataCommitResult& result) -> core::Result<void> {
                    auto refreshed = catalogue.refresh({result.source_raw_path});
                    return refreshed ? core::Result<void>{}
                                     : std::unexpected(std::move(refreshed.error()));
                };
                auto applied = operations::apply_artwork_write_plan(
                    plan,
                    [&journal, &dependent](const metadata::ArtworkWritePlanSource& source,
                                           const core::CancellationToken& source_token) {
                        return operations::commit_artwork_source(source, journal, dependent,
                                                                 source_token);
                    },
                    [&report](const operations::ArtworkApplyProgress& progress) {
                        report(wire::encode(progress));
                    },
                    token, operations::ArtworkApplyOptions{.maximum_parallelism = 2U});
                if (!applied) {
                    return Json{{"error", wire::encode(applied.error())}};
                }
                return Json{{"result", wire::encode(*applied)}};
            };
        });
    jobs.on("paths.preflight", [](const Json& params) -> core::Result<JobRegistry::Work> {
        const auto given = params.find("plan");
        if (given == params.end()) {
            return std::unexpected(bad_params("a path plan to check is required", "plan"));
        }
        auto plan = wire::decode_path_plan(*given);
        if (!plan) {
            return std::unexpected(std::move(plan.error()));
        }
        return [plan = std::move(*plan)](const core::CancellationToken& token,
                                         const JobRegistry::Reporter&) {
            auto checked = operations::preflight_output_paths(plan, token);
            if (!checked) {
                return Json{{"error", wire::encode(checked.error())}};
            }
            return Json{{"result", wire::encode(*checked)}};
        };
    });

    jobs.on(
        "preparation.apply",
        [database = database, &catalogue,
         follow = std::move(follow)](const Json& params) -> core::Result<JobRegistry::Work> {
            const auto given = params.find("plan");
            if (given == params.end()) {
                return std::unexpected(bad_params("a reviewed plan is required", "plan"));
            }
            auto plan = wire::decode_preparation_plan(*given);
            if (!plan) {
                return std::unexpected(std::move(plan.error()));
            }
            if (!plan->ready() || !plan->has_path_operation()) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::conflict,
                    .message = "the plan is not ready to move files; nothing was changed",
                    .context = {{.key = "param", .value = "plan"}}});
            }
            return [plan = std::move(*plan), database, &catalogue, follow](
                       const core::CancellationToken& token, const JobRegistry::Reporter& report) {
                auto file_opened = persistence::SqliteFilePublicationJournal::open(database);
                if (!file_opened) {
                    return Json{{"error", wire::encode(file_opened.error())}};
                }
                auto metadata_opened = persistence::SqliteMetadataOperationJournal::open(database);
                if (!metadata_opened) {
                    return Json{{"error", wire::encode(metadata_opened.error())}};
                }
                auto file_journal = std::move(*file_opened);
                auto metadata_journal = std::move(*metadata_opened);
                const operations::MetadataDependentStateCommitter metadata_dependent =
                    [&catalogue](
                        const operations::MetadataCommitResult& result) -> core::Result<void> {
                    auto refreshed = catalogue.refresh({result.source_raw_path});
                    return refreshed ? core::Result<void>{}
                                     : std::unexpected(std::move(refreshed.error()));
                };
                const operations::FilePublicationDependentStateCommitter file_dependent =
                    [&follow](const operations::FilePublicationCommitResult& result) {
                        return follow(result, nullptr);
                    };
                const operations::DestinationArtifactDependentStateCommitter artifact_dependent =
                    [&follow](const operations::FilePublicationCommitResult& result,
                              const metadata::MetadataDocument& document) {
                        return follow(result, &document);
                    };
                auto applied = operations::apply_preparation_publications(
                    plan, file_journal, metadata_journal, metadata_dependent, file_dependent,
                    artifact_dependent,
                    [&report](const operations::FilePublicationApplyProgress& progress) {
                        report(wire::encode(progress));
                    },
                    token, operations::FilePublicationApplyOptions{.maximum_parallelism = 2U});
                if (!applied) {
                    return Json{{"error", wire::encode(applied.error())}};
                }
                return Json{{"result", wire::encode(*applied)}};
            };
        });
    jobs.on(
        "replaygain.apply",
        [database = database, &catalogue](const Json& params) -> core::Result<JobRegistry::Work> {
            const auto listed = params.find("paths");
            if (listed == params.end() || !listed->is_array() || listed->empty() ||
                listed->size() > 10'000U) {
                return std::unexpected(
                    bad_params("between 1 and 10000 paths are required", "paths"));
            }
            std::vector<std::string> paths;
            for (const auto& encoded : *listed) {
                auto path = encoded.is_string()
                                ? protocol::decode_raw_path(encoded.get<std::string>())
                                : core::Result<std::string>{std::unexpected(core::Error{})};
                if (!path) {
                    return std::unexpected(bad_params("a path is not an encoded path", "paths"));
                }
                paths.push_back(std::move(*path));
            }
            loudness::ReplayGainSettings settings{
                .grouping = {.mode = loudness::LoudnessGroupingMode::selection_album,
                             .expression = {}},
                .true_peak = params.value("true_peak", false),
                .sidecar_only = params.value("sidecar", false)};
            const auto grouping = params.value("grouping", std::string{"album"});
            if (grouping == "track") {
                settings.grouping.mode = loudness::LoudnessGroupingMode::track;
            } else if (grouping == "release") {
                settings.grouping.mode = loudness::LoudnessGroupingMode::release;
            } else if (grouping != "album") {
                return std::unexpected(
                    bad_params("grouping is album, release or track", "grouping"));
            }
            return [paths = std::move(paths), settings, database, &catalogue](
                       const core::CancellationToken& token, const JobRegistry::Reporter& report) {
                const auto failed = [](const core::Error& error) {
                    return Json{{"error", wire::encode(error)}};
                };
                report(Json{{"phase", "reading"}, {"completed", 0}, {"total", paths.size()}});
                std::vector<metadata::StagedMetadataSource> sources;
                sources.reserve(paths.size());
                for (const auto& path : paths) {
                    sources.push_back(
                        metadata::StagedMetadataSource{.raw_path = path,
                                                       .source_revision = std::nullopt,
                                                       .baseline = {},
                                                       .logical_track = false,
                                                       .cue_sheet = std::nullopt,
                                                       .logical_identity = std::nullopt,
                                                       .needs_metadata_capture = true});
                }
                const auto access = metadata::local_metadata_file_access();
                auto captured =
                    metadata::capture_uncached_metadata_sources(std::move(sources), access, token);
                if (!captured) {
                    return failed(captured.error());
                }
                // Gains already in a file's sidecar are part of what it says,
                // as Trackknife reads it: the same gains again change nothing.
                for (auto& source : *captured) {
                    if (!source.source_revision) {
                        continue;
                    }
                    const auto sidecar = metadata::read_loudness_sidecar(source.raw_path);
                    if (sidecar && *sidecar && (*sidecar)->matches(*source.source_revision)) {
                        loudness::project_loudness_sidecar(source.baseline, **sidecar, {},
                                                           std::nullopt);
                    }
                }
                auto selection =
                    metadata::StagedMetadataSelection::create(std::move(*captured), {});
                if (!selection) {
                    return failed(selection.error());
                }
                std::vector<std::size_t> items(paths.size());
                for (std::size_t index = 0U; index < items.size(); ++index) {
                    items[index] = index;
                }
                const std::vector<loudness::ReplayGainAudio> audio(paths.size());
                auto measured = loudness::measure_replaygain(
                    *selection, metadata::StagedMetadataPatchSet{}, items, audio, settings,
                    [&report](const loudness::LoudnessScanProgress& progress) {
                        report(Json{{"phase", "measuring"},
                                    {"completed", progress.completed_items},
                                    {"total", progress.total_items}});
                    },
                    token);
                if (!measured) {
                    return failed(measured.error());
                }
                auto problems = Json::array();
                for (const auto& problem : measured->problems) {
                    problems.push_back(
                        Json{{"path", problem.raw_path
                                          ? Json(protocol::encode_raw_path(*problem.raw_path))
                                          : Json()},
                             {"message", wire::encode_text(problem.message)}});
                }
                auto tracks = Json::array();
                for (const auto& row : measured->rows) {
                    tracks.push_back(Json{{"path", protocol::encode_raw_path(row.raw_path)},
                                          {"title", wire::encode_text(row.title)},
                                          {"integrated_lufs", row.integrated_lufs},
                                          {"track_gain", row.track_gain},
                                          {"track_peak", row.track_peak},
                                          {"album_gain", row.album_gain},
                                          {"album_peak", row.album_peak},
                                          {"status", row.status}});
                }
                Json result{{"tracks", std::move(tracks)},
                            {"problems", std::move(problems)},
                            {"written", 0},
                            {"failed", 0}};
                auto staged = loudness::stage_replaygain(*selection, measured->proposals, token);
                if (!staged) {
                    return failed(staged.error());
                }
                if (staged->staged_fields == 0U) {
                    return Json{{"result", std::move(result)}};
                }
                auto plan = metadata::build_metadata_write_plan(
                    staged->selection, staged->patches, access, token,
                    metadata::MetadataWritePlanOptions{.sidecar_loudness = settings.sidecar_only,
                                                       .true_peak_loudness = settings.true_peak});
                if (!plan) {
                    return failed(plan.error());
                }
                // As the ReplayGain window does: what was measured is written
                // whole or not at all.
                if (!plan->ready()) {
                    for (const auto& source : plan->sources) {
                        for (const auto& issue : source.issues) {
                            result["problems"].push_back(
                                Json{{"path", protocol::encode_raw_path(source.raw_path)},
                                     {"message", wire::encode_text(issue.error.message)}});
                        }
                    }
                    for (const auto& sidecar : plan->sidecars) {
                        for (const auto& issue : sidecar.issues) {
                            result["problems"].push_back(
                                Json{{"path", protocol::encode_raw_path(sidecar.raw_audio_path)},
                                     {"message", wire::encode_text(issue.error.message)}});
                        }
                    }
                    result["refused"] = true;
                    return Json{{"result", std::move(result)}};
                }
                auto applied = apply_metadata_plan(
                    *plan, database, catalogue,
                    [&report](const operations::MetadataApplyProgress& progress) {
                        report(Json{{"phase", "writing"},
                                    {"completed", progress.completed_sources},
                                    {"total", progress.total_sources}});
                    },
                    token);
                if (!applied) {
                    return failed(applied.error());
                }
                // Files whose gains went into their tags, a CUE sheet or a
                // sidecar: each counts once, wherever they went.
                std::size_t written = 0U;
                std::size_t not_written = 0U;
                const auto count = [&written, &not_written,
                                    &result](const operations::MetadataApplySourceState state,
                                             const std::string& raw_path,
                                             const std::optional<core::Error>& issue) {
                    if (state == operations::MetadataApplySourceState::committed) {
                        ++written;
                    } else if (state == operations::MetadataApplySourceState::failed) {
                        ++not_written;
                    }
                    if (issue) {
                        result["problems"].push_back(
                            Json{{"path", protocol::encode_raw_path(raw_path)},
                                 {"message", wire::encode_text(issue->message)}});
                    }
                };
                for (const auto& source : applied->sources) {
                    count(source.state, source.raw_path, source.issue);
                }
                for (const auto& sheet : applied->cue_sheets) {
                    count(sheet.state, sheet.raw_cue_path, sheet.issue);
                }
                for (const auto& sidecar : applied->sidecars) {
                    count(sidecar.state, sidecar.raw_audio_path, sidecar.issue);
                }
                result["written"] = written;
                result["failed"] = not_written;
                return Json{{"result", std::move(result)}};
            };
        });
}

} // namespace trackknife::engine
