// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>

namespace trackknife::core {
// Position deltas are bounded by monotonic wall time; seeks never earn credit.
class ListenAccounting final {
  public:
    bool observe(const std::string& identity, double duration, double position, bool playing,
                 std::int64_t now) {
        changed_ = identity != identity_;
        if (changed_) {
            identity_ = identity;
            listened_ = 0;
            uncredited_ = 0;
            submitted_ = false;
        }
        const double dt = static_cast<double>(now - last_) / 1000.0;
        const double advance = position - position_;
        if (!changed_ && playing && playing_ && dt > 0 && dt <= 5) {
            uncredited_ += dt;
            if (advance != 0) {
                if (advance > 0 && uncredited_ <= 10 && advance <= uncredited_ + 1)
                    listened_ += std::min(uncredited_, advance);
                uncredited_ = 0;
            } else if (uncredited_ > 10) {
                uncredited_ = 0;
            }
        } else {
            uncredited_ = 0;
        }
        last_ = now;
        position_ = position;
        playing_ = playing;
        if (!submitted_ && !identity.empty() && duration > 30 &&
            listened_ >= std::min(duration / 2, 240.0)) {
            submitted_ = true;
            return true;
        }
        return false;
    }
    void reset() { *this = ListenAccounting{}; }
    [[nodiscard]] double listened() const { return listened_; }
    [[nodiscard]] bool changed() const { return changed_; }

  private:
    std::string identity_;
    std::int64_t last_{};
    double position_{}, listened_{}, uncredited_{};
    bool playing_{}, submitted_{}, changed_{};
};
} // namespace trackknife::core
