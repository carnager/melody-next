// SPDX-License-Identifier: GPL-3.0-only

// ADR-0220 and ADR-0222: jobs, and the fault injection the framing exists for.
// ADR-0219 records a cover scan starving the control path; the point of
// submitting long work as a job is that it becomes impossible, so the central
// case here is a long job running while control calls are answered.

#include "trackknife/engine/job_registry.hpp"
#include "trackknife/protocol/dispatch.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace protocol = trackknife::protocol;
namespace engine = trackknife::engine;
namespace core = trackknife::core;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

// Collects what the engine would write to a socket.
class RecordedEvents final {
  public:
    [[nodiscard]] engine::EventSink sink() {
        return [this](const protocol::Event& event) {
            const std::lock_guard guard{mutex_};
            events_.push_back(event);
        };
    }
    [[nodiscard]] std::vector<protocol::Event> taken() {
        const std::lock_guard guard{mutex_};
        return events_;
    }
    [[nodiscard]] std::size_t count(const std::string& name) {
        const std::lock_guard guard{mutex_};
        std::size_t total = 0;
        for (const auto& event : events_) {
            total += event.name == name ? 1U : 0U;
        }
        return total;
    }

  private:
    std::mutex mutex_;
    std::vector<protocol::Event> events_;
};

void a_job_reports_progress_and_finishes() {
    RecordedEvents recorded;
    engine::JobRegistry jobs{recorded.sink()};

    const auto id =
        jobs.submit("catalogue.scan", [](const core::CancellationToken&, const auto& report) {
            for (int step = 1; step <= 3; ++step) {
                report(protocol::Json{{"visited", step}});
            }
            return protocol::Json{{"indexed", 3}};
        });
    require(!id.is_nil(), "submitting answers with an identity at once");
    jobs.wait_all();

    require(recorded.count("job.progress") == 3U, "every progress report is emitted");
    require(recorded.count("job.finished") == 1U, "and exactly one finish");

    const auto identity = id.to_string();
    for (const auto& event : recorded.taken()) {
        require(event.data.at("job_id") == identity, "every event names its job");
    }
    for (const auto& event : recorded.taken()) {
        if (event.name == "job.finished") {
            require(event.data.at("outcome").at("indexed") == 3,
                    "the finish carries the result document, with no separate collect step");
            require(event.data.at("cancelled") == false,
                    "a job that ran to completion is not cancelled");
            require(event.data.at("job") == "catalogue.scan", "and names what it was");
        }
    }
}

