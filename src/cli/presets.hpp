// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace trackknife::cli {

// One of Melody's naming layouts, as melody-cli needs it: its name and the
// two expressions a conversion is named by.
struct NamingLayout final {
    std::string name;
    std::string directory;
    std::string basename;
};

// ADR-0339: the naming layouts in Melody's presets database at `path`, by
// name -- read, never written; Melody checks them when it saves them. Read
// here with SQLite alone, so that melody-cli builds without the media
// libraries the rest of persistence needs. Not found when there is no such
// database.
[[nodiscard]] core::Result<std::vector<NamingLayout>>
read_naming_layouts(const std::filesystem::path& path);

} // namespace trackknife::cli
