// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <ostream>
#include <string_view>

namespace trackknife::cli {

// melody-cli's help: one line for each command, grouped by what it is for,
// with how to choose the engine -- what `melody-cli`, `--help` and `help`
// print.
void print_overview(std::ostream& out);

// What one command does, its arguments and examples: `help COMMAND` and
// `COMMAND --help`. Every entry the command's name appears in -- `play`
// resumes, plays an album, plays a list. False when no command is called so.
[[nodiscard]] bool print_command_help(std::ostream& out, std::string_view command);

// Every command, a tab and what it does in a few words, one a line: what
// the shell's completion offers.
void print_commands(std::ostream& out);

} // namespace trackknife::cli
