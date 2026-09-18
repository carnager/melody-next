// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/persistence/list_repository.hpp"
#include "trackknife/persistence/workspace_backup.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

} // namespace

int main() {
    namespace core = trackknife::core;
    namespace persistence = trackknife::persistence;
    const auto root = std::filesystem::temp_directory_path() /
                      ("trackknife-workspace-backup-" + core::StableId::random().to_string());
    std::filesystem::create_directory(root);
    const auto source = root / "workspace.sqlite";
    const auto backup = root / "workspace-backup.sqlite";

    auto repository = persistence::ListRepository::open(source);
    require(repository.has_value(), "source workspace opens");
    const persistence::ListDocument document{
        .id = core::StableId::random(),
        .kind = persistence::ListKind::scratch,
        .name = "Backup evidence",
        .pinned = true,
        .dirty = false,
        .items = {},
    };
    const std::vector documents{document};
    require(repository->replace_all(documents).has_value(), "source state is committed");

    const auto created = persistence::create_workspace_database_backup(source, backup);
    require(created.has_value(), "consistent backup is created while source remains open");
    require(created->schema_version == 36U && created->size_bytes > 0U,
            "backup reports schema and size evidence");
    require(!persistence::create_workspace_database_backup(source, backup),
            "an existing backup is never replaced");

    auto restored = persistence::ListRepository::open(backup);
    require(restored.has_value(), "backup opens as a workspace");
    const auto lists = restored->load_all();
    require(lists && *lists == std::vector{document}, "committed workspace state round trips");

    const auto corrupt = root / "corrupt.sqlite";
    {
        std::ofstream output{corrupt, std::ios::binary};
        output << "not sqlite";
    }
    require(!persistence::inspect_workspace_database_backup(corrupt),
            "corrupt input is rejected before restore");

    const auto live = root / "restored.sqlite";
    const auto rollback = root / "before-restore.sqlite";
    {
        auto prior = persistence::ListRepository::open(live);
        require(prior.has_value(), "prior live workspace opens");
        require(prior->replace_all({}).has_value(), "prior live workspace is initialized");
    }
    const auto restored_info =
        persistence::restore_workspace_database_backup(backup, live, rollback);
    require(restored_info && std::filesystem::exists(rollback),
            "restore publishes the backup and retains the prior database");
    auto restored_live = persistence::ListRepository::open(live);
    const auto restored_documents =
        restored_live ? restored_live->load_all()
                      : trackknife::core::Result<std::vector<persistence::ListDocument>>{
                            std::unexpected{restored_live.error()}};
    require(restored_documents && *restored_documents == documents,
            "restored live database contains the backed-up workspace");
    require(!persistence::restore_workspace_database_backup(corrupt, live, root / "unused.sqlite"),
            "corrupt restore input cannot change the live workspace");

    std::filesystem::remove_all(root);
}
