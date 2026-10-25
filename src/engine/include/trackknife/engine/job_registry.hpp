// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/protocol/message.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace trackknife::engine {

// Where the engine writes unsolicited messages. Whoever owns the socket
// supplies one. It is called from job threads, so an implementation that
// touches a non-thread-safe transport must marshal.
using EventSink = std::function<void(const protocol::Event&)>;

// ADR-0327: an event with something only one connection is to hear -- a job's
// outcome, for the client that started it. Every client is given `everyone`;
// the connection `recipient` names is given `for_recipient` instead, as it
// would be an answer to its own request.
using AddressedSink = std::function<void(const protocol::Event& everyone, std::uint64_t recipient,
                                         const protocol::Event& for_recipient)>;

// ADR-0327: the connection whose request this thread is handling, or 0. A
// server sets it around each request it dispatches; what answers later, on
// its own -- a job's thread -- takes it with it.
[[nodiscard]] std::uint64_t current_requester() noexcept;
class RequesterScope final {
  public:
    explicit RequesterScope(std::uint64_t connection) noexcept;
    ~RequesterScope();
    RequesterScope(const RequesterScope&) = delete;
    RequesterScope& operator=(const RequesterScope&) = delete;
    RequesterScope(RequesterScope&&) = delete;
    RequesterScope& operator=(RequesterScope&&) = delete;

  private:
    std::uint64_t previous_;
};

// ADR-0220 and ADR-0222: a long operation is a job, not a slow handler. It is
// submitted, answers immediately with an identity, reports through events, and
// can be cancelled -- so a scan can never occupy the control path while it
// runs. That is the direct answer to ADR-0219, where a cover scan starved
// everything else.
//
// The engine owns the thread here, unlike the synchronous calls on Catalogue
// and Workspace where the caller supplies one. The difference is not
// inconsistency: a remote client has no thread to lend, and the engine has to
// keep answering while the job runs.
class JobRegistry final {
  public:
    // A job reports progress by calling this with whatever it wants the
    // client to see. The registry stamps the job identity and emits it.
    using Reporter = std::function<void(protocol::Json progress)>;
    // Returns the outcome document reported by job.finished.
    using Work = std::function<protocol::Json(const core::CancellationToken&, const Reporter&)>;

    // `addressed`, where there is one, carries a job's outcome to the client
    // that started it only (ADR-0327); without it every client hears it.
    explicit JobRegistry(EventSink sink, AddressedSink addressed = {});
    JobRegistry(const JobRegistry&) = delete;
    JobRegistry(JobRegistry&&) = delete;
    JobRegistry& operator=(const JobRegistry&) = delete;
    JobRegistry& operator=(JobRegistry&&) = delete;
    // Cancels everything still running and waits for it. A job outliving the
    // registry would report into a destroyed sink.
    ~JobRegistry();

    // Starts the work on its own thread and answers at once.
    [[nodiscard]] core::StableId submit(std::string name, Work work);

    // Asks a job to stop. Answers whether the identity was known, not whether
    // the job stopped: cancelling is a request, and a job that completes
    // before the request arrives completes.
    [[nodiscard]] bool cancel(const core::StableId& job_id);

    // Blocks until nothing is running. For tests and for shutdown; the
    // protocol itself never waits.
    void wait_all();

  private:
    struct Running {
        core::CancellationSource cancellation;
        std::thread worker;
    };

    // Not named emit: Qt defines that as a macro, and these headers must
    // stay includable from Qt code even though they use none of it.
    void publish(const protocol::Event& event);
    void publish_finished(protocol::Json data, protocol::Json outcome, std::uint64_t owner);
    void retire(const core::StableId& job_id);
    // Joins the threads of jobs that have retired. Their threads are kept,
    // not detached: a detached one still publishing job.finished could
    // outlive the registry.
    void reap();

    EventSink sink_;
    AddressedSink addressed_;
    // Serialises the sink on its own, so a slow sink never holds up submit
    // or cancel -- those take mutex_ only.
    std::mutex sink_mutex_;
    std::mutex mutex_;
    std::map<core::StableId, Running> running_;
    std::vector<std::thread> retired_;
};

} // namespace trackknife::engine
