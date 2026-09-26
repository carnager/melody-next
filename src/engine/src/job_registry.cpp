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

void JobRegistry::publish(const protocol::Event& event) {
    // Serialised because jobs run concurrently and a sink is usually a single
    // socket. Holding the lock across the call also means a sink cannot
    // observe a half-written event. Not mutex_: a sink that is slow must not
    // stall job.cancel, which is the one thing that has to be answered at
    // once.
    const std::lock_guard guard{sink_mutex_};
    if (sink_) {
        sink_(event);
    }
}

void JobRegistry::retire(const core::StableId& job_id) {
    const std::lock_guard guard{mutex_};
    if (const auto found = running_.find(job_id); found != running_.end()) {
        retired_.push_back(std::move(found->second.worker));
        running_.erase(found);
    }
}

void JobRegistry::reap() {
    std::vector<std::thread> retired;
    {
        const std::lock_guard guard{mutex_};
        retired.swap(retired_);
    }
    for (auto& worker : retired) {
        // A retired job has only its final event left to publish.
        if (worker.joinable()) {
            worker.join();
        }
    }
}

core::StableId JobRegistry::submit(std::string name, Work work) {
    reap();
    const auto job_id = core::StableId::random();
    const auto identity = job_id.to_string();
    core::CancellationSource cancellation;
    const auto token = cancellation.token();

    const Reporter reporter = [this, identity](protocol::Json progress) {
        protocol::Json data = progress.is_object() ? std::move(progress) : protocol::Json::object();
        data["job_id"] = identity;
        publish(protocol::Event{.name = "job.progress", .data = std::move(data)});
    };

    // Listed before its thread starts, under the lock the thread's retire()
    // takes: a job that finished at once used to retire before it was
    // listed, and was then listed as running forever.
    const std::lock_guard guard{mutex_};
    auto& running = running_[job_id];
    running.cancellation = cancellation;
    running.worker = std::thread{[this, job_id, identity, name = std::move(name), work = std::move(work),
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
        publish(protocol::Event{.name = "job.finished", .data = std::move(data)});
    }};
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
        reap();
        std::thread worker;
        {
            const std::lock_guard guard{mutex_};
            if (running_.empty() && retired_.empty()) {
                return;
            }
            if (running_.empty()) {
                continue;
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
