// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/operations/metadata_journal.hpp"

#include <filesystem>
#include <memory>

namespace trackknife::persistence {

class SqliteMetadataOperationJournal final : public operations::MetadataOperationJournal {
  public:
    SqliteMetadataOperationJournal(SqliteMetadataOperationJournal&&) noexcept;
    SqliteMetadataOperationJournal& operator=(SqliteMetadataOperationJournal&&) noexcept;
    SqliteMetadataOperationJournal(const SqliteMetadataOperationJournal&) = delete;
    SqliteMetadataOperationJournal& operator=(const SqliteMetadataOperationJournal&) = delete;
    ~SqliteMetadataOperationJournal() override;

    [[nodiscard]] static core::Result<SqliteMetadataOperationJournal>
    open(const std::filesystem::path& path);

    [[nodiscard]] core::Result<void>
    create(const operations::MetadataOperationJournalRecord& record) override;
    [[nodiscard]] core::Result<void>
    transition(const core::StableId& id,
               const operations::MetadataOperationJournalTransition& transition) override;
    [[nodiscard]] core::Result<std::optional<operations::MetadataOperationJournalRecord>>
    load(const core::StableId& id) const override;
    [[nodiscard]] core::Result<std::vector<operations::MetadataOperationJournalRecord>>
    load_incomplete() const override;
    [[nodiscard]] core::Result<std::vector<operations::MetadataOperationJournalRecord>>
    load_incomplete_for_source(const std::string& raw_path) const override;
    [[nodiscard]] core::Result<std::optional<operations::MetadataOperationBackupRecord>>
    load_backup(const core::StableId& id) const override;
    [[nodiscard]] core::Result<std::vector<operations::MetadataOperationBackupRecord>>
    load_backups() const override;
    [[nodiscard]] core::Result<void>
    transition_backup(const core::StableId& id,
                      const operations::MetadataOperationBackupTransition& transition) override;

  private:
    struct Impl;
    explicit SqliteMetadataOperationJournal(std::unique_ptr<Impl> implementation);

    std::unique_ptr<Impl> implementation_;
};

} // namespace trackknife::persistence
