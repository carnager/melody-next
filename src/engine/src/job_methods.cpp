// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/job_methods.hpp"

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

void register_catalogue_jobs(JobCatalog& jobs, Catalogue& catalogue) {
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

} // namespace trackknife::engine
