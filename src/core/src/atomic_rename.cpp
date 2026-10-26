// SPDX-License-Identifier: GPL-3.0-only
#include "trackknife/core/posix.hpp"

#include "trackknife/core/atomic_rename.hpp"

#include "trackknife/core/stable_id.hpp"

#include <algorithm>
#include <cerrno>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1U << 0U)
#endif

namespace trackknife::core {
namespace {

[[nodiscard]] Error publish_error(const ErrorCode code, const std::string& operation,
                                  const int saved_errno, const std::string& target_name) {
    return Error{
        .code = code,
        .message = operation +
                   " failed: " + std::error_code{saved_errno, std::generic_category()}.message(),
        .context = {{.key = "target", .value = target_name},
                    {.key = "errno", .value = std::to_string(saved_errno)}},
    };
}

} // namespace

Result<NoReplacePublishMethod> publish_no_replace_at(const int source_dir_fd,
                                                     const std::string& source_name,
                                                     const int target_dir_fd,
                                                     const std::string& target_name) {
    if (core::rename_with_flags(source_dir_fd, source_name.c_str(), target_dir_fd,
                                target_name.c_str(), RENAME_NOREPLACE) == 0) {
        return NoReplacePublishMethod::atomic_rename;
    }
    auto saved = errno;
    if (saved == EEXIST) {
        return std::unexpected(publish_error(ErrorCode::conflict, "publishing without replacement",
                                             saved, target_name));
    }
    if (saved == EINVAL || saved == EOPNOTSUPP || saved == ENOTSUP) {
        // The filesystem rejected the flag (NFS, sshfs) — but the VFS
        // resolves the target before the filesystem sees the flag, so an
        // occupied target already reported EEXIST above. The target was
        // therefore absent an instant ago; a plain rename now is the
        // tightest available no-replace publish. A hard-link rung would be
        // fully atomic but breaks on NFS silly-rename when callers hold the
        // published file open.
        if (::renameat(source_dir_fd, source_name.c_str(), target_dir_fd, target_name.c_str()) ==
            0) {
            return NoReplacePublishMethod::checked_rename;
        }
        saved = errno;
        return std::unexpected(
            publish_error(ErrorCode::io, "publishing the prepared file", saved, target_name));
    }
    if (saved != ENOSYS) {
        return std::unexpected(
            publish_error(ErrorCode::io, "publishing without replacement", saved, target_name));
    }

    // No renameat2 at all (very old kernels): check the target ourselves,
    // then rename plainly. The window between check and rename is this
    // rung's documented cost; callers still verify the outcome.
    struct stat existing{};
    if (::fstatat(target_dir_fd, target_name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0) {
        return std::unexpected(publish_error(ErrorCode::conflict, "publishing without replacement",
                                             EEXIST, target_name));
    }
    if (errno != ENOENT) {
        saved = errno;
        return std::unexpected(
            publish_error(ErrorCode::io, "checking the publication target", saved, target_name));
    }
    if (::renameat(source_dir_fd, source_name.c_str(), target_dir_fd, target_name.c_str()) != 0) {
        saved = errno;
        return std::unexpected(
            publish_error(ErrorCode::io, "publishing the prepared file", saved, target_name));
    }
    return NoReplacePublishMethod::checked_rename;
}

std::filesystem::path temporary_sibling(const std::filesystem::path& target,
                                        const std::string_view label) {
    constexpr std::size_t longest_name = 255U;
    const auto suffix = "." + std::string{label} + "-" + StableId::random().to_string();
    auto name = target.filename().native();
    const auto room = longest_name - 1U - std::min(suffix.size(), longest_name - 1U);
    if (name.size() > room) {
        name.resize(room);
        // Not inside a character: a continuation byte is never its start.
        while (!name.empty() && (static_cast<unsigned char>(name.back()) & 0xC0U) == 0x80U) {
            name.pop_back();
        }
        // ...nor after a lead byte whose continuation was cut off.
        if (!name.empty() && (static_cast<unsigned char>(name.back()) & 0xC0U) == 0xC0U) {
            name.pop_back();
        }
    }
    return target.parent_path() / ("." + name + suffix);
}

} // namespace trackknife::core
