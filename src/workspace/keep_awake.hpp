// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>

#include <memory>

namespace trackknife::bench {

// ADR-0277: while a long file job runs -- tags written, gains measured,
// albums converted -- the computer does not go to sleep on its own, which
// would stop the job, the engine on another machine left waiting for parts
// that never come. A hold is taken for the job and let go when it ends;
// while any is held, idle sleep is held off: an IOKit power assertion on
// macOS, a logind idle inhibitor on Linux. The person can still put the
// computer to sleep, and closing a laptop's lid still does.
class KeepAwake final {
  public:
    using Hold = std::shared_ptr<void>;
    // `reason` is what the system shows for the hold, where it shows one.
    [[nodiscard]] static Hold hold(const QString& reason);
    // How many holds there are: for tests.
    [[nodiscard]] static int holds();
};

} // namespace trackknife::bench
