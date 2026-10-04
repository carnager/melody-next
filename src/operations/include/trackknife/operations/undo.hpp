// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/error.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/operations/file_publication.hpp"
#include "trackknife/operations/file_publication_journal.hpp"
#include "trackknife/operations/metadata_commit.hpp"
#include "trackknife/operations/metadata_journal.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace trackknife::operations {

// ADR-0263: one write to undo, as its journal names it -- a tag write in
// place, or a publication (moved, renamed, written at a new path).
enum class UndoKind : std::uint8_t { metadata, publication };

struct UndoRequest {
    UndoKind kind{UndoKind::metadata};
    core::StableId journal_id;

    friend bool operator==(const UndoRequest&, const UndoRequest&) = default;
};

// How one went: the file back at `to_raw_path` (from `from_raw_path`, the
// same for a tag write), or why it was not undone.
struct UndoOutcome {
    UndoRequest request;
    std::optional<core::Error> issue;
    std::string from_raw_path;
    std::string to_raw_path;

    friend bool operator==(const UndoOutcome&, const UndoOutcome&) = default;
};

using UndoProgressCallback = std::function<void(std::size_t completed, std::size_t total)>;

// Each undone in the order given -- newest first, as a batch is unwound --
// one refused not stopping the rest. Fails only when cancelled.
[[nodiscard]] core::Result<std::vector<UndoOutcome>>
undo_operations(std::span<const UndoRequest> requests, MetadataOperationJournal& metadata_journal,
                FilePublicationJournal& publication_journal,
                const MetadataDependentStateCommitter& metadata_dependent,
                const FilePublicationDependentStateCommitter& publication_dependent,
                const UndoProgressCallback& progress = {},
                const core::CancellationToken& cancellation = {});

} // namespace trackknife::operations
