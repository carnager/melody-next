// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/job_registry.hpp"

#include <utility>
#include <vector>

namespace trackknife::engine {

JobRegistry::JobRegistry(EventSink sink) : sink_(std::move(sink)) {}

JobRegistry::~JobRegistry() {
    {
        const std::lock_guard guard{mutex_};
        for (auto& [id, running] : running_) {
            running.cancellation.request_cancellation();
        }
    }
    wait_all();
}

void JobRegistry::emit(const protocol::Event& event) {
    // Serialised because jobs run concurrently and a sink is usually a single
    // socket. Holding the lock across the call also means a sink cannot
    // observe a half-written event.
    const std::lock_guard guard{mutex_};
    if (sink_) {
        sink_(event);
    }
}

void JobRegistry::retire(const core::StableId& job_id) {
    const std::lock_guard guard{mutex_};
    if (const auto found = running_.find(job_id); found != running_.end()) {
        found->second.worker.detach();
        running_.erase(found);
    }
}

core::StableId JobRegistry::submit(std::string name, Work work) {
    const auto job_id = core::StableId::random();
    const auto identity = job_id.to_string();
    core::CancellationSource cancellation;
    const auto token = cancellation.token();

    const Reporter reporter = [this, identity](protocol::Json progress) {
        protocol::Json data = progress.is_object() ? std::move(progress) : protocol::Json::object();
        data["job_id"] = identity;
        emit(protocol::Event{.name = "job.progress", .data = std::move(data)});
    };

    std::thread worker{[this, job_id, identity, name = std::move(name), work = std::move(work),
                        token, reporter]() mutable {
        protocol::Json data = protocol::Json::object();
        data["job_id"] = identity;
        data["job"] = name;
        data["outcome"] = work(token, reporter);
        // Whether it stopped early is the job's to report in its outcome;
        // the registry does not second-guess it.
        data["cancelled"] = token.is_cancellation_requested();
        // Retire before the final event so a client acting on job.finished
        // cannot find the job still listed as running.
        retire(job_id);
        emit(protocol::Event{.name = "job.finished", .data = std::move(data)});
    }};

    const std::lock_guard guard{mutex_};
    running_.emplace(job_id, Running{.cancellation = cancellation, .worker = std::move(worker)});
    return job_id;
}

bool JobRegistry::cancel(const core::StableId& job_id) {
    const std::lock_guard guard{mutex_};
    const auto found = running_.find(job_id);
    if (found == running_.end()) {
        return false;
    }
    found->second.cancellation.request_cancellation();
    return true;
}

void JobRegistry::wait_all() {
    while (true) {
        std::thread worker;
        {
            const std::lock_guard guard{mutex_};
            if (running_.empty()) {
                return;
            }
            worker = std::move(running_.begin()->second.worker);
            running_.erase(running_.begin());
        }
        if (worker.joinable()) {
            worker.join();
        }
    }
}

} // namespace trackknife::engine
