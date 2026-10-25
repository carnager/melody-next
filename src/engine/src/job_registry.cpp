// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/job_registry.hpp"

#include <utility>
#include <vector>

namespace trackknife::engine {

namespace {

thread_local std::uint64_t requester = 0;

} // namespace

std::uint64_t current_requester() noexcept { return requester; }

RequesterScope::RequesterScope(const std::uint64_t connection) noexcept : previous_(requester) {
    requester = connection;
}

RequesterScope::~RequesterScope() { requester = previous_; }

JobRegistry::JobRegistry(EventSink sink, AddressedSink addressed)
    : sink_(std::move(sink)), addressed_(std::move(addressed)) {}

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

void JobRegistry::publish_finished(protocol::Json data, protocol::Json outcome,
                                   const std::uint64_t owner) {
    const std::lock_guard guard{sink_mutex_};
    // ADR-0327: the outcome -- a cover's bytes, megabytes of them -- is the
    // asker's. Every other client hears that the job finished, not what it
    // found: broadcast, one cover fetched let every client go past its limit
    // on unread events, and went over the phone's mobile data besides.
    if (owner != 0U && addressed_) {
        auto for_owner = data;
        for_owner["outcome"] = std::move(outcome);
        addressed_(protocol::Event{.name = "job.finished", .data = std::move(data)}, owner,
                   protocol::Event{.name = "job.finished", .data = std::move(for_owner)});
        return;
    }
    data["outcome"] = std::move(outcome);
    if (sink_) {
        sink_(protocol::Event{.name = "job.finished", .data = std::move(data)});
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
    // Whose request this is: its outcome goes to that connection.
    const auto owner = current_requester();
    const std::lock_guard guard{mutex_};
    auto& running = running_[job_id];
    running.cancellation = cancellation;
    running.worker = std::thread{[this, job_id, identity, name = std::move(name),
                                  work = std::move(work), token, reporter, owner]() mutable {
        protocol::Json data = protocol::Json::object();
        data["job_id"] = identity;
        data["job"] = name;
        auto outcome = work(token, reporter);
        // Whether it stopped early is the job's to report in its outcome;
        // the registry does not second-guess it.
        data["cancelled"] = token.is_cancellation_requested();
        // Retire before the final event so a client acting on job.finished
        // cannot find the job still listed as running.
        retire(job_id);
        publish_finished(std::move(data), std::move(outcome), owner);
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
