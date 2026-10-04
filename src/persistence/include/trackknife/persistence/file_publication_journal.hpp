// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/operations/file_publication_journal.hpp"

#include <filesystem>
#include <memory>

namespace trackknife::persistence {

class SqliteFilePublicationJournal final : public operations::FilePublicationJournal {
  public:
    SqliteFilePublicationJournal(SqliteFilePublicationJournal&&) noexcept;
    SqliteFilePublicationJournal& operator=(SqliteFilePublicationJournal&&) noexcept;
    SqliteFilePublicationJournal(const SqliteFilePublicationJournal&) = delete;
    SqliteFilePublicationJournal& operator=(const SqliteFilePublicationJournal&) = delete;
    ~SqliteFilePublicationJournal() override;

    [[nodiscard]] static core::Result<SqliteFilePublicationJournal>
    open(const std::filesystem::path& path);

    [[nodiscard]] core::Result<void>
    create(const operations::FilePublicationJournalRecord& record) override;
    [[nodiscard]] core::Result<void>
    transition(const core::StableId& id,
               const operations::FilePublicationJournalTransition& transition) override;
    [[nodiscard]] core::Result<std::optional<operations::FilePublicationJournalRecord>>
    load(const core::StableId& id) const override;
    [[nodiscard]] core::Result<std::vector<operations::FilePublicationJournalRecord>>
    load_incomplete() const override;
    [[nodiscard]] core::Result<std::vector<operations::FilePublicationJournalRecord>>
    load_reversals(const core::StableId& journal_id) const override;
    [[nodiscard]] core::Result<std::optional<operations::FilePublicationBackupRecord>>
    load_backup(const core::StableId& id) const override;
    [[nodiscard]] core::Result<std::vector<operations::FilePublicationBackupRecord>>
    load_backups() const override;
    [[nodiscard]] core::Result<void>
    transition_backup(const core::StableId& id,
                      const operations::FilePublicationBackupTransition& transition) override;
    // UI/history query over terminal and non-terminal evidence. This is not
    // part of the executor interface because recovery only needs incomplete
    // records and reversal lookups.
    [[nodiscard]] core::Result<std::vector<operations::FilePublicationJournalRecord>>
    load_recent() const;

  private:
    struct Impl;
    explicit SqliteFilePublicationJournal(std::unique_ptr<Impl> implementation);

    std::unique_ptr<Impl> implementation_;
};

} // namespace trackknife::persistence
