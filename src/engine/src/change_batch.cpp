// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/change_batch.hpp"

#include "trackknife/engine/catalogue_methods.hpp"

#include <utility>

namespace trackknife::engine {

CatalogueChangeBatch::CatalogueChangeBatch(EventSink sink, const std::chrono::milliseconds quiet,
                                           const std::chrono::milliseconds longest)
    : sink_(std::move(sink)), quiet_(quiet), longest_(longest),
      thread_([this](const std::stop_token& stop) { run(stop); }) {}

CatalogueChangeBatch::~CatalogueChangeBatch() {
    thread_.request_stop();
    woken_.notify_all();
}

void CatalogueChangeBatch::add(const LocalCatalogue::Change& change) {
    {
        const std::scoped_lock held{mutex_};
        for (const auto& path : change.paths) {
            if (seen_paths_.insert(path).second) {
                paths_.push_back(path);
            }
        }
        albums_.insert(change.albums.begin(), change.albums.end());
        everything_ = everything_ || change.everything;
        const auto now = std::chrono::steady_clock::now();
        if (!first_) {
            first_ = now;
        }
        last_ = now;
    }
    woken_.notify_all();
}

void CatalogueChangeBatch::run(const std::stop_token& stop) {
    std::unique_lock held{mutex_};
    while (!stop.stop_requested()) {
        if (!first_) {
            woken_.wait(held, stop, [this] { return first_.has_value(); });
            continue;
        }
        // Still for `quiet_`, or going on for `longest_`: told.
        const auto due = std::min(last_ + quiet_, *first_ + longest_);
        if (std::chrono::steady_clock::now() < due) {
            woken_.wait_until(held, stop, due, [] { return false; });
            continue;
        }
        LocalCatalogue::Change batch{.paths = std::exchange(paths_, {}),
                                     .albums = {albums_.begin(), albums_.end()},
                                     .everything = std::exchange(everything_, false)};
        seen_paths_.clear();
        albums_.clear();
        first_.reset();
        held.unlock();
        sink_(catalogue_change_event(batch));
        held.lock();
    }
}

} // namespace trackknife::engine
