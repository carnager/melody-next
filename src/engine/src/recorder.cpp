// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/recorder.hpp"

#include "trackknife/persistence/list_repository.hpp"

#include <utility>

namespace trackknife::engine {
namespace {

// The listening store keys on a ListItem, so an observation is turned back
// into one. Only the fields the identity is derived from matter.
[[nodiscard]] persistence::ListItem as_item(const audio::TrackSource& source) {
    persistence::ListItem item;
    item.source = persistence::ListSource::local;
    item.source_reference = source.raw_path;
    if (source.selection.stream_index || source.selection.subsong_index) {
        item.source_selection = persistence::ListItemSourceSelection{
            source.selection.stream_index, source.selection.subsong_index};
    }
    if (source.segment) {
        item.segment =
            persistence::ListItemSegment{source.segment->start_sample, source.segment->end_sample};
    }
    return item;
}

} // namespace

Recorder::Recorder(Player& player, Workspace& workspace, const std::chrono::milliseconds interval)
    : player_(&player), workspace_(&workspace), interval_(interval) {}

Recorder::~Recorder() { stop(); }

void Recorder::drain(const std::int64_t monotonic_ms, const std::int64_t wall_ms) {
    const auto observations = player_->observe(monotonic_ms);

    if (observations.listened_entry) {
        auto item = as_item(observations.listened_source);
        // A listen that cannot be attributed is dropped rather than recorded
        // against nothing: the store refuses an unqualified source, and
        // forcing one would invent a play count for a file it cannot name.
        static_cast<void>(workspace_->record_local_listen(item, core::StableId::random(), wall_ms));
    }

    if (observations.resume_entry) {
        auto item = as_item(observations.resume_source);
        if (auto key = workspace_->local_listening_key(item)) {
            static_cast<void>(
                workspace_->save_local_resume(*key, observations.resume_position_ms, wall_ms));
        }
    }
}

void Recorder::start() {
    if (running_.exchange(true)) {
        return;
    }
    worker_ = std::thread{[this] {
        // Monotonic for accounting, wall for timestamps: crediting listening
        // from wall time would gain or lose a track's worth whenever the
        // clock is adjusted.
        const auto started = std::chrono::steady_clock::now();
        while (running_.load()) {
            const auto monotonic = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - started)
                                       .count();
            const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
            drain(monotonic, wall);
            std::this_thread::sleep_for(interval_);
        }
    }};
}

void Recorder::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

} // namespace trackknife::engine