void cancelling_is_a_request_not_a_guarantee() {
    RecordedEvents recorded;
    engine::JobRegistry jobs{recorded.sink()};

    std::atomic_bool release{false};
    const auto id =
        jobs.submit("slow", [&release](const core::CancellationToken& token, const auto&) {
            while (!release.load() && !token.is_cancellation_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            return protocol::Json{{"stopped_early", token.is_cancellation_requested()}};
        });

    require(jobs.cancel(id), "cancelling a running job is acknowledged");
    jobs.wait_all();
    for (const auto& event : recorded.taken()) {
        if (event.name == "job.finished") {
            require(event.data.at("cancelled") == true, "the finish reports that it stopped early");
        }
    }

    // A job that already finished is not cancellable, and saying so is how a
    // client learns the difference between "stopping" and "already stopped".
    require(!jobs.cancel(id), "cancelling a finished job reports the identity is unknown");
    require(!jobs.cancel(core::StableId::random()), "as does an identity that never existed");
}

void a_long_job_does_not_starve_the_control_path() {
    // The ADR-0219 scenario. A job runs for as long as the control calls take,
    // and every one of them is answered while it runs.
    RecordedEvents recorded;
    engine::JobRegistry jobs{recorded.sink()};

    protocol::Dispatcher dispatcher;
    dispatcher.on("playback.state", [](const protocol::Json&) -> core::Result<protocol::Json> {
        return protocol::Json{{"state", "playing"}};
    });

    std::atomic_bool finish{false};
    std::atomic_int reports{0};
    static_cast<void>(jobs.submit(
        "catalogue.scan", [&](const core::CancellationToken& token, const auto& report) {
            while (!finish.load() && !token.is_cancellation_requested()) {
                report(protocol::Json{{"visited", reports.fetch_add(1) + 1}});
                std::this_thread::sleep_for(std::chrono::microseconds{200});
            }
            return protocol::Json{{"done", true}};
        }));

    constexpr int calls = 500;
    const auto started = std::chrono::steady_clock::now();
    for (int call = 0; call < calls; ++call) {
        const auto response = dispatcher.dispatch(
            protocol::Request{.id = call + 1, .method = "playback.state", .params = {}});
        require(response.result.has_value(), "every control call is answered while a job runs");
        require(response.id == call + 1, "and answered with its own id");
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    finish.store(true);
    jobs.wait_all();

    require(reports.load() > 0, "the job really was running throughout");
    // Not a timing assertion about the machine: 500 trivial dispatches sharing
    // a process with a busy job should still be milliseconds, and a second
    // would mean the job is blocking them.
    require(elapsed < std::chrono::seconds{1}, "control calls are not delayed by a running job");
}

void a_registry_cancels_and_joins_what_it_owns() {
    // A job outliving its registry would report into a destroyed sink.
    RecordedEvents recorded;
    std::atomic_bool observed_cancellation{false};
    {
        engine::JobRegistry jobs{recorded.sink()};
        static_cast<void>(
            jobs.submit("forever", [&](const core::CancellationToken& token, const auto&) {
                while (!token.is_cancellation_requested()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
                observed_cancellation.store(true);
                return protocol::Json{};
            }));
    }
    require(observed_cancellation.load(), "destroying the registry cancels what it owns");
}

// A job that finished before submit() had listed it retired first, found
// nothing, and was then listed -- as running, for good, cancellable forever.
void a_finished_job_is_not_listed() {
    RecordedEvents events;
    engine::JobRegistry registry{events.sink()};
    std::vector<core::StableId> submitted;
    for (int index = 0; index < 200; ++index) {
        submitted.push_back(registry.submit(
            "instant", [](const core::CancellationToken&, const engine::JobRegistry::Reporter&) {
                return protocol::Json{};
            }));
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (events.count("job.finished") < submitted.size() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    require(events.count("job.finished") == submitted.size(), "every job finishes");
    for (const auto& job_id : submitted) {
        require(!registry.cancel(job_id), "and a finished job is not still listed as running");
    }
}

// A sink is usually a socket. One that is slow must not hold up job.cancel,
// which the engine answers ahead of everything else precisely so that it is
// never kept waiting.
void a_slow_sink_does_not_hold_cancel() {
    std::atomic_bool stalling{false};
    std::atomic_bool release{false};
    engine::JobRegistry registry{[&](const protocol::Event& event) {
        if (event.name == "job.progress") {
            stalling.store(true);
            while (!release.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }
    }};
    const auto waiting = registry.submit(
        "waiting", [](const core::CancellationToken& token, const engine::JobRegistry::Reporter&) {
            while (!token.is_cancellation_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            return protocol::Json{};
        });
    (void)registry.submit("reporting",
                          [](const core::CancellationToken&,
                             const engine::JobRegistry::Reporter& report) {
                              report(protocol::Json{{"done", 1}});
                              return protocol::Json{};
                          });
    while (!stalling.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const auto started = std::chrono::steady_clock::now();
    require(registry.cancel(waiting), "a running job can be cancelled");
    require(std::chrono::steady_clock::now() - started < std::chrono::milliseconds{500},
            "while the sink is stuck on another job's event");
    release.store(true);
}

// The last thing a job does is publish job.finished. A job whose thread was
// detached could do that after the registry -- and its sink -- had gone.
void a_job_finishes_before_its_registry_goes() {
    std::atomic<int> finished{0};
    for (int round = 0; round < 10; ++round) {
        std::atomic_bool retired{false};
        {
            // A sink that takes a moment, as a socket may: the job has
            // already left the running list when it gets here.
            engine::JobRegistry registry{[&](const protocol::Event& event) {
                if (event.name == "job.finished") {
                    retired.store(true);
                    std::this_thread::sleep_for(std::chrono::milliseconds{20});
                    finished.fetch_add(1);
                }
            }};
            (void)registry.submit(
                "instant", [](const core::CancellationToken&, const engine::JobRegistry::Reporter&) {
                    return protocol::Json{};
                });
            while (!retired.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }
        require(finished.load() == round + 1,
                "a job's last event is published before its registry is destroyed");
    }
}

} // namespace

int main() {
    a_job_reports_progress_and_finishes();
    cancelling_is_a_request_not_a_guarantee();
    a_long_job_does_not_starve_the_control_path();
    a_registry_cancels_and_joins_what_it_owns();
    a_finished_job_is_not_listed();
    a_slow_sink_does_not_hold_cancel();
    a_job_finishes_before_its_registry_goes();
    std::cout << "job registry: 7 scenarios\n";
    return EXIT_SUCCESS;
}
