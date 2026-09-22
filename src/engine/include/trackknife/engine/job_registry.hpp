// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/protocol/message.hpp"

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace trackknife::engine {

// Where the engine writes unsolicited messages. Whoever owns the socket
// supplies one. It is called from job threads, so an implementation that
// touches a non-thread-safe transport must marshal.
using EventSink = std::function<void(const protocol::Event&)>;

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

    explicit JobRegistry(EventSink sink);
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

    void emit(const protocol::Event& event);
    void retire(const core::StableId& job_id);

    EventSink sink_;
    std::mutex mutex_;
    std::map<core::StableId, Running> running_;
};

} // namespace trackknife::engine
