// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/audio/playback_order.hpp"
#include <algorithm>
#include <numeric>

namespace trackknife::audio {

PlaybackOrder::PlaybackOrder(const std::uint32_t seed) : generator_(seed) {}

void PlaybackOrder::startCycle() {
    excluded_ = current_;
    remaining_ = count_ - 1;
    swaps_.clear();
    history_ = {current_};
    cursor_ = 0U;
    pending_.reset();
    pending_wrap_ = false;
}

void PlaybackOrder::reset(const int count, const int current, const bool random) {
    albums_.clear();
    album_cycle_.clear();
    next_album_cycle_.clear();
    count_ = count;
    current_ = current;
    random_ = random;
    startCycle();
}

std::vector<int> PlaybackOrder::albumCycle(const bool initial) {
    std::vector<std::size_t> groups(albums_.size());
    std::iota(groups.begin(), groups.end(), 0U);
    std::shuffle(groups.begin(), groups.end(), generator_);
    const auto active = std::ranges::find_if(groups, [&](const auto group) {
        return std::ranges::find(albums_[group], current_) != albums_[group].end();
    });
    if (active != groups.end()) {
        if (initial)
            std::iter_swap(groups.begin(), active);
        else if (active == groups.begin() && groups.size() > 1U)
            std::iter_swap(groups.begin(), groups.begin() + 1);
    }
    std::vector<int> cycle;
    for (const auto group : groups)
        cycle.insert(cycle.end(), albums_[group].begin(), albums_[group].end());
    return cycle;
}

void PlaybackOrder::resetAlbums(std::vector<std::vector<int>> groups, const int current) {
    reset(0, current, false);
    albums_ = std::move(groups);
    album_cycle_ = albumCycle(true);
    const auto found = std::ranges::find(album_cycle_, current);
    album_cursor_ = static_cast<std::size_t>(found - album_cycle_.begin());
}

int PlaybackOrder::draw() {
    const auto slot = std::uniform_int_distribution<int>{0, remaining_ - 1}(generator_);
    const auto value = [&](const int index) {
        const auto found = swaps_.find(index);
        return found == swaps_.end() ? index : found->second;
    };
    const auto result = value(slot);
    --remaining_;
    if (slot != remaining_) {
        swaps_[slot] = value(remaining_);
    }
    swaps_.erase(remaining_);
    return result >= excluded_ ? result + 1 : result;
}

std::optional<int> PlaybackOrder::adjacent(const int direction, const bool repeat) {
    if (!albums_.empty()) {
        if (album_cursor_ >= album_cycle_.size())
            return std::nullopt;
        if (direction < 0)
            return album_cursor_ > 0 ? std::optional{album_cycle_[album_cursor_ - 1]}
                                     : std::nullopt;
        if (album_cursor_ + 1 < album_cycle_.size())
            return album_cycle_[album_cursor_ + 1];
        if (!repeat)
            return std::nullopt;
        if (next_album_cycle_.empty())
            next_album_cycle_ = albumCycle(false);
        return next_album_cycle_.empty() ? std::nullopt : std::optional{next_album_cycle_.front()};
    }
    if (count_ <= 0 || current_ < 0 || current_ >= count_) {
        return std::nullopt;
    }
    if (!random_) {
        const auto row = current_ + direction;
        if (row >= 0 && row < count_) {
            return row;
        }
        return repeat ? std::optional{direction < 0 ? count_ - 1 : 0} : std::nullopt;
    }
    if (direction < 0) {
        return cursor_ > 0U ? std::optional{history_[cursor_ - 1U]} : std::nullopt;
    }
    if (cursor_ + 1U < history_.size()) {
        return history_[cursor_ + 1U];
    }
    if (pending_) {
        return pending_wrap_ && !repeat ? std::nullopt : pending_;
    }
    if (remaining_ <= 0) {
        if (!repeat) {
            return std::nullopt;
        }
        // Retain the completed cycle until this candidate is committed, so a
        // status refresh never erases Previous's history.
        pending_wrap_ = true;
        pending_ =
            count_ == 1
                ? current_
                : static_cast<int>((static_cast<std::int64_t>(current_) +
                                    std::uniform_int_distribution<int>{1, count_ - 1}(generator_)) %
                                   count_);
    } else {
        pending_ = draw();
    }
    return pending_;
}

void PlaybackOrder::advance(const int row, const int direction) {
    if (!albums_.empty()) {
        if (direction < 0 && album_cursor_ > 0 && album_cycle_[album_cursor_ - 1] == row)
            --album_cursor_;
        else if (album_cursor_ + 1 < album_cycle_.size() && album_cycle_[album_cursor_ + 1] == row)
            ++album_cursor_;
        else if (!next_album_cycle_.empty() && next_album_cycle_.front() == row) {
            album_cycle_ = std::move(next_album_cycle_);
            next_album_cycle_.clear();
            album_cursor_ = 0;
        } else if (row != current_) {
            auto groups = std::move(albums_);
            resetAlbums(std::move(groups), row);
        }
        current_ = row;
        return;
    }
    if (row == current_) {
        return;
    }
    if (!random_) {
        current_ = row;
        return;
    }
    if (direction < 0 && cursor_ > 0U && history_[cursor_ - 1U] == row) {
        --cursor_;
    } else if (cursor_ + 1U < history_.size() && history_[cursor_ + 1U] == row) {
        ++cursor_;
    } else if (pending_ == row) {
        if (history_.size() == static_cast<std::size_t>(count_)) {
            current_ = row;
            startCycle();
            return;
        }
        history_.push_back(row);
        ++cursor_;
        pending_.reset();
    } else {
        reset(count_, row, random_);
        return;
    }
    current_ = row;
}

} // namespace trackknife::audio
