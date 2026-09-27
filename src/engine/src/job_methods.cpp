// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/job_methods.hpp"

#include "trackknife/engine/file_work_wire.hpp"
#include "trackknife/loudness/scan.hpp"
#include "trackknife/operations/cue_replay_gain_apply.hpp"
#include "trackknife/operations/loudness_sidecar_apply.hpp"
#include "trackknife/operations/metadata_apply.hpp"
#include "trackknife/operations/metadata_commit.hpp"
#include "trackknife/persistence/operation_journal.hpp"

#include <atomic>
#include <chrono>
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
                             LocalCatalogue& catalogue) {
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

    jobs.on("metadata.apply", [database = std::move(database),
                               &catalogue](const Json& params) -> core::Result<JobRegistry::Work> {
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
        return [plan = std::move(*plan), database,
                &catalogue](const core::CancellationToken& token, const JobRegistry::Reporter& report) {
            auto opened = persistence::SqliteMetadataOperationJournal::open(database);
            if (!opened) {
                return Json{{"error", wire::encode(opened.error())}};
            }
            auto journal = std::move(*opened);
            // Part of the commit: a file written is a file re-read into the
            // library before the write counts, so nothing sees it half done.
            const operations::MetadataDependentStateCommitter dependent =
                [&catalogue](const operations::MetadataCommitResult& result) -> core::Result<void> {
                auto refreshed = catalogue.refresh({result.source_raw_path});
                return refreshed ? core::Result<void>{}
                                 : std::unexpected(std::move(refreshed.error()));
            };
            auto applied = operations::apply_metadata_write_plan(
                plan,
                [&journal, &dependent](const metadata::MetadataWritePlanSource& source,
                                       const core::CancellationToken& source_token) {
                    return operations::commit_flac_metadata_source(source, journal, dependent,
                                                                   source_token);
                },
                [&journal, &dependent](const metadata::MetadataWritePlanCueSheet& sheet,
                                       const core::CancellationToken& sheet_token) {
                    return operations::commit_cue_replay_gain_sheet(sheet, journal, dependent,
                                                                    sheet_token);
                },
                [&journal, &dependent](const metadata::MetadataWritePlanSidecar& sidecar,
                                       const core::CancellationToken& sidecar_token) {
                    return operations::commit_loudness_sidecar(sidecar, journal, dependent,
                                                               sidecar_token);
                },
                [&report](const operations::MetadataApplyProgress& progress) {
                    report(wire::encode(progress));
                },
                token);
            if (!applied) {
                return Json{{"error", wire::encode(applied.error())}};
            }
            return Json{{"result", wire::encode(*applied)}};
        };
    });
}

} // namespace trackknife::engine
