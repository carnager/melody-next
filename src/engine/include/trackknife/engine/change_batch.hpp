// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/job_registry.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace trackknife::engine {

// ADR-0315: what changed in the catalogue, told to the clients as one
// `catalogue.changed` once it has been still for a moment -- not one for
// each file a write re-read, each of which made a window reload its library.
// At most `longest` after the first change of a batch, so a long write is
// still seen to go on.
class CatalogueChangeBatch final {
  public:
    explicit CatalogueChangeBatch(EventSink sink,
                                  std::chrono::milliseconds quiet = std::chrono::milliseconds{300},
                                  std::chrono::milliseconds longest = std::chrono::seconds{1});
    ~CatalogueChangeBatch();
    CatalogueChangeBatch(const CatalogueChangeBatch&) = delete;
    CatalogueChangeBatch& operator=(const CatalogueChangeBatch&) = delete;

    // Added to the batch: its paths and albums once each.
    void add(const LocalCatalogue::Change& change);

  private:
    void run(const std::stop_token& stop);

    EventSink sink_;
    std::chrono::milliseconds quiet_;
    std::chrono::milliseconds longest_;
    std::mutex mutex_;
    std::condition_variable_any woken_;
    // The batch, its paths in the order first told.
    std::vector<std::string> paths_;
    std::set<std::string> seen_paths_;
    std::set<std::string> albums_;
    bool everything_{false};
    std::optional<std::chrono::steady_clock::time_point> first_;
    std::chrono::steady_clock::time_point last_;
    std::jthread thread_;
};

} // namespace trackknife::engine
