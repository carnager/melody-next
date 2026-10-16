// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace trackknife::cli {

// ADR-0286: melody-cli config -- melodyd.conf read and changed on this
// machine, checked as melodyd would before it is written. `words` follow
// "config". Returns the exit status.
[[nodiscard]] int config_command(const std::vector<std::string>& words);

} // namespace trackknife::cli
