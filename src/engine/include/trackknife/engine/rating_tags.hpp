// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "trackknife/protocol/dispatch.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace trackknife::engine {

class Workspace;

// ADR-0237 stage 2: ratings written into the files they rate, when the
// engine is asked to -- an option, off until turned on. The engine's database
// stays where ratings live and every client reads them there; this is a copy
// in the files for other players, as FMPS_RATING (the freedesktop rating:
// 0.0-1.0, the 0-10 rating over ten). Unrated removes it. Album ratings stay
// in the engine only: no player reads one from tags.
//
// Writes go through the tagger's own plan and commit, journaled in the
// engine's database and re-read into the library, one file at a time on a
// thread of their own: rating is never held up by a file being written.
class RatingTags final {
  public:
    RatingTags(std::filesystem::path database, LocalCatalogue& catalogue, Workspace& workspace);
    RatingTags(const RatingTags&) = delete;
    RatingTags& operator=(const RatingTags&) = delete;
    ~RatingTags();

    [[nodiscard]] bool enabled() const;
    // Kept across restarts. Turned on, every rated track is written once;
    // turned off, what is in the files stays there.
    [[nodiscard]] core::Result<void> set_enabled(bool enabled);

    // A rating was stored: its files follow, when this is on.
    void rated(const std::string& hash, bool album, unsigned rating);

    // Files still to write, and waiting until there are none (for tests).
    [[nodiscard]] std::size_t pending() const;
    void wait_idle();

    // One file given `rating` (0 removes it). False when it already had it,
    // or cannot carry tags.
    [[nodiscard]] static core::Result<bool> write(const std::filesystem::path& database,
                                                  LocalCatalogue& catalogue,
                                                  const std::string& raw_path, unsigned rating,
                                                  const core::CancellationToken& cancellation = {});

  private:
    struct Work {
        // Nothing: every rated track.
        std::optional<std::string> hash;
        unsigned rating{0};
    };
    void run();

    std::filesystem::path database_;
    LocalCatalogue& catalogue_;
    Workspace& workspace_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Work> queue_;
    std::size_t in_flight_{0U};
    bool enabled_{false};
    bool stopping_{false};
    core::CancellationSource cancellation_;
    std::thread worker_;
};

// ratings.tags answers {write_tags, pending}; ratings.set_tags {write_tags}
// turns writing on or off and answers the same.
void register_rating_tag_methods(protocol::Dispatcher& dispatcher, RatingTags& tags);

} // namespace trackknife::engine
