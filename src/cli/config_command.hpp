// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::cli {

// ADR-0286: melody-cli config -- melodyd.conf read and changed on this
// machine, checked as melodyd would before it is written. `words` follow
// "config". Returns the exit status.
[[nodiscard]] int config_command(const std::vector<std::string>& words);

// Every key melodyd.conf takes, as its skeleton has it commented out: the
// key, a tab and the first line said about it -- for the shell's completion.
void print_config_keys(std::ostream& out);

} // namespace trackknife::cli
