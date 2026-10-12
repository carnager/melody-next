// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdlib>

namespace trackknife::core {

// A variable of the process's environment, or nullptr. getenv is unsafe only
// against a concurrent setenv, and nothing here changes the environment once
// threads run -- the window sets its scale factor before Qt starts -- so
// reading it is safe from any thread.
[[nodiscard]] inline const char* environment_variable(const char* name) noexcept {
    return std::getenv(name); // NOLINT(concurrency-mt-unsafe): see above
}

} // namespace trackknife::core
