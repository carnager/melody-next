// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace trackknife::bench {

// ADR-0237: a folder on an engine's machine, as it lists it: where it is,
// its parent, and the folders in it -- those that are links said apart --
// with where browsing starts there: its home and its music folders.
struct EngineFolderListing {
    std::string path;
    std::optional<std::string> parent;
    std::vector<std::string> folders;
    std::vector<std::string> linked;
    std::string home;
    std::vector<std::string> roots;
};
using EngineFolderListingCompletion = std::function<void(core::Result<EngineFolderListing>)>;
// A folder made: its path.
using EngineFolderMadeCompletion = std::function<void(core::Result<std::string>)>;

// Lists a folder (empty: where the engine starts) and answers on the asker's
// thread; and, where it can, makes one.
struct EngineFolderLister {
    std::function<void(std::string, EngineFolderListingCompletion)> list;
    // A folder `name` made in a folder; empty, none can be made.
    std::function<void(std::string, std::string, EngineFolderMadeCompletion)> make;

    EngineFolderLister() = default;
    // A lister alone, as a function.
    template <typename List>
        requires(!std::is_same_v<std::decay_t<List>, EngineFolderLister> &&
                 std::is_invocable_v<List&, std::string, EngineFolderListingCompletion>)
    EngineFolderLister(List lister) // NOLINT(google-explicit-constructor)
        : list(std::move(lister)) {}

    explicit operator bool() const { return static_cast<bool>(list); }
    void operator()(std::string path, EngineFolderListingCompletion done) const {
        list(std::move(path), std::move(done));
    }
};

} // namespace trackknife::bench
