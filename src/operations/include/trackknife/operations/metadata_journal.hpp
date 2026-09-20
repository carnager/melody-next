// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/error.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/metadata/artwork_write_plan.hpp"
#include "trackknife/metadata/staged_patch.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::operations {

enum class MetadataOperationJournalState : std::uint8_t {
    planned,
    prepared,
    published,
    complete,
    rolled_back,
    needs_reconciliation,
};

enum class MetadataOperationContentKind : std::uint8_t {
    text_fields,
    embedded_artwork,
    // ADR-0145: carrier rewrites share the text evidence shape, with the
    // carrier-internal identity encoded in each change's exact_native_name.
    cue_replay_gain,
    loudness_sidecar,
    folder_image,
};

// The file-mutation journal reaches `complete` before its exact old inode can
// be offered as undo. Backup lifecycle is deliberately separate: commit
// recovery never mistakes a retained undo artifact for an incomplete write.
enum class MetadataOperationBackupState : std::uint8_t {
    retained,
    undoing,
    undone,
    released,
    needs_reconciliation,
};

struct MetadataOperationJournalChange {
    std::size_t field_index{0U};
    std::string canonical_name;
    std::string property_name;
    bool original_present{false};
    std::vector<std::string> original_values;
    metadata::StagedMetadataPatchKind kind{metadata::StagedMetadataPatchKind::replace_values};
    std::vector<std::string> planned_values;
    std::vector<std::size_t> item_indexes;
    std::optional<std::string> exact_native_name;

    friend bool operator==(const MetadataOperationJournalChange&,
                           const MetadataOperationJournalChange&) = default;
};

struct MetadataOperationJournalArtwork {
    metadata::ArtworkWritePlanIntentKind kind{metadata::ArtworkWritePlanIntentKind::replace};
    std::size_t target_ordinal{0U};
    std::size_t original_item_count{0U};
    std::size_t planned_item_count{0U};
    // Add has no original target and leaves this empty. Its target ordinal is
    // the resulting append position in the embedded-picture inventory.
    std::optional<core::ContentFingerprint> original_target_fingerprint;
    std::optional<core::ContentFingerprint> replacement_fingerprint;
    core::ContentFingerprint original_inventory_fingerprint;
    core::ContentFingerprint planned_inventory_fingerprint;

    friend bool operator==(const MetadataOperationJournalArtwork&,
                           const MetadataOperationJournalArtwork&) = default;
};

struct MetadataOperationJournalRecord {
    core::StableId id;
    MetadataOperationJournalState state{MetadataOperationJournalState::planned};
    std::string source_raw_path;
    std::string prepared_raw_path;
    std::string backup_raw_path;
    core::LocalSourceRevision expected_revision;
    std::optional<core::LocalSourceRevision> prepared_revision;
    std::optional<core::LocalSourceRevision> published_revision;
    std::vector<std::size_t> occurrence_indexes;
    MetadataOperationContentKind content_kind{MetadataOperationContentKind::text_fields};
    std::vector<MetadataOperationJournalChange> changes;
    std::optional<MetadataOperationJournalArtwork> artwork;
    std::optional<core::Error> failure;

    friend bool operator==(const MetadataOperationJournalRecord&,
                           const MetadataOperationJournalRecord&) = default;
};

struct MetadataOperationJournalTransition {
    MetadataOperationJournalState expected_state{MetadataOperationJournalState::planned};
    MetadataOperationJournalState state{MetadataOperationJournalState::planned};
    std::optional<core::LocalSourceRevision> prepared_revision;
    std::optional<core::LocalSourceRevision> published_revision;
    std::optional<core::Error> failure;
};

struct MetadataOperationBackupRecord {
    MetadataOperationJournalRecord operation;
    MetadataOperationBackupState state{MetadataOperationBackupState::retained};
    std::optional<core::StableId> undo_id;
    std::int64_t completed_at_unix_seconds{0};
    std::int64_t updated_at_unix_seconds{0};
    std::optional<core::Error> failure;

    friend bool operator==(const MetadataOperationBackupRecord&,
                           const MetadataOperationBackupRecord&) = default;
};

struct MetadataOperationBackupTransition {
    MetadataOperationBackupState expected_state{MetadataOperationBackupState::retained};
    MetadataOperationBackupState state{MetadataOperationBackupState::retained};
    std::optional<core::StableId> undo_id;
    std::optional<core::Error> failure;
};

class MetadataOperationJournal {
  public:
    MetadataOperationJournal() = default;
    MetadataOperationJournal(MetadataOperationJournal&&) noexcept = default;
    MetadataOperationJournal& operator=(MetadataOperationJournal&&) noexcept = default;
    MetadataOperationJournal(const MetadataOperationJournal&) = delete;
    MetadataOperationJournal& operator=(const MetadataOperationJournal&) = delete;
    virtual ~MetadataOperationJournal() = default;

    [[nodiscard]] virtual core::Result<void>
    create(const MetadataOperationJournalRecord& record) = 0;
    [[nodiscard]] virtual core::Result<void>
    transition(const core::StableId& id, const MetadataOperationJournalTransition& transition) = 0;
    [[nodiscard]] virtual core::Result<std::optional<MetadataOperationJournalRecord>>
    load(const core::StableId& id) const = 0;
    [[nodiscard]] virtual core::Result<std::vector<MetadataOperationJournalRecord>>
    load_incomplete() const = 0;
    // Admission checks validate only evidence belonging to the file being saved.
    // Full recovery still uses load_incomplete() and reports damaged records.
    [[nodiscard]] virtual core::Result<std::vector<MetadataOperationJournalRecord>>
    load_incomplete_for_source(const std::string& raw_path) const {
        auto records = load_incomplete();
        if (!records)
            return std::unexpected(records.error());
        std::vector<MetadataOperationJournalRecord> matching;
        for (auto& record : *records)
            if (record.source_raw_path == raw_path)
                matching.push_back(std::move(record));
        return matching;
    }
    [[nodiscard]] virtual core::Result<std::optional<MetadataOperationBackupRecord>>
    load_backup(const core::StableId& id) const = 0;
    [[nodiscard]] virtual core::Result<std::vector<MetadataOperationBackupRecord>>
    load_backups() const = 0;
    [[nodiscard]] virtual core::Result<void>
    transition_backup(const core::StableId& id,
                      const MetadataOperationBackupTransition& transition) = 0;
};

} // namespace trackknife::operations
