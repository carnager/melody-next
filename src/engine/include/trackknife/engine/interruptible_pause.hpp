// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace trackknife::engine {

// The wait between a sampling worker's ticks, which stopping cuts short. A
// plain sleep made shutdown wait out the longest interval -- five seconds for
// the playback store -- so every stop and restart of the engine did.
class InterruptiblePause final {
  public:
    // Returns false once interrupted; the worker should then finish.
    [[nodiscard]] bool wait(const std::chrono::milliseconds interval) {
        std::unique_lock lock{mutex_};
        return !woken_.wait_for(lock, interval, [this] { return interrupted_; });
    }

    void interrupt() {
        {
            const std::lock_guard lock{mutex_};
            interrupted_ = true;
        }
        woken_.notify_all();
    }

    // Before a worker starts again.
    void reset() {
        const std::lock_guard lock{mutex_};
        interrupted_ = false;
    }

  private:
    std::mutex mutex_;
    std::condition_variable woken_;
    bool interrupted_{false};
};

} // namespace trackknife::engine
