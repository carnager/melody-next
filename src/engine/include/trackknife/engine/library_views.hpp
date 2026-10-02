// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/persistence/local_library.hpp"

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace trackknife::engine {

// ADR-0254: a library view, grouped. One pass over the library evaluates
// every level for every track; the tree is kept, keyed by the levels, the
// filter and what they read, so opening a node evaluates nothing.
class LibraryViews final {
  public:
    // How many grouped trees are kept: a few views switched between, each
    // with and without a filter.
    static constexpr std::size_t kept = 4U;
    // Branches one track may add across all levels together, $each by $each.
    static constexpr std::size_t maximum_branches = 4'096U;

    // The node at `request.view_path`: its children, or at the depth of the
    // last level its tracks, in library order, paged by offset and limit.
    [[nodiscard]] core::Result<persistence::LibraryPage>
    query(const persistence::LocalLibrary& library, const persistence::LibraryQuery& request,
          const core::CancellationToken& cancellation = {}) const;
    // The available files under that node, in library order.
    [[nodiscard]] core::Result<std::vector<std::string>>
    paths(const persistence::LocalLibrary& library, const persistence::LibraryQuery& request,
          const core::CancellationToken& cancellation = {}) const;

    struct Tree;

  private:
    [[nodiscard]] core::Result<std::shared_ptr<const Tree>>
    tree(const persistence::LocalLibrary& library, const persistence::LibraryQuery& request,
         const core::CancellationToken& cancellation) const;

    mutable std::mutex mutex_;
    mutable std::deque<std::shared_ptr<const Tree>> trees_;
};

} // namespace trackknife::engine
