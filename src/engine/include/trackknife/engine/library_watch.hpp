// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/engine/catalogue.hpp"

#include <thread>

namespace trackknife::engine {

// ADR-0322: the library's folders watched where the engine is, as MPD's
// auto_update does. A file written, added, moved or deleted there is re-read
// -- that file, when its size or modification time changed -- rather than
// the library walked. What changes on another machine (a NAS's own writes
// under a share mounted here) never reaches this machine's inotify:
// melody-watch, run there, tells of that.
class LibraryWatch final {
  public:
    explicit LibraryWatch(LocalCatalogue& catalogue) : catalogue_(catalogue) {}
    LibraryWatch(const LibraryWatch&) = delete;
    LibraryWatch(LibraryWatch&&) = delete;
    LibraryWatch& operator=(const LibraryWatch&) = delete;
    LibraryWatch& operator=(LibraryWatch&&) = delete;
    ~LibraryWatch() { stop(); }

    void start();
    void stop();

  private:
    void run(const core::CancellationToken& token);

    LocalCatalogue& catalogue_;
    core::CancellationSource stopped_;
    std::thread thread_;
};

} // namespace trackknife::engine
