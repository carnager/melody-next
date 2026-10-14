// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "trackknife/formats/decoder.hpp"
#include "trackknife/metadata/document.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <string_view>

namespace trackknife::lists {

enum class EditKind { sort, reverse, remove_duplicates, shuffle_albums };

struct Entry {
    std::string raw_path;
    std::optional<std::string> logical_reference;
    formats::AudioSourceSelection selection;
    std::optional<formats::SampleRange> segment;
    metadata::MetadataDocument metadata;
    // Cached title, artist, album, album artist, date, and track number fallbacks.
    std::array<std::string, 6> display;
};

struct EditRequest {
    EditKind kind{EditKind::sort};
    std::string expression;
    bool descending{false};
    std::uint32_t seed{0};
};

struct EditPlan {
    // New row -> old row for ordering; ascending old positions for removal.
    std::vector<int> positions;
    bool removal{false};
};

// Digit runs compared as numbers, the rest byte by byte: the order of every
// list sort. Keys are lowercased by the caller.
[[nodiscard]] bool natural_less(std::string_view left, std::string_view right);

[[nodiscard]] core::Result<EditPlan> plan_reverse(std::size_t rows,
                                                  const core::CancellationToken& cancellation = {});

// Deterministic, read-only planning over a detached cached snapshot. No I/O.
// Equal sort keys remain in original order in both directions. Duplicate
// identity includes exact raw path, logical reference, decoder selection/range.
[[nodiscard]] core::Result<EditPlan> plan_edit(std::span<const Entry> entries,
                                               const EditRequest& request,
                                               const core::CancellationToken& cancellation = {},
                                               std::atomic<std::size_t>* progress = nullptr);

} // namespace trackknife::lists
