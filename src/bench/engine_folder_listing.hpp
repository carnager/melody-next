// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::bench {

// ADR-0237: a folder on an engine's machine, as it lists it: where it is,
// its parent, and the folders in it.
struct EngineFolderListing {
    std::string path;
    std::optional<std::string> parent;
    std::vector<std::string> folders;
};
using EngineFolderListingCompletion = std::function<void(core::Result<EngineFolderListing>)>;
// Lists a folder (empty: where the engine starts) and answers on the asker's
// thread.
using EngineFolderLister = std::function<void(std::string, EngineFolderListingCompletion)>;

} // namespace trackknife::bench
