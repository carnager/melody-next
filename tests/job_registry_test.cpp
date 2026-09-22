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

} // namespace

int main() {
    a_job_reports_progress_and_finishes();
    cancelling_is_a_request_not_a_guarantee();
    a_long_job_does_not_starve_the_control_path();
    a_registry_cancels_and_joins_what_it_owns();
    std::cout << "job registry: 4 scenarios\n";
    return EXIT_SUCCESS;
}
