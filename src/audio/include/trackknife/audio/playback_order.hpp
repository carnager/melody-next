// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <optional>
#include <random>
#include <unordered_map>
#include <vector>

namespace trackknife::audio {

// Lazy Fisher-Yates traversal over row occurrences. Reset is constant-time for
// a new list; each forward choice uses bounded work without shuffling the UI
// model or allocating one entry per unplayed row. Edits begin a fresh cycle.
class PlaybackOrder final {
  public:
    explicit PlaybackOrder(std::uint32_t seed = std::random_device{}());
    void reset(int count, int current, bool random);
    // Groups contain row occurrences in their existing list order.
    void resetAlbums(std::vector<std::vector<int>> groups, int current);
    [[nodiscard]] std::optional<int> adjacent(int direction, bool repeat);
    void advance(int row, int direction = 1);

  private:
    void startCycle();
    [[nodiscard]] int draw();
    [[nodiscard]] std::vector<int> albumCycle(bool initial);
    std::vector<std::vector<int>> albums_;
    std::vector<int> album_cycle_;
    std::vector<int> next_album_cycle_;
    std::size_t album_cursor_{0};

    std::mt19937 generator_;
    int count_{0};
    int current_{-1};
    bool random_{false};
    int excluded_{-1};
    int remaining_{0};
    std::unordered_map<int, int> swaps_;
    std::vector<int> history_;
    std::size_t cursor_{0U};
    std::optional<int> pending_;
    bool pending_wrap_{false};
};

} // namespace trackknife::audio
