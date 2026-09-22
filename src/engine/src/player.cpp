// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/player.hpp"

#include <algorithm>
#include <utility>

namespace trackknife::engine {

// The engine's queue, seen the way the advance rules need it.
class Player::QueueView final : public audio::PlaybackList {
  public:
    explicit QueueView(const std::vector<QueueEntry>& entries) : entries_(&entries) {}

    [[nodiscard]] int row_count() const override { return static_cast<int>(entries_->size()); }
    [[nodiscard]] int row_of_entry(const core::StableId& entry, const int hint_row) const override {
        if (entry.is_nil()) {
            return -1;
        }
        if (hint_row >= 0 && hint_row < row_count() &&
            (*entries_)[static_cast<std::size_t>(hint_row)].entry_id == entry) {
            return hint_row;
        }
        const auto found = std::ranges::find_if(*entries_, [&entry](const QueueEntry& candidate) {
            return candidate.entry_id == entry;
        });
        return found == entries_->end() ? -1
                                        : static_cast<int>(std::distance(entries_->begin(), found));
    }
    [[nodiscard]] audio::TrackSource source_at(const int row) const override {
        return (*entries_)[static_cast<std::size_t>(row)].source;
    }

  private:
    const std::vector<QueueEntry>* entries_;
};

Player::Player(std::unique_ptr<audio::LocalAuditionService> audition)
    : audition_(std::move(audition)) {}

Player::~Player() = default;

core::Result<std::unique_ptr<Player>> Player::create() {
    auto audition = audio::LocalAuditionService::create();
    if (!audition) {
        return std::unexpected(std::move(audition.error()));
    }
    return std::unique_ptr<Player>{new Player{std::move(*audition)}};
}

void Player::reset_order_locked() {
    const QueueView view{queue_};
    row_ = view.row_of_entry(anchors_.current, row_);
    order_.reset(view.row_count(), row_, modes_.random);
}

core::Result<void> Player::start_locked(const std::size_t row) {
    const auto& entry = queue_[row];
    // A segment is optional, and the audition service spells the two cases as
    // separate calls rather than an optional parameter.
    auto started =
        entry.source.segment
            ? audition_->load_selected_segment_and_play(
                  entry.source.raw_path, entry.source.selection, *entry.source.segment)
            : audition_->load_selected_and_play(entry.source.raw_path, entry.source.selection);
    if (!started) {
        return std::unexpected(std::move(started.error()));
    }
    anchors_.current = entry.entry_id;
    anchors_.source = entry.source;
    row_ = static_cast<int>(row);
    order_.advance(row_, 1);
    return {};
}

void Player::replace_queue(std::vector<QueueEntry> entries) {
    const std::lock_guard guard{mutex_};
    queue_ = std::move(entries);
    // ADR-0221: the playing entry is followed by identity. If it has gone,
    // playback is not silently handed to whatever now sits at its old row.
    const QueueView view{queue_};
    row_ = view.row_of_entry(anchors_.current, -1);
    if (row_ < 0 && !anchors_.current.is_nil()) {
        anchors_.current = core::StableId{};
    }
    reset_order_locked();
}

std::vector<QueueEntry> Player::queue() const {
    const std::lock_guard guard{mutex_};
    return queue_;
}

core::Result<void> Player::play_entry(const core::StableId& entry_id) {
    const std::lock_guard guard{mutex_};
    const QueueView view{queue_};
    const auto row = view.row_of_entry(entry_id, row_);
    if (row < 0) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::not_found,
                        .message = "no such entry in the queue",
                        .context = {{.key = "entry", .value = entry_id.to_string()}}});
    }
    return start_locked(static_cast<std::size_t>(row));
}

core::Result<void> Player::resume() {
    const std::lock_guard guard{mutex_};
    return audition_->play();
}

core::Result<void> Player::pause() {
    const std::lock_guard guard{mutex_};
    return audition_->pause();
}

core::Result<void> Player::stop() {
    const std::lock_guard guard{mutex_};
    auto stopped = audition_->stop();
    anchors_.current = core::StableId{};
    anchors_.source = {};
    row_ = -1;
    return stopped;
}

core::Result<void> Player::seek_ms(const std::int64_t position_ms) {
    const std::lock_guard guard{mutex_};
    if (position_ms < 0) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "a position cannot be negative",
                                           .context = {}});
    }
    return audition_->seek_to_seconds(static_cast<double>(position_ms) / 1000.0);
}

core::Result<void> Player::step(const int direction) {
    const std::lock_guard guard{mutex_};
    const QueueView view{queue_};
    const auto choice =
        audio::adjacent_playback_row(view, anchors_, modes_, order_, {}, direction, row_);
    if (!choice) {
        return std::unexpected(core::Error{.code = core::ErrorCode::not_found,
                                           .message = "nothing to play in that direction",
                                           .context = {}});
    }
    return start_locked(static_cast<std::size_t>(choice->row));
}

audio::PlaybackModes Player::modes() const {
    const std::lock_guard guard{mutex_};
    return modes_;
}

void Player::set_modes(audio::PlaybackModes modes) {
    const std::lock_guard guard{mutex_};
    modes_ = modes;
    // Random and album-random change the traversal, so the order is rebuilt
    // rather than left describing the previous mode.
    reset_order_locked();
}

Player::State Player::state() const {
    const std::lock_guard guard{mutex_};
    const auto snapshot = audition_->snapshot();
    State current;
    switch (snapshot.state) {
    case audio::LocalAuditionState::playing:
    case audio::LocalAuditionState::draining:
        current.status = "playing";
        break;
    case audio::LocalAuditionState::paused:
        current.status = "paused";
        break;
    case audio::LocalAuditionState::loading:
    case audio::LocalAuditionState::buffering:
        current.status = "loading";
        break;
    default:
        current.status = "stopped";
        break;
    }
    current.entry = anchors_.current;
    current.source = anchors_.source;
    if (snapshot.format && snapshot.format->sample_rate > 0) {
        const auto rate = static_cast<std::int64_t>(snapshot.format->sample_rate);
        current.position_ms =
            snapshot.position_sample / rate * 1000 + snapshot.position_sample % rate * 1000 / rate;
    }
    if (row_ >= 0 && static_cast<std::size_t>(row_) < queue_.size()) {
        current.duration_ms = queue_[static_cast<std::size_t>(row_)].duration_ms.value_or(-1);
    }
    current.queue_size = queue_.size();
    current.modes = modes_;
    return current;
}

} // namespace trackknife::engine
