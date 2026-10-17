// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/job_registry.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace trackknife::engine {

// Albums that came into the library, said once each to every client as
// catalogue.albums_added {albums: [{key, artist, album, date, folder,
// paths}]} -- keys and paths encoded -- once whole: when no track of it has
// come for `settle`, so one still being copied in is not said half. An album
// is new when every track of it came after the engine started -- one copied
// in while it was stopped is found by the scan that follows -- and one that
// only grew is not. Looked for when the catalogue says something changed,
// on a thread of its own.
class AlbumArrivals final {
  public:
    AlbumArrivals(LocalCatalogue& catalogue, EventSink sink,
                  std::chrono::seconds settle = std::chrono::seconds{30});
    ~AlbumArrivals();
    AlbumArrivals(const AlbumArrivals&) = delete;
    AlbumArrivals& operator=(const AlbumArrivals&) = delete;

    // Something came into the catalogue, or changed in it: look again.
    void changed();

  private:
    void run(const std::stop_token& stop);
    // The albums of these keys, said.
    void announce(const std::vector<std::string>& keys);

    LocalCatalogue& catalogue_;
    EventSink sink_;
    std::chrono::seconds settle_;
    std::int64_t since_;
    std::set<std::string> announced_;
    std::mutex mutex_;
    std::condition_variable_any woken_;
    bool look_{true};
    std::jthread thread_;
};

} // namespace trackknife::engine
