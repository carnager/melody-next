// SPDX-License-Identifier: GPL-3.0-only
#include "trackknife/operations/undo_copies.hpp"

#include <mutex>
#include <system_error>
#include <utility>

namespace trackknife::operations {
namespace {

std::mutex folder_mutex;
std::filesystem::path configured_folder;

} // namespace

void set_undo_copy_folder(std::filesystem::path folder) {
    const std::scoped_lock lock{folder_mutex};
    configured_folder = std::move(folder);
}

std::filesystem::path undo_copy_folder() {
    const std::scoped_lock lock{folder_mutex};
    return configured_folder;
}

std::string undo_copy_path(const std::string& source_raw_path, const std::string& name) {
    const auto folder = undo_copy_folder();
    if (folder.empty()) {
        return (std::filesystem::path{source_raw_path}.parent_path() / name).native();
    }
    std::error_code ignored;
    if (std::filesystem::create_directories(folder, ignored)) {
        std::filesystem::permissions(folder, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, ignored);
    }
    return (folder / name).native();
}

} // namespace trackknife::operations
