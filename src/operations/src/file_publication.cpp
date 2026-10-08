// SPDX-License-Identifier: GPL-3.0-only
#include "trackknife/core/posix.hpp"

#include "trackknife/operations/file_publication.hpp"

#include "trackknife/core/atomic_rename.hpp"
#include "trackknife/operations/undo_copies.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#ifndef __APPLE__
#include <sys/syscall.h>
#endif
#include <sys/xattr.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1U << 0U)
#endif

namespace trackknife::operations {
namespace {

using State = FilePublicationJournalState;
using Content = FilePublicationContentKind;
constexpr auto lock_retry_interval = std::chrono::milliseconds{10};
constexpr std::size_t copy_buffer_bytes = 1024U * 1024U;
constexpr std::size_t maximum_xattr_name_bytes = 1U * 1024U * 1024U;
constexpr std::size_t maximum_xattr_count = 4'096U;
constexpr std::size_t maximum_xattr_value_bytes = 16U * 1024U * 1024U;
constexpr std::size_t maximum_total_xattr_bytes = 64U * 1024U * 1024U;

[[nodiscard]] bool prepared_lifecycle(const FilePublicationJournalRecord& record) noexcept {
    return record.publication == OutputPathPublicationKind::cross_filesystem_copy ||
           record.content == Content::prepared_destination_artifact;
}

[[nodiscard]] bool preserves_source_bytes(const FilePublicationJournalRecord& record) noexcept {
    return record.content == Content::preserve_source_bytes;
}

class Descriptor final {
  public:
    Descriptor() = default;
    explicit Descriptor(const int value) : value_{value} {}
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : value_{std::exchange(other.value_, -1)} {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) {
            static_cast<void>(close());
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    ~Descriptor() { static_cast<void>(close()); }

    [[nodiscard]] int get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept { return value_ >= 0; }

  private:
    [[nodiscard]] bool close() noexcept {
        if (!valid()) {
            return true;
        }
        return ::close(std::exchange(value_, -1)) == 0;
    }

    int value_{-1};
};

struct DirectoryWalk {
    Descriptor descriptor;
    std::vector<std::string> missing_raw_paths;
};

struct LockedSource {
    Descriptor parent;
    Descriptor source;
    core::LocalSourceRevision revision;
};

struct PreparedCopy {
    Descriptor descriptor;
    core::LocalSourceRevision revision;
    // Non-blocking degradations on limited destination filesystems
    // (ADR-0111): preservation that could not happen, surfaced to the user
    // without failing the publication.
    std::vector<std::string> notes;
};

struct ExtendedAttribute {
    std::string name;
    std::vector<unsigned char> value;

    friend bool operator==(const ExtendedAttribute&, const ExtendedAttribute&) = default;
};

// A filesystem without extended-attribute support (NFS without server
// xattrs, sshfs, FAT) reports an empty, unsupported listing instead of an
// error; preservation and verification degrade accordingly (ADR-0111).
struct ExtendedAttributeListing {
    std::vector<ExtendedAttribute> attributes;
    bool supported{true};
};

// What filesystem-metadata preservation actually achieved on the prepared
// copy; later verification only enforces what was achievable.
struct AppliedFilesystemMetadata {
    bool ownership_preserved{true};
    bool mode_preserved{true};
    std::vector<std::string> notes;
};

[[nodiscard]] core::Error
publication_error(const core::ErrorCode code, std::string message,
                  const std::string& source_raw_path, const std::string& target_raw_path = {},
                  const std::optional<core::StableId>& journal_id = std::nullopt) {
    core::Error result{
        .code = code,
        .message = std::move(message),
        .context = {{.key = "source", .value = core::escape_raw_path(source_raw_path)}},
    };
    if (!target_raw_path.empty()) {
        result.context.push_back(
            {.key = "target", .value = core::escape_raw_path(target_raw_path)});
    }
    if (journal_id) {
        result.context.push_back({.key = "journal_id", .value = journal_id->to_string()});
    }
    return result;
}

[[nodiscard]] core::Error
system_error(const std::string_view operation, const int number, const std::string& source_raw_path,
             const std::string& target_raw_path = {},
             const std::optional<core::StableId>& journal_id = std::nullopt) {
    return publication_error(core::ErrorCode::io,
                             std::string{operation} + ": " +
                                 std::error_code{number, std::generic_category()}.message(),
                             source_raw_path, target_raw_path, journal_id);
}

[[nodiscard]] core::Error cancelled(const std::string& source_raw_path,
                                    const std::string& target_raw_path = {}) {
    return publication_error(core::ErrorCode::cancelled,
                             "File publication was cancelled before target publication",
                             source_raw_path, target_raw_path);
}

[[nodiscard]] core::Error journal_failure(const core::Error& failure) {
    return {.code = failure.code, .message = failure.message, .context = {}};
}

[[nodiscard]] bool normal_absolute_path(const std::string_view raw_path) {
    if (raw_path.empty() || raw_path.find('\0') != std::string_view::npos) {
        return false;
    }
    const std::filesystem::path path{raw_path};
    return path.is_absolute() && path == path.lexically_normal();
}

[[nodiscard]] core::LocalSourceRevision revision_from_stat(const struct stat& status) {
    return {
        .device = static_cast<std::uint64_t>(status.st_dev),
        .inode = static_cast<std::uint64_t>(status.st_ino),
        .size = static_cast<std::uint64_t>(status.st_size),
        .modification_time_seconds = static_cast<std::int64_t>(status.st_mtim.tv_sec),
        .modification_time_nanoseconds = static_cast<std::int64_t>(status.st_mtim.tv_nsec),
    };
}

[[nodiscard]] core::Result<DirectoryWalk>
walk_directory(const std::string& raw_path, const bool allow_missing,
               const std::string& source_raw_path, const std::string& target_raw_path,
               const std::optional<core::StableId>& journal_id = std::nullopt) {
    if (!normal_absolute_path(raw_path)) {
        return std::unexpected(
            publication_error(core::ErrorCode::invalid_argument,
                              "File publication requires normalized absolute directory paths",
                              source_raw_path, target_raw_path, journal_id));
    }
    DirectoryWalk result{
        .descriptor = Descriptor{::open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)},
        .missing_raw_paths = {},
    };
    if (!result.descriptor.valid()) {
        return std::unexpected(system_error("Opening the filesystem root failed", errno,
                                            source_raw_path, target_raw_path, journal_id));
    }
    const std::filesystem::path path{raw_path};
    auto current = path.root_path();
    std::vector<std::string> components;
    for (const auto& component : path.relative_path()) {
        components.push_back(component.native());
    }
    for (std::size_t index = 0U; index < components.size(); ++index) {
        Descriptor next{::openat(result.descriptor.get(), components[index].c_str(),
                                 O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)};
        if (next.valid()) {
            result.descriptor = std::move(next);
            current /= components[index];
            continue;
        }
        const auto number = errno;
        if (number == ENOENT && allow_missing) {
            for (; index < components.size(); ++index) {
                current /= components[index];
                result.missing_raw_paths.push_back(current.native());
            }
            return result;
        }
        return std::unexpected(
            system_error(number == ELOOP ? "Refusing a symbolic-link directory component"
                                         : "Opening a publication directory component failed",
                         number, source_raw_path, target_raw_path, journal_id));
    }
    return result;
}

[[nodiscard]] core::Result<DirectoryWalk>
create_planned_directories(const FilePublicationJournalRecord& record) {
    const auto parent = std::filesystem::path{record.target_raw_path}.parent_path();
    DirectoryWalk result{
        .descriptor = Descriptor{::open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)},
        .missing_raw_paths = {},
    };
    if (!result.descriptor.valid()) {
        return std::unexpected(system_error("Opening the filesystem root failed", errno,
                                            record.source_raw_path, record.target_raw_path,
                                            record.id));
    }
    auto current = parent.root_path();
    std::size_t missing_index = 0U;
    for (const auto& component_path : parent.relative_path()) {
        const auto& component = component_path.native();
        current /= component;
        Descriptor next{::openat(result.descriptor.get(), component.c_str(),
                                 O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)};
        if (next.valid()) {
            if (missing_index < record.planned_missing_directory_raw_paths.size() &&
                current.native() == record.planned_missing_directory_raw_paths[missing_index]) {
                return std::unexpected(publication_error(
                    core::ErrorCode::conflict,
                    "A planned missing target directory appeared before publication",
                    record.source_raw_path, record.target_raw_path, record.id));
            }
            result.descriptor = std::move(next);
            continue;
        }
        const auto number = errno;
        if (number != ENOENT ||
            missing_index == record.planned_missing_directory_raw_paths.size() ||
            current.native() != record.planned_missing_directory_raw_paths[missing_index]) {
            return std::unexpected(system_error(
                number == ELOOP ? "Refusing a symbolic-link target directory"
                                : "Target directory topology changed before publication",
                number, record.source_raw_path, record.target_raw_path, record.id));
        }
        if (::mkdirat(result.descriptor.get(), component.c_str(), 0777) != 0) {
            return std::unexpected(system_error("Creating a planned target directory failed", errno,
                                                record.source_raw_path, record.target_raw_path,
                                                record.id));
        }
        if (::fsync(result.descriptor.get()) != 0) {
            return std::unexpected(system_error("Syncing a created target directory failed", errno,
                                                record.source_raw_path, record.target_raw_path,
                                                record.id));
        }
        next = Descriptor{::openat(result.descriptor.get(), component.c_str(),
                                   O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)};
        if (!next.valid()) {
            return std::unexpected(system_error("Opening a created target directory failed", errno,
                                                record.source_raw_path, record.target_raw_path,
                                                record.id));
        }
        result.descriptor = std::move(next);
        ++missing_index;
    }
    if (missing_index != record.planned_missing_directory_raw_paths.size()) {
        return std::unexpected(publication_error(
            core::ErrorCode::conflict,
            "The planned target-directory chain no longer reaches the target parent",
            record.source_raw_path, record.target_raw_path, record.id));
    }
    return result;
}

// Exclusive when the filesystem can express it. NFS emulates flock via
// POSIX locks, which need a write-open descriptor for exclusivity, so
// read-only descriptors get EBADF there; shared locks (or none, where no
// lock manager exists) degrade gracefully — the revision revalidation
// before every mutation carries correctness either way (ADR-0111).
[[nodiscard]] core::Result<void>
lock_descriptor(const Descriptor& descriptor, const core::CancellationToken& cancellation,
                const std::string& source_raw_path, const std::string& target_raw_path,
                const std::optional<core::StableId>& journal_id = std::nullopt) {
    auto operation = LOCK_EX | LOCK_NB;
    while (::flock(descriptor.get(), operation) != 0) {
        const auto number = errno;
        if (number == EBADF && (operation & LOCK_EX) != 0) {
            operation = LOCK_SH | LOCK_NB;
            continue;
        }
        if (number == ENOLCK || number == EOPNOTSUPP || number == ENOTSUP ||
            (number == EBADF && (operation & LOCK_SH) != 0)) {
            return {};
        }
        if (number != EWOULDBLOCK && number != EAGAIN && number != EINTR) {
            return std::unexpected(system_error("Locking the publication source failed", number,
                                                source_raw_path, target_raw_path, journal_id));
        }
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled(source_raw_path, target_raw_path));
        }
        std::this_thread::sleep_for(lock_retry_interval);
    }
    return {};
}

[[nodiscard]] core::Result<LockedSource>
open_locked_source(const std::string& raw_path, const core::LocalSourceRevision& expected_revision,
                   const core::CancellationToken& cancellation, const std::string& target_raw_path,
                   const std::optional<core::StableId>& journal_id = std::nullopt) {
    const std::filesystem::path path{raw_path};
    auto parent =
        walk_directory(path.parent_path().native(), false, raw_path, target_raw_path, journal_id);
    if (!parent) {
        return std::unexpected(std::move(parent.error()));
    }
    Descriptor source{::openat(parent->descriptor.get(), path.filename().c_str(),
                               O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (!source.valid()) {
        return std::unexpected(system_error("Opening the publication source failed", errno,
                                            raw_path, target_raw_path, journal_id));
    }
    if (auto locked = lock_descriptor(source, cancellation, raw_path, target_raw_path, journal_id);
        !locked) {
        return std::unexpected(std::move(locked.error()));
    }
    struct stat descriptor_status{};
    struct stat path_status{};
    if (::fstat(source.get(), &descriptor_status) != 0 ||
        ::fstatat(parent->descriptor.get(), path.filename().c_str(), &path_status,
                  AT_SYMLINK_NOFOLLOW) != 0) {
        return std::unexpected(system_error("Observing the locked publication source failed", errno,
                                            raw_path, target_raw_path, journal_id));
    }
    if (!S_ISREG(descriptor_status.st_mode) || descriptor_status.st_size < 0 ||
        !S_ISREG(path_status.st_mode) || path_status.st_nlink != 1 ||
        descriptor_status.st_dev != path_status.st_dev ||
        descriptor_status.st_ino != path_status.st_ino) {
        return std::unexpected(publication_error(
            core::ErrorCode::unsupported,
            "File publication requires one direct regular source path with one hard link", raw_path,
            target_raw_path, journal_id));
    }
    const auto revision = revision_from_stat(descriptor_status);
    if (revision != expected_revision) {
        return std::unexpected(publication_error(
            core::ErrorCode::conflict, "Publication source changed after filesystem preflight",
            raw_path, target_raw_path, journal_id));
    }
    return LockedSource{
        .parent = std::move(parent->descriptor), .source = std::move(source), .revision = revision};
}

[[nodiscard]] core::Result<std::optional<core::LocalSourceRevision>>
observe_direct_revision(const std::string& raw_path, const std::string& source_raw_path,
                        const std::string& target_raw_path,
                        const std::optional<core::StableId>& journal_id = std::nullopt) {
    const std::filesystem::path path{raw_path};
    auto parent = walk_directory(path.parent_path().native(), true, source_raw_path,
                                 target_raw_path, journal_id);
    if (!parent) {
        return std::unexpected(std::move(parent.error()));
    }
    if (!parent->missing_raw_paths.empty()) {
        return std::optional<core::LocalSourceRevision>{};
    }
    Descriptor descriptor{::openat(parent->descriptor.get(), path.filename().c_str(),
                                   O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (!descriptor.valid()) {
        if (errno == ENOENT) {
            return std::optional<core::LocalSourceRevision>{};
        }
        return std::unexpected(system_error("Opening a publication path failed", errno,
                                            source_raw_path, target_raw_path, journal_id));
    }
    struct stat descriptor_status{};
    struct stat path_status{};
    if (::fstat(descriptor.get(), &descriptor_status) != 0 ||
        ::fstatat(parent->descriptor.get(), path.filename().c_str(), &path_status,
                  AT_SYMLINK_NOFOLLOW) != 0) {
        return std::unexpected(system_error("Observing a publication path failed", errno,
                                            source_raw_path, target_raw_path, journal_id));
    }
    if (!S_ISREG(descriptor_status.st_mode) || descriptor_status.st_size < 0 ||
        !S_ISREG(path_status.st_mode) || path_status.st_nlink != 1 ||
        descriptor_status.st_dev != path_status.st_dev ||
        descriptor_status.st_ino != path_status.st_ino) {
        return std::unexpected(
            publication_error(core::ErrorCode::conflict,
                              "A publication path has changed identity or hard-link topology",
                              source_raw_path, target_raw_path, journal_id));
    }
    return std::optional{revision_from_stat(descriptor_status)};
}

[[nodiscard]] core::Result<void> require_target_absent(const Descriptor& target_parent,
                                                       const std::string& target_name,
                                                       const FilePublicationJournalRecord& record) {
    struct stat status{};
    if (::fstatat(target_parent.get(), target_name.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0) {
        const auto same_source = S_ISREG(status.st_mode) &&
                                 revision_from_stat(status) == record.expected_source_revision;
        return std::unexpected(publication_error(
            same_source ? core::ErrorCode::unsupported : core::ErrorCode::conflict,
            same_source ? "Case-only aliases are not yet safe for crash-recoverable atomic rename"
                        : "Publication target appeared after filesystem preflight",
            record.source_raw_path, record.target_raw_path, record.id));
    }
    if (errno != ENOENT) {
        return std::unexpected(system_error("Observing the publication target failed", errno,
                                            record.source_raw_path, record.target_raw_path,
                                            record.id));
    }
    return {};
}

[[nodiscard]] core::Result<void> require_descriptor_entry(
    const Descriptor& descriptor, const Descriptor& parent, const std::string& name,
    const core::LocalSourceRevision& expected_revision, const FilePublicationJournalRecord& record,
    const std::string_view description) {
    struct stat descriptor_status{};
    struct stat path_status{};
    if (::fstat(descriptor.get(), &descriptor_status) != 0 ||
        ::fstatat(parent.get(), name.c_str(), &path_status, AT_SYMLINK_NOFOLLOW) != 0) {
        return std::unexpected(
            system_error(std::string{"Rechecking "} + std::string{description} + " failed", errno,
                         record.source_raw_path, record.target_raw_path, record.id));
    }
    if (!S_ISREG(path_status.st_mode) || path_status.st_nlink != 1 ||
        descriptor_status.st_dev != path_status.st_dev ||
        descriptor_status.st_ino != path_status.st_ino ||
        revision_from_stat(descriptor_status) != expected_revision ||
        revision_from_stat(path_status) != expected_revision) {
        return std::unexpected(publication_error(
            core::ErrorCode::conflict,
            std::string{description} + " changed immediately before filesystem mutation",
            record.source_raw_path, record.target_raw_path, record.id));
    }
    return {};
}

[[nodiscard]] core::Result<void>
require_locked_source_entry(const LockedSource& source, const std::string& source_name,
                            const FilePublicationJournalRecord& record) {
    return require_descriptor_entry(source.source, source.parent, source_name,
                                    record.expected_source_revision, record,
                                    "locked publication source");
}

[[nodiscard]] core::Result<void> rename_no_replace(const Descriptor& source_parent,
                                                   const std::string& source_name,
                                                   const Descriptor& target_parent,
                                                   const std::string& target_name,
                                                   const FilePublicationJournalRecord& record) {
    auto published = core::publish_no_replace_at(source_parent.get(), source_name,
                                                 target_parent.get(), target_name);
    if (!published) {
        return std::unexpected(
            std::move(published.error())
                .with_context("source", core::escape_raw_path(record.source_raw_path))
                .with_context("journal_id", record.id.to_string()));
    }
    return {};
}

[[nodiscard]] core::Result<void>
remove_descriptor_entry(const Descriptor& descriptor, const Descriptor& parent,
                        const std::string& name, const core::LocalSourceRevision& expected_revision,
                        const FilePublicationJournalRecord& record,
                        const std::string_view description) {
    if (auto current = require_descriptor_entry(descriptor, parent, name, expected_revision, record,
                                                description);
        !current) {
        return current;
    }
    if (::unlinkat(parent.get(), name.c_str(), 0) != 0) {
        return std::unexpected(
            system_error(std::string{"Removing "} + std::string{description} + " failed", errno,
                         record.source_raw_path, record.target_raw_path, record.id));
    }
    if (::fsync(parent.get()) != 0) {
        return std::unexpected(
            system_error(std::string{"Syncing removal of "} + std::string{description} + " failed",
                         errno, record.source_raw_path, record.target_raw_path, record.id));
    }
    struct stat status{};
    if (::fstatat(parent.get(), name.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0 ||
        errno != ENOENT) {
        return std::unexpected(
            publication_error(core::ErrorCode::conflict,
                              std::string{description} + " still exists after its durable removal",
                              record.source_raw_path, record.target_raw_path, record.id));
    }
    return {};
}

// ADR-0263: the source a publication replaces is kept, renamed beside
// itself, never removed; retention releases it later, or undo restores it.
[[nodiscard]] core::Result<void>
retain_source_entry(const Descriptor& descriptor, const Descriptor& parent, const std::string& name,
                    const core::LocalSourceRevision& expected_revision,
                    const FilePublicationJournalRecord& record) {
    if (auto current = require_descriptor_entry(descriptor, parent, name, expected_revision, record,
                                                "publication source");
        !current) {
        return current;
    }
    const auto retained =
        file_publication_retained_path(record.source_raw_path, record.id).filename().native();
    if (auto renamed = rename_no_replace(parent, name, parent, retained, record); !renamed) {
        return renamed;
    }
    if (::fsync(parent.get()) != 0) {
        return std::unexpected(system_error("Syncing the retained publication source failed", errno,
                                            record.source_raw_path, record.target_raw_path,
                                            record.id));
    }
    return {};
}

[[nodiscard]] core::Result<void> read_exact_at(const Descriptor& descriptor,
                                               std::span<char> destination, std::uint64_t offset,
                                               const FilePublicationJournalRecord& record,
                                               const std::string_view description) {
    std::size_t completed = 0U;
    while (completed < destination.size()) {
        ssize_t count = -1;
        do {
            count = ::pread(descriptor.get(), destination.data() + completed,
                            destination.size() - completed, static_cast<off_t>(offset + completed));
        } while (count < 0 && errno == EINTR);
        if (count <= 0) {
            return std::unexpected(
                system_error(std::string{"Reading "} + std::string{description} + " failed",
                             count < 0 ? errno : EIO, record.source_raw_path,
                             record.target_raw_path, record.id));
        }
        completed += static_cast<std::size_t>(count);
    }
    return {};
}

[[nodiscard]] core::Result<ExtendedAttributeListing>
read_extended_attributes(const Descriptor& descriptor, const FilePublicationJournalRecord& record,
                         const std::string_view description) {
    const auto listed = core::list_extended_attributes(descriptor.get(), nullptr, 0U);
    if (listed < 0 && (errno == ENOTSUP || errno == EOPNOTSUPP)) {
        return ExtendedAttributeListing{.attributes = {}, .supported = false};
    }
    if (listed < 0) {
        return std::unexpected(system_error(
            std::string{"Listing extended attributes on "} + std::string{description} + " failed",
            errno, record.source_raw_path, record.target_raw_path, record.id));
    }
    if (static_cast<std::size_t>(listed) > maximum_xattr_name_bytes) {
        return std::unexpected(publication_error(
            core::ErrorCode::limit_exceeded,
            std::string{description} + " has too many extended-attribute name bytes",
            record.source_raw_path, record.target_raw_path, record.id));
    }
    std::vector<char> names(static_cast<std::size_t>(listed));
    if (listed > 0) {
        const auto repeated =
            core::list_extended_attributes(descriptor.get(), names.data(), names.size());
        if (repeated < 0 || repeated != listed) {
            return std::unexpected(
                repeated < 0
                    ? system_error(std::string{"Reading extended-attribute names on "} +
                                       std::string{description} + " failed",
                                   errno, record.source_raw_path, record.target_raw_path, record.id)
                    : publication_error(core::ErrorCode::conflict,
                                        std::string{description} +
                                            " extended attributes changed while being observed",
                                        record.source_raw_path, record.target_raw_path, record.id));
        }
    }
    std::vector<ExtendedAttribute> attributes;
    std::size_t offset = 0U;
    std::size_t total_value_bytes = 0U;
    while (offset < names.size()) {
        const auto end =
            std::find(names.begin() + static_cast<std::ptrdiff_t>(offset), names.end(), '\0');
        if (end == names.end() || end == names.begin() + static_cast<std::ptrdiff_t>(offset) ||
            attributes.size() == maximum_xattr_count) {
            return std::unexpected(publication_error(
                core::ErrorCode::backend,
                std::string{description} + " has a malformed extended-attribute name list",
                record.source_raw_path, record.target_raw_path, record.id));
        }
        std::string name{names.begin() + static_cast<std::ptrdiff_t>(offset), end};
        const auto value_size =
            core::get_extended_attribute(descriptor.get(), name.c_str(), nullptr, 0U);
        if (value_size < 0) {
            return std::unexpected(system_error(std::string{"Sizing an extended attribute on "} +
                                                    std::string{description} + " failed",
                                                errno, record.source_raw_path,
                                                record.target_raw_path, record.id));
        }
        if (static_cast<std::size_t>(value_size) > maximum_xattr_value_bytes ||
            static_cast<std::size_t>(value_size) > maximum_total_xattr_bytes - total_value_bytes) {
            return std::unexpected(publication_error(
                core::ErrorCode::limit_exceeded,
                std::string{description} + " extended-attribute values exceed copy limits",
                record.source_raw_path, record.target_raw_path, record.id));
        }
        std::vector<unsigned char> value(static_cast<std::size_t>(value_size));
        if (value_size > 0) {
            const auto read = core::get_extended_attribute(descriptor.get(), name.c_str(),
                                                           value.data(), value.size());
            if (read < 0 || read != value_size) {
                return std::unexpected(
                    read < 0 ? system_error(std::string{"Reading an extended attribute on "} +
                                                std::string{description} + " failed",
                                            errno, record.source_raw_path, record.target_raw_path,
                                            record.id)
                             : publication_error(
                                   core::ErrorCode::conflict,
                                   std::string{description} +
                                       " extended attributes changed while being read",
                                   record.source_raw_path, record.target_raw_path, record.id));
            }
        }
        total_value_bytes += value.size();
        // Only user-namespace attributes are user metadata; system.,
        // security., and trusted. names are kernel- or filesystem-owned
        // representations (NFS ACLs, SELinux labels) that the destination
        // manages itself and often refuses to accept or remove.
        if (core::user_extended_attribute(name)) {
            attributes.push_back(
                ExtendedAttribute{.name = std::move(name), .value = std::move(value)});
        }
        offset = static_cast<std::size_t>(std::distance(names.begin(), end)) + 1U;
    }
    std::ranges::sort(attributes, {}, &ExtendedAttribute::name);
    return ExtendedAttributeListing{.attributes = std::move(attributes), .supported = true};
}

[[nodiscard]] core::Result<AppliedFilesystemMetadata>
apply_copy_filesystem_metadata(const LockedSource& source, const struct stat& source_status,
                               const ExtendedAttributeListing& source_attributes,
                               const Descriptor& prepared,
                               const FilePublicationJournalRecord& record) {
    AppliedFilesystemMetadata applied;
    auto prepared_attributes =
        read_extended_attributes(prepared, record, "the prepared target copy");
    if (!prepared_attributes) {
        return std::unexpected(std::move(prepared_attributes.error()));
    }
    if (prepared_attributes->supported) {
        for (const auto& attribute : prepared_attributes->attributes) {
            if (std::ranges::none_of(source_attributes.attributes,
                                     [&attribute](const auto& source_attribute) {
                                         return source_attribute.name == attribute.name;
                                     }) &&
                core::remove_extended_attribute(prepared.get(), attribute.name.c_str()) != 0) {
                return std::unexpected(
                    system_error("Removing an unowned prepared-copy extended attribute failed",
                                 errno, record.source_raw_path, record.target_raw_path, record.id));
            }
        }
    } else if (!source_attributes.attributes.empty()) {
        // The destination filesystem cannot store what the source carries;
        // the publication proceeds and says so instead of failing.
        applied.notes.push_back("Extended attributes were not preserved: the destination "
                                "filesystem does not support them");
    }
    struct stat prepared_status{};
    if (::fstat(prepared.get(), &prepared_status) != 0) {
        return std::unexpected(system_error("Observing prepared-copy ownership failed", errno,
                                            record.source_raw_path, record.target_raw_path,
                                            record.id));
    }
    if (prepared_status.st_uid != source_status.st_uid ||
        prepared_status.st_gid != source_status.st_gid) {
        if (::fchown(prepared.get(), source_status.st_uid, source_status.st_gid) != 0) {
            // Network filesystems with server-side identity mapping refuse
            // ownership changes; the copy keeps the writing user's identity.
            if (errno == EPERM || errno == ENOTSUP || errno == EOPNOTSUPP) {
                applied.ownership_preserved = false;
                applied.notes.push_back("File ownership was not preserved: the destination "
                                        "filesystem refused the change");
            } else {
                return std::unexpected(system_error("Preserving prepared-copy ownership failed",
                                                    errno, record.source_raw_path,
                                                    record.target_raw_path, record.id));
            }
        }
    }
    if (::fchmod(prepared.get(), source_status.st_mode & 07777) != 0) {
        if (errno == ENOTSUP || errno == EOPNOTSUPP) {
            applied.mode_preserved = false;
            applied.notes.push_back("File permissions were not preserved: the destination "
                                    "filesystem does not support them");
        } else {
            return std::unexpected(system_error("Preserving prepared-copy permissions failed",
                                                errno, record.source_raw_path,
                                                record.target_raw_path, record.id));
        }
    }
    if (prepared_attributes->supported) {
        for (const auto& attribute : source_attributes.attributes) {
            const auto* data = attribute.value.empty() ? nullptr : attribute.value.data();
            if (core::set_extended_attribute(prepared.get(), attribute.name.c_str(), data,
                                             attribute.value.size()) != 0) {
                return std::unexpected(
                    system_error("Preserving a prepared-copy extended attribute failed", errno,
                                 record.source_raw_path, record.target_raw_path, record.id));
            }
        }
    }
    auto source_after = read_extended_attributes(source.source, record, "the locked source");
    auto prepared_after = read_extended_attributes(prepared, record, "the prepared target copy");
    if (!source_after || !prepared_after) {
        return std::unexpected(!source_after ? std::move(source_after.error())
                                             : std::move(prepared_after.error()));
    }
    if ((source_after->supported && source_attributes.supported &&
         source_after->attributes != source_attributes.attributes) ||
        (prepared_after->supported && source_attributes.supported &&
         prepared_after->attributes != source_attributes.attributes)) {
        return std::unexpected(
            publication_error(core::ErrorCode::conflict,
                              "Filesystem extended attributes changed during copy preparation",
                              record.source_raw_path, record.target_raw_path, record.id));
    }
    if (::fstat(prepared.get(), &prepared_status) != 0 ||
        (applied.ownership_preserved && (prepared_status.st_uid != source_status.st_uid ||
                                         prepared_status.st_gid != source_status.st_gid)) ||
        (applied.mode_preserved &&
         (prepared_status.st_mode & 07777) != (source_status.st_mode & 07777))) {
        return std::unexpected(
            publication_error(core::ErrorCode::conflict,
                              "Prepared-copy ownership or permissions differ from the source",
                              record.source_raw_path, record.target_raw_path, record.id));
    }
    return applied;
}

[[nodiscard]] core::Result<void> verify_exact_copy(const LockedSource& source,
                                                   const LockedSource& candidate,
                                                   const FilePublicationJournalRecord& record,
                                                   const core::CancellationToken& cancellation) {
    struct stat source_status{};
    struct stat candidate_status{};
    if (::fstat(source.source.get(), &source_status) != 0 ||
        ::fstat(candidate.source.get(), &candidate_status) != 0) {
        return std::unexpected(system_error("Observing a copy candidate failed", errno,
                                            record.source_raw_path, record.target_raw_path,
                                            record.id));
    }
    // Ownership is host identity, not content: limited destinations keep the
    // writing user's identity (ADR-0111), so recovery does not compare it.
    if (source.revision.size != candidate.revision.size ||
        (source_status.st_mode & 07777) != (candidate_status.st_mode & 07777) ||
        source_status.st_mtim.tv_sec != candidate_status.st_mtim.tv_sec ||
        source_status.st_mtim.tv_nsec != candidate_status.st_mtim.tv_nsec) {
        return std::unexpected(publication_error(
            core::ErrorCode::conflict, "Prepared target copy differs from the source metadata",
            record.source_raw_path, record.target_raw_path, record.id));
    }
    auto source_attributes = read_extended_attributes(source.source, record, "the recovery source");
    auto candidate_attributes =
        read_extended_attributes(candidate.source, record, "the recovery copy candidate");
    if (!source_attributes || !candidate_attributes) {
        return std::unexpected(!source_attributes ? std::move(source_attributes.error())
                                                  : std::move(candidate_attributes.error()));
    }
    if (source_attributes->supported && candidate_attributes->supported &&
        source_attributes->attributes != candidate_attributes->attributes) {
        return std::unexpected(
            publication_error(core::ErrorCode::conflict,
                              "Prepared target copy differs from the source extended attributes",
                              record.source_raw_path, record.target_raw_path, record.id));
    }
    std::vector<char> source_bytes(copy_buffer_bytes);
    std::vector<char> candidate_bytes(copy_buffer_bytes);
    std::uint64_t offset = 0U;
    while (offset < source.revision.size) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled(record.source_raw_path, record.target_raw_path));
        }
        const auto requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(source_bytes.size(), source.revision.size - offset));
        if (auto read_source =
                read_exact_at(source.source, std::span{source_bytes}.first(requested), offset,
                              record, "the recovery source");
            !read_source) {
            return read_source;
        }
        if (auto read_candidate =
                read_exact_at(candidate.source, std::span{candidate_bytes}.first(requested), offset,
                              record, "the recovery copy candidate");
            !read_candidate) {
            return read_candidate;
        }
        if (!std::equal(source_bytes.begin(),
                        source_bytes.begin() + static_cast<std::ptrdiff_t>(requested),
                        candidate_bytes.begin())) {
            return std::unexpected(publication_error(
                core::ErrorCode::conflict, "Prepared target copy differs from the source bytes",
                record.source_raw_path, record.target_raw_path, record.id));
        }
        offset += static_cast<std::uint64_t>(requested);
    }
    return {};
}

[[nodiscard]] core::Result<PreparedCopy>
copy_source_to_prepared(const LockedSource& source, const Descriptor& target_parent,
                        const FilePublicationJournalRecord& record,
                        const core::CancellationToken& cancellation) {
    if (source.revision.size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return std::unexpected(publication_error(
            core::ErrorCode::unsupported, "Source is too large for verified copy publication",
            record.source_raw_path, record.target_raw_path, record.id));
    }
    struct stat source_status{};
    if (::fstat(source.source.get(), &source_status) != 0 ||
        revision_from_stat(source_status) != source.revision) {
        return std::unexpected(publication_error(
            core::ErrorCode::conflict, "Source changed before verified copy publication",
            record.source_raw_path, record.target_raw_path, record.id));
    }
    auto source_attributes = read_extended_attributes(source.source, record, "the locked source");
    if (!source_attributes) {
        return std::unexpected(std::move(source_attributes.error()));
    }
    const auto prepared_path = std::filesystem::path{record.prepared_raw_path};
    Descriptor prepared{::openat(target_parent.get(), prepared_path.filename().c_str(),
                                 O_RDWR | O_CLOEXEC | O_CREAT | O_EXCL | O_NOFOLLOW, 0600)};
    if (!prepared.valid()) {
        return std::unexpected(system_error("Creating the prepared target copy failed", errno,
                                            record.source_raw_path, record.target_raw_path,
                                            record.id));
    }
    const auto cleanup = [&](core::Error failure) -> core::Result<PreparedCopy> {
        struct stat descriptor_status{};
        struct stat path_status{};
        if (::fstat(prepared.get(), &descriptor_status) != 0 ||
            ::fstatat(target_parent.get(), prepared_path.filename().c_str(), &path_status,
                      AT_SYMLINK_NOFOLLOW) != 0) {
            return std::unexpected(system_error("Observing an incomplete prepared copy failed",
                                                errno, record.source_raw_path,
                                                record.target_raw_path, record.id));
        }
        if (descriptor_status.st_dev != path_status.st_dev ||
            descriptor_status.st_ino != path_status.st_ino) {
            return std::unexpected(publication_error(
                core::ErrorCode::conflict, "Incomplete prepared-copy path changed before cleanup",
                record.source_raw_path, record.target_raw_path, record.id));
        }
        if (::unlinkat(target_parent.get(), prepared_path.filename().c_str(), 0) != 0 ||
            ::fsync(target_parent.get()) != 0) {
            return std::unexpected(system_error("Cleaning an incomplete prepared copy failed",
                                                errno, record.source_raw_path,
                                                record.target_raw_path, record.id));
        }
        return std::unexpected(std::move(failure));
    };

    std::vector<char> buffer(copy_buffer_bytes);
    std::uint64_t offset = 0U;
    while (offset < source.revision.size) {
        if (cancellation.is_cancellation_requested()) {
            return cleanup(cancelled(record.source_raw_path, record.target_raw_path));
        }
        const auto requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), source.revision.size - offset));
        ssize_t read_count = -1;
        do {
            read_count =
                ::pread(source.source.get(), buffer.data(), requested, static_cast<off_t>(offset));
        } while (read_count < 0 && errno == EINTR);
        if (read_count <= 0) {
            return cleanup(system_error("Reading the cross-filesystem source failed",
                                        read_count < 0 ? errno : EIO, record.source_raw_path,
                                        record.target_raw_path, record.id));
        }
        std::size_t written = 0U;
        while (written < static_cast<std::size_t>(read_count)) {
            ssize_t write_count = -1;
            do {
                write_count =
                    ::pwrite(prepared.get(), buffer.data() + written,
                             static_cast<std::size_t>(read_count) - written,
                             static_cast<off_t>(offset + static_cast<std::uint64_t>(written)));
            } while (write_count < 0 && errno == EINTR);
            if (write_count <= 0) {
                return cleanup(system_error("Writing the prepared target copy failed",
                                            write_count < 0 ? errno : EIO, record.source_raw_path,
                                            record.target_raw_path, record.id));
            }
            written += static_cast<std::size_t>(write_count);
        }
        offset += static_cast<std::uint64_t>(read_count);
    }
    const std::array times{source_status.st_atim, source_status.st_mtim};
    auto applied =
        apply_copy_filesystem_metadata(source, source_status, *source_attributes, prepared, record);
    if (!applied) {
        return cleanup(std::move(applied.error()));
    }
    if (::futimens(prepared.get(), times.data()) != 0 || ::fsync(prepared.get()) != 0) {
        return cleanup(system_error("Finalizing the prepared target copy failed", errno,
                                    record.source_raw_path, record.target_raw_path, record.id));
    }
    if (::flock(prepared.get(), LOCK_EX | LOCK_NB) != 0 && errno != ENOLCK && errno != EOPNOTSUPP &&
        errno != ENOTSUP && errno != EBADF) {
        return cleanup(system_error("Locking the prepared target copy failed", errno,
                                    record.source_raw_path, record.target_raw_path, record.id));
    }
    struct stat prepared_status{};
    if (::fstat(prepared.get(), &prepared_status) != 0 || !S_ISREG(prepared_status.st_mode) ||
        prepared_status.st_nlink != 1 || prepared_status.st_size < 0 ||
        static_cast<std::uint64_t>(prepared_status.st_size) != source.revision.size) {
        return cleanup(publication_error(
            core::ErrorCode::conflict, "Prepared target copy has invalid filesystem evidence",
            record.source_raw_path, record.target_raw_path, record.id));
    }
    const auto prepared_revision = revision_from_stat(prepared_status);
    if (auto current =
            require_descriptor_entry(prepared, target_parent, prepared_path.filename().native(),
                                     prepared_revision, record, "prepared target copy");
        !current) {
        return cleanup(std::move(current.error()));
    }
    if (auto current_source = require_locked_source_entry(
            source, std::filesystem::path{record.source_raw_path}.filename().native(), record);
        !current_source) {
        return cleanup(std::move(current_source.error()));
    }

    std::vector<char> comparison(copy_buffer_bytes);
    offset = 0U;
    while (offset < source.revision.size) {
        const auto requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), source.revision.size - offset));
        if (auto read_source = read_exact_at(source.source, std::span{buffer}.first(requested),
                                             offset, record, "the locked source for verification");
            !read_source) {
            return cleanup(std::move(read_source.error()));
        }
        if (auto read_prepared =
                read_exact_at(prepared, std::span{comparison}.first(requested), offset, record,
                              "the prepared target for verification");
            !read_prepared) {
            return cleanup(std::move(read_prepared.error()));
        }
        if (!std::equal(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(requested),
                        comparison.begin())) {
            return cleanup(publication_error(
                core::ErrorCode::conflict, "Prepared target copy differs from the locked source",
                record.source_raw_path, record.target_raw_path, record.id));
        }
        offset += requested;
    }
    auto source_attributes_after =
        read_extended_attributes(source.source, record, "the locked source");
    auto prepared_attributes_after =
        read_extended_attributes(prepared, record, "the prepared target copy");
    if (!source_attributes_after || !prepared_attributes_after) {
        return cleanup(!source_attributes_after ? std::move(source_attributes_after.error())
                                                : std::move(prepared_attributes_after.error()));
    }
    if ((source_attributes_after->supported && source_attributes->supported &&
         source_attributes_after->attributes != source_attributes->attributes) ||
        (prepared_attributes_after->supported && source_attributes->supported &&
         prepared_attributes_after->attributes != source_attributes->attributes)) {
        return cleanup(
            publication_error(core::ErrorCode::conflict,
                              "Filesystem extended attributes changed during byte verification",
                              record.source_raw_path, record.target_raw_path, record.id));
    }
    if (::futimens(prepared.get(), times.data()) != 0 || ::fsync(prepared.get()) != 0) {
        return cleanup(system_error("Finalizing verified target-copy timestamps failed", errno,
                                    record.source_raw_path, record.target_raw_path, record.id));
    }
    struct stat final_status{};
    if (::fstat(prepared.get(), &final_status) != 0 ||
        revision_from_stat(final_status) != prepared_revision ||
        (applied->ownership_preserved && (final_status.st_uid != source_status.st_uid ||
                                          final_status.st_gid != source_status.st_gid)) ||
        (applied->mode_preserved &&
         (final_status.st_mode & 07777) != (source_status.st_mode & 07777)) ||
        final_status.st_atim.tv_sec != source_status.st_atim.tv_sec ||
        final_status.st_atim.tv_nsec != source_status.st_atim.tv_nsec ||
        final_status.st_mtim.tv_sec != source_status.st_mtim.tv_sec ||
        final_status.st_mtim.tv_nsec != source_status.st_mtim.tv_nsec) {
        return cleanup(publication_error(
            core::ErrorCode::conflict,
            "Prepared target copy changed while finalizing its preserved metadata",
            record.source_raw_path, record.target_raw_path, record.id));
    }
    return PreparedCopy{.descriptor = std::move(prepared),
                        .revision = prepared_revision,
                        .notes = std::move(applied->notes)};
}

[[nodiscard]] core::Result<PreparedCopy>
prepare_destination_artifact(const LockedSource& source, const Descriptor& target_parent,
                             const FilePublicationJournalRecord& record,
                             const FilePublicationDestinationArtifactPreparer& artifact_preparer,
                             const core::CancellationToken& cancellation) {
    struct stat source_status{};
    if (::fstat(source.source.get(), &source_status) != 0 ||
        revision_from_stat(source_status) != source.revision) {
        return std::unexpected(publication_error(
            core::ErrorCode::conflict, "Source changed before destination-artifact preparation",
            record.source_raw_path, record.target_raw_path, record.id));
    }
    auto source_attributes = read_extended_attributes(source.source, record, "the locked source");
    if (!source_attributes) {
        return std::unexpected(std::move(source_attributes.error()));
    }
    auto existing = observe_direct_revision(record.prepared_raw_path, record.source_raw_path,
                                            record.target_raw_path, record.id);
    if (!existing) {
        return std::unexpected(std::move(existing.error()));
    }
    if (*existing) {
        return std::unexpected(
            publication_error(core::ErrorCode::conflict, "Destination-artifact path already exists",
                              record.source_raw_path, record.target_raw_path, record.id));
    }

    auto prepared_result = artifact_preparer(record.prepared_raw_path, cancellation);
    if (!prepared_result) {
        auto remained = observe_direct_revision(record.prepared_raw_path, record.source_raw_path,
                                                record.target_raw_path, record.id);
        if (!remained) {
            return std::unexpected(std::move(remained.error()));
        }
        if (*remained) {
            return std::unexpected(publication_error(
                core::ErrorCode::conflict,
                "Destination-artifact preparer left an unjournaled file after failure",
                record.source_raw_path, record.target_raw_path, record.id));
        }
        return std::unexpected(std::move(prepared_result.error()));
    }

    const auto prepared_path = std::filesystem::path{record.prepared_raw_path};
    Descriptor prepared{::openat(target_parent.get(), prepared_path.filename().c_str(),
                                 O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (!prepared.valid()) {
        return std::unexpected(system_error("Opening the prepared destination artifact failed",
                                            errno, record.source_raw_path, record.target_raw_path,
                                            record.id));
    }
    if (auto locked = lock_descriptor(prepared, cancellation, record.source_raw_path,
                                      record.target_raw_path, record.id);
        !locked) {
        return std::unexpected(std::move(locked.error()));
    }
    struct stat prepared_status{};
    if (::fstat(prepared.get(), &prepared_status) != 0 || !S_ISREG(prepared_status.st_mode) ||
        prepared_status.st_nlink != 1 || prepared_status.st_size < 0 ||
        revision_from_stat(prepared_status) != *prepared_result) {
        return std::unexpected(publication_error(
            core::ErrorCode::conflict,
            "Prepared destination artifact differs from the preparer's verified revision",
            record.source_raw_path, record.target_raw_path, record.id));
    }
    const auto cleanup = [&](core::Error failure) -> core::Result<PreparedCopy> {
        const auto current_revision = revision_from_stat(prepared_status);
        auto removed =
            remove_descriptor_entry(prepared, target_parent, prepared_path.filename().native(),
                                    current_revision, record, "prepared destination artifact");
        return std::unexpected(removed ? std::move(failure) : std::move(removed.error()));
    };
    if (auto current =
            require_descriptor_entry(prepared, target_parent, prepared_path.filename().native(),
                                     *prepared_result, record, "prepared destination artifact");
        !current) {
        return cleanup(std::move(current.error()));
    }
    if (auto current_source = require_locked_source_entry(
            source, std::filesystem::path{record.source_raw_path}.filename().native(), record);
        !current_source) {
        return cleanup(std::move(current_source.error()));
    }
    auto applied =
        apply_copy_filesystem_metadata(source, source_status, *source_attributes, prepared, record);
    if (!applied) {
        return cleanup(std::move(applied.error()));
    }
    if (::fsync(prepared.get()) != 0 || ::fsync(target_parent.get()) != 0) {
        return cleanup(system_error("Making the destination artifact durable failed", errno,
                                    record.source_raw_path, record.target_raw_path, record.id));
    }
    if (::fstat(prepared.get(), &prepared_status) != 0 ||
        revision_from_stat(prepared_status) != *prepared_result) {
        return cleanup(
            publication_error(core::ErrorCode::conflict,
                              "Prepared destination artifact changed while becoming durable",
                              record.source_raw_path, record.target_raw_path, record.id));
    }
    if (auto current_source = require_locked_source_entry(
            source, std::filesystem::path{record.source_raw_path}.filename().native(), record);
        !current_source) {
        return cleanup(std::move(current_source.error()));
    }
    return PreparedCopy{.descriptor = std::move(prepared),
                        .revision = *prepared_result,
                        .notes = std::move(applied->notes)};
}

[[nodiscard]] core::Result<void>
verify_published_topology(const FilePublicationJournalRecord& record,
                          const core::LocalSourceRevision& target_revision) {
    auto source = observe_direct_revision(record.source_raw_path, record.source_raw_path,
                                          record.target_raw_path, record.id);
    auto target = observe_direct_revision(record.target_raw_path, record.source_raw_path,
                                          record.target_raw_path, record.id);
    if (!source || !target) {
        return std::unexpected(!source ? std::move(source.error()) : std::move(target.error()));
    }
    if (*source || !*target || **target != target_revision) {
        return std::unexpected(
            publication_error(core::ErrorCode::conflict,
                              "Published rename no longer has the recorded source/target identity",
                              record.source_raw_path, record.target_raw_path, record.id));
    }
    return {};
}

[[nodiscard]] core::Result<void>
sync_publication_directories(const Descriptor& source_parent, const Descriptor& target_parent,
                             const FilePublicationJournalRecord& record) {
    if (::fsync(source_parent.get()) != 0 || ::fsync(target_parent.get()) != 0) {
        return std::unexpected(system_error("Syncing renamed-file directories failed", errno,
                                            record.source_raw_path, record.target_raw_path,
                                            record.id));
    }
    return {};
}

[[nodiscard]] core::Result<void>
transition(FilePublicationJournal& journal, const FilePublicationJournalRecord& record,
           const State from, const State to,
           const std::optional<core::LocalSourceRevision>& target_revision,
           const std::optional<core::Error>& failure = std::nullopt) {
    return journal.transition(record.id,
                              FilePublicationJournalTransition{.expected_state = from,
                                                               .state = to,
                                                               .prepared_revision = std::nullopt,
                                                               .target_revision = target_revision,
                                                               .failure = failure});
}

[[nodiscard]] core::Result<void>
cross_transition(FilePublicationJournal& journal, const FilePublicationJournalRecord& record,
                 const State from, const State to,
                 const std::optional<core::LocalSourceRevision>& prepared_revision,
                 const std::optional<core::LocalSourceRevision>& target_revision,
                 const std::optional<core::Error>& failure = std::nullopt) {
    return journal.transition(
        record.id, FilePublicationJournalTransition{.expected_state = from,
                                                    .state = to,
                                                    .prepared_revision = prepared_revision,
                                                    .target_revision = target_revision,
                                                    .failure = failure});
}

[[nodiscard]] core::Result<void>
cross_terminal_transition(FilePublicationJournal& journal,
                          const FilePublicationJournalRecord& record, const State from,
                          const std::optional<core::LocalSourceRevision>& prepared_revision,
                          const std::optional<core::LocalSourceRevision>& target_revision,
                          const core::Error& failure, const bool rolled_back) {
    return cross_transition(journal, record, from,
                            rolled_back ? State::rolled_back : State::needs_reconciliation,
                            prepared_revision, target_revision, journal_failure(failure));
}

[[nodiscard]] core::Result<void>
terminal_transition(FilePublicationJournal& journal, const FilePublicationJournalRecord& record,
                    const State from,
                    const std::optional<core::LocalSourceRevision>& target_revision,
                    const core::Error& failure, const bool rolled_back) {
    return transition(journal, record, from,
                      rolled_back ? State::rolled_back : State::needs_reconciliation,
                      target_revision, journal_failure(failure));
}

[[nodiscard]] core::Result<void> rollback_rename(const FilePublicationJournalRecord& record,
                                                 const core::LocalSourceRevision& target_revision) {
    auto source = observe_direct_revision(record.source_raw_path, record.source_raw_path,
                                          record.target_raw_path, record.id);
    auto target = observe_direct_revision(record.target_raw_path, record.source_raw_path,
                                          record.target_raw_path, record.id);
    if (!source || !target) {
        return std::unexpected(!source ? std::move(source.error()) : std::move(target.error()));
    }
    if (*source || !*target || **target != target_revision) {
        return std::unexpected(
            publication_error(core::ErrorCode::conflict,
                              "Published rename cannot be rolled back from the recorded identities",
                              record.source_raw_path, record.target_raw_path, record.id));
    }
    const auto source_path = std::filesystem::path{record.source_raw_path};
    const auto target_path = std::filesystem::path{record.target_raw_path};
    auto source_parent = walk_directory(source_path.parent_path().native(), false,
                                        record.source_raw_path, record.target_raw_path, record.id);
    auto target_parent = walk_directory(target_path.parent_path().native(), false,
                                        record.source_raw_path, record.target_raw_path, record.id);
    if (!source_parent || !target_parent) {
        return std::unexpected(!source_parent ? std::move(source_parent.error())
                                              : std::move(target_parent.error()));
    }
    FilePublicationJournalRecord reverse = record;
    reverse.source_raw_path = record.target_raw_path;
    reverse.target_raw_path = record.source_raw_path;
    if (auto renamed =
            rename_no_replace(target_parent->descriptor, target_path.filename().native(),
                              source_parent->descriptor, source_path.filename().native(), reverse);
        !renamed) {
        return renamed;
    }
    if (auto synced = sync_publication_directories(source_parent->descriptor,
                                                   target_parent->descriptor, record);
        !synced) {
        return synced;
    }
    auto restored = observe_direct_revision(record.source_raw_path, record.source_raw_path,
                                            record.target_raw_path, record.id);
    auto removed = observe_direct_revision(record.target_raw_path, record.source_raw_path,
                                           record.target_raw_path, record.id);
    if (!restored || !removed) {
        return std::unexpected(!restored ? std::move(restored.error())
                                         : std::move(removed.error()));
    }
    if (!*restored || **restored != record.expected_source_revision || *removed) {
        return std::unexpected(publication_error(
            core::ErrorCode::conflict, "Rename rollback could not verify the original path",
            record.source_raw_path, record.target_raw_path, record.id));
    }
    return {};
}

[[nodiscard]] core::Result<void>
verify_cross_published_topology(const FilePublicationJournalRecord& record,
                                const core::LocalSourceRevision& prepared_revision,
                                const core::LocalSourceRevision& target_revision) {
    auto source = observe_direct_revision(record.source_raw_path, record.source_raw_path,
                                          record.target_raw_path, record.id);
    auto prepared = observe_direct_revision(record.prepared_raw_path, record.source_raw_path,
                                            record.target_raw_path, record.id);
    auto target = observe_direct_revision(record.target_raw_path, record.source_raw_path,
                                          record.target_raw_path, record.id);
    if (!source || !prepared || !target) {
        return std::unexpected(!source     ? std::move(source.error())
                               : !prepared ? std::move(prepared.error())
                                           : std::move(target.error()));
    }
    if (!*source || **source != record.expected_source_revision || *prepared || !*target ||
        **target != target_revision || target_revision != prepared_revision) {
        return std::unexpected(publication_error(
            core::ErrorCode::conflict,
            "Cross-filesystem publication no longer has its recorded path identities",
            record.source_raw_path, record.target_raw_path, record.id));
    }
    return {};
}

[[nodiscard]] core::Result<void>
verify_cross_source_removed_topology(const FilePublicationJournalRecord& record,
                                     const core::LocalSourceRevision& target_revision) {
    auto source = observe_direct_revision(record.source_raw_path, record.source_raw_path,
                                          record.target_raw_path, record.id);
    auto prepared = observe_direct_revision(record.prepared_raw_path, record.source_raw_path,
                                            record.target_raw_path, record.id);
    auto target = observe_direct_revision(record.target_raw_path, record.source_raw_path,
                                          record.target_raw_path, record.id);
    auto retained = observe_direct_revision(
        file_publication_retained_path(record.source_raw_path, record.id).native(),
        record.source_raw_path, record.target_raw_path, record.id);
    if (!source || !prepared || !target || !retained) {
        return std::unexpected(!source     ? std::move(source.error())
                               : !prepared ? std::move(prepared.error())
                               : !target   ? std::move(target.error())
                                           : std::move(retained.error()));
    }
    // The source kept beside it as it was -- or, from before sources were
    // kept, gone.
    if (*source || *prepared || !*target || **target != target_revision ||
        (*retained && **retained != record.expected_source_revision)) {
        return std::unexpected(publication_error(
            core::ErrorCode::conflict,
            "Completed cross-filesystem move no longer has its recorded target identity",
            record.source_raw_path, record.target_raw_path, record.id));
    }
    return {};
}

[[nodiscard]] core::Result<void> rollback_locked_cross_filesystem_target(
    const FilePublicationJournalRecord& record, const Descriptor& target_descriptor,
    const Descriptor& target_parent, const core::LocalSourceRevision& target_revision) {
    auto source = observe_direct_revision(record.source_raw_path, record.source_raw_path,
                                          record.target_raw_path, record.id);
    auto prepared = observe_direct_revision(record.prepared_raw_path, record.source_raw_path,
                                            record.target_raw_path, record.id);
    if (!source || !prepared) {
        return std::unexpected(!source ? std::move(source.error()) : std::move(prepared.error()));
    }
    if (!*source || **source != record.expected_source_revision || *prepared) {
        return std::unexpected(publication_error(
            core::ErrorCode::conflict,
            "Cross-filesystem target cannot be rolled back from recorded identities",
            record.source_raw_path, record.target_raw_path, record.id));
    }
    if (auto removed = remove_descriptor_entry(
            target_descriptor, target_parent,
            std::filesystem::path{record.target_raw_path}.filename().native(), target_revision,
            record, "published target");
        !removed) {
        return removed;
    }
    auto restored_source = observe_direct_revision(record.source_raw_path, record.source_raw_path,
                                                   record.target_raw_path, record.id);
    auto removed_target = observe_direct_revision(record.target_raw_path, record.source_raw_path,
                                                  record.target_raw_path, record.id);
    if (!restored_source || !removed_target || !*restored_source ||
        **restored_source != record.expected_source_revision || *removed_target) {
        return std::unexpected(publication_error(
            core::ErrorCode::conflict, "Cross-filesystem target rollback could not be verified",
            record.source_raw_path, record.target_raw_path, record.id));
    }
    return {};
}

[[nodiscard]] FilePublicationCommitResult
commit_result(const FilePublicationJournalRecord& record,
              const core::LocalSourceRevision& target_revision) {
    return {
        .journal_id = record.id,
        .content = record.content,
        .source_raw_path = record.source_raw_path,
        .target_raw_path = record.target_raw_path,
        .source_revision = record.expected_source_revision,
        .target_revision = target_revision,
        .occurrence_indexes = record.occurrence_indexes,
        .notes = {},
    };
}

[[nodiscard]] core::Result<FilePublicationCommitResult> execute_created_same_filesystem_rename(
    FilePublicationJournalRecord record, const LockedSource& source,
    const Descriptor& target_parent, FilePublicationJournal& journal,
    const FilePublicationDependentStateCommitter& dependent_state_committer,
    const core::CancellationToken& cancellation) {
    const auto finish_before_publication =
        [&](const core::Error& failure) -> core::Result<FilePublicationCommitResult> {
        auto terminal =
            terminal_transition(journal, record, State::planned, std::nullopt, failure, true);
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(failure);
    };
    if (cancellation.is_cancellation_requested()) {
        return finish_before_publication(cancelled(record.source_raw_path, record.target_raw_path));
    }
    const auto source_path = std::filesystem::path{record.source_raw_path};
    const auto target_path = std::filesystem::path{record.target_raw_path};
    if (auto absent = require_target_absent(target_parent, target_path.filename().native(), record);
        !absent) {
        return finish_before_publication(absent.error());
    }
    if (auto current_source =
            require_locked_source_entry(source, source_path.filename().native(), record);
        !current_source) {
        return finish_before_publication(current_source.error());
    }
    if (cancellation.is_cancellation_requested()) {
        return finish_before_publication(cancelled(record.source_raw_path, record.target_raw_path));
    }
    if (auto renamed = rename_no_replace(source.parent, source_path.filename().native(),
                                         target_parent, target_path.filename().native(), record);
        !renamed) {
        return finish_before_publication(renamed.error());
    }
    const auto target_revision = source.revision;
    auto synced = sync_publication_directories(source.parent, target_parent, record);
    auto verified = verify_published_topology(record, target_revision);
    if (!synced || !verified) {
        const auto failure = !synced ? synced.error() : verified.error();
        const auto rolled_back = rollback_rename(record, target_revision);
        auto terminal = terminal_transition(journal, record, State::planned, std::nullopt, failure,
                                            rolled_back.has_value());
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(rolled_back ? failure : rolled_back.error());
    }
    if (auto published =
            transition(journal, record, State::planned, State::target_published, target_revision);
        !published) {
        const auto& failure = published.error();
        const auto rolled_back = rollback_rename(record, target_revision);
        auto terminal = terminal_transition(journal, record, State::planned, std::nullopt, failure,
                                            rolled_back.has_value());
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(rolled_back ? failure : rolled_back.error());
    }
    record.state = State::target_published;
    record.target_revision = target_revision;
    auto result = commit_result(record, target_revision);
    if (auto dependent = dependent_state_committer(result); !dependent) {
        const auto& failure = dependent.error();
        const auto rolled_back = rollback_rename(record, target_revision);
        auto terminal = terminal_transition(journal, record, State::target_published,
                                            target_revision, failure, rolled_back.has_value());
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(rolled_back ? failure : rolled_back.error());
    }
    if (auto final_topology = verify_published_topology(record, target_revision); !final_topology) {
        auto marked = terminal_transition(journal, record, State::target_published, target_revision,
                                          final_topology.error(), false);
        return std::unexpected(marked ? final_topology.error() : std::move(marked.error()));
    }
    if (auto dependent = transition(journal, record, State::target_published,
                                    State::dependent_state_committed, target_revision);
        !dependent) {
        return std::unexpected(std::move(dependent.error()));
    }
    if (auto completed = transition(journal, record, State::dependent_state_committed,
                                    State::complete, target_revision);
        !completed) {
        return std::unexpected(std::move(completed.error()));
    }
    return result;
}

[[nodiscard]] core::Result<FilePublicationCommitResult> execute_prepared_publication(
    FilePublicationJournalRecord record, const LockedSource& source,
    const Descriptor& target_parent, const PreparedCopy& prepared, FilePublicationJournal& journal,
    const FilePublicationDependentStateCommitter& dependent_state_committer,
    const core::CancellationToken& cancellation) {
    const auto target_path = std::filesystem::path{record.target_raw_path};
    const auto rollback_prepared =
        [&](const core::Error& failure) -> core::Result<FilePublicationCommitResult> {
        const auto removed = remove_descriptor_entry(
            prepared.descriptor, target_parent,
            std::filesystem::path{record.prepared_raw_path}.filename().native(), prepared.revision,
            record, "prepared target artifact");
        auto terminal =
            cross_terminal_transition(journal, record, State::target_prepared, prepared.revision,
                                      std::nullopt, failure, removed.has_value());
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(removed ? failure : removed.error());
    };
    if (cancellation.is_cancellation_requested()) {
        return rollback_prepared(cancelled(record.source_raw_path, record.target_raw_path));
    }
    if (auto current = require_descriptor_entry(
            prepared.descriptor, target_parent,
            std::filesystem::path{record.prepared_raw_path}.filename().native(), prepared.revision,
            record, "prepared target artifact");
        !current) {
        return rollback_prepared(current.error());
    }
    if (auto absent = require_target_absent(target_parent, target_path.filename().native(), record);
        !absent) {
        return rollback_prepared(absent.error());
    }
    if (auto published = rename_no_replace(
            target_parent, std::filesystem::path{record.prepared_raw_path}.filename().native(),
            target_parent, target_path.filename().native(), record);
        !published) {
        return rollback_prepared(published.error());
    }
    const auto target_revision = prepared.revision;
    if (auto synced = sync_publication_directories(target_parent, target_parent, record); !synced) {
        const auto rolled_back = rollback_locked_cross_filesystem_target(
            record, prepared.descriptor, target_parent, target_revision);
        auto terminal =
            cross_terminal_transition(journal, record, State::target_prepared, prepared.revision,
                                      std::nullopt, synced.error(), rolled_back.has_value());
        if (!terminal) {
            return std::unexpected(terminal.error());
        }
        return std::unexpected(rolled_back ? synced.error() : rolled_back.error());
    }
    if (auto verified = verify_cross_published_topology(record, prepared.revision, target_revision);
        !verified) {
        const auto rolled_back = rollback_locked_cross_filesystem_target(
            record, prepared.descriptor, target_parent, target_revision);
        auto terminal =
            cross_terminal_transition(journal, record, State::target_prepared, prepared.revision,
                                      std::nullopt, verified.error(), rolled_back.has_value());
        if (!terminal) {
            return std::unexpected(terminal.error());
        }
        return std::unexpected(rolled_back ? verified.error() : rolled_back.error());
    }
    if (auto published_state =
            cross_transition(journal, record, State::target_prepared, State::target_published,
                             prepared.revision, target_revision);
        !published_state) {
        const auto rolled_back = rollback_locked_cross_filesystem_target(
            record, prepared.descriptor, target_parent, target_revision);
        auto terminal = cross_terminal_transition(journal, record, State::target_prepared,
                                                  prepared.revision, std::nullopt,
                                                  published_state.error(), rolled_back.has_value());
        if (!terminal) {
            return std::unexpected(terminal.error());
        }
        return std::unexpected(rolled_back ? published_state.error() : rolled_back.error());
    }
    record.state = State::target_published;
    record.target_revision = target_revision;
    auto result = commit_result(record, target_revision);
    result.notes = prepared.notes;
    if (auto dependent = dependent_state_committer(result); !dependent) {
        const auto rolled_back = rollback_locked_cross_filesystem_target(
            record, prepared.descriptor, target_parent, target_revision);
        auto terminal =
            cross_terminal_transition(journal, record, State::target_published, prepared.revision,
                                      target_revision, dependent.error(), rolled_back.has_value());
        if (!terminal) {
            return std::unexpected(terminal.error());
        }
        return std::unexpected(rolled_back ? dependent.error() : rolled_back.error());
    }
    if (auto verified = verify_cross_published_topology(record, prepared.revision, target_revision);
        !verified) {
        auto terminal =
            cross_terminal_transition(journal, record, State::target_published, prepared.revision,
                                      target_revision, verified.error(), false);
        return std::unexpected(terminal ? verified.error() : std::move(terminal.error()));
    }
    if (auto dependent_state =
            cross_transition(journal, record, State::target_published,
                             State::dependent_state_committed, prepared.revision, target_revision);
        !dependent_state) {
        return std::unexpected(std::move(dependent_state.error()));
    }
    record.state = State::dependent_state_committed;
    if (auto current_target = require_descriptor_entry(prepared.descriptor, target_parent,
                                                       target_path.filename().native(),
                                                       target_revision, record, "published target");
        !current_target) {
        return std::unexpected(std::move(current_target.error()));
    }
    const auto source_name = std::filesystem::path{record.source_raw_path}.filename().native();
    if (auto retained = retain_source_entry(source.source, source.parent, source_name,
                                            record.expected_source_revision, record);
        !retained) {
        return std::unexpected(std::move(retained.error()));
    }
    if (auto verified = verify_cross_source_removed_topology(record, target_revision); !verified) {
        auto terminal =
            cross_terminal_transition(journal, record, State::dependent_state_committed,
                                      prepared.revision, target_revision, verified.error(), false);
        return std::unexpected(terminal ? verified.error() : std::move(terminal.error()));
    }
    if (auto removed_state =
            cross_transition(journal, record, State::dependent_state_committed,
                             State::source_removed, prepared.revision, target_revision);
        !removed_state) {
        return std::unexpected(std::move(removed_state.error()));
    }
    if (auto completed = cross_transition(journal, record, State::source_removed, State::complete,
                                          prepared.revision, target_revision);
        !completed) {
        return std::unexpected(std::move(completed.error()));
    }
    return result;
}

[[nodiscard]] core::Result<void> mark_reconciliation(FilePublicationJournal& journal,
                                                     const FilePublicationJournalRecord& record,
                                                     const core::Error& issue) {
    return terminal_transition(journal, record, record.state, record.target_revision, issue, false);
}

[[nodiscard]] core::Result<void>
mark_cross_reconciliation(FilePublicationJournal& journal,
                          const FilePublicationJournalRecord& record, const core::Error& issue) {
    return cross_terminal_transition(journal, record, record.state, record.prepared_revision,
                                     record.target_revision, issue, false);
}

} // namespace

core::Result<FilePublicationCommitResult> commit_same_filesystem_publication(
    const OutputPathPreflight& preflight, const std::size_t source_index,
    FilePublicationJournal& journal,
    const FilePublicationDependentStateCommitter& dependent_state_committer,
    const core::CancellationToken& cancellation,
    const FilePublicationDirectoryCreationObserver& directory_creation_observer) {
    if (cancellation.is_cancellation_requested()) {
        const auto source = source_index < preflight.sources.size()
                                ? preflight.sources[source_index].planned.source_raw_path
                                : std::string{};
        return std::unexpected(cancelled(source));
    }
    if (!dependent_state_committer || !preflight.ready() ||
        source_index >= preflight.sources.size()) {
        return std::unexpected(publication_error(
            core::ErrorCode::invalid_argument,
            "File publication requires a ready preflight, source, and state committer", {}));
    }
    const auto& checked = preflight.sources[source_index];
    if (checked.publication != OutputPathPublicationKind::same_filesystem_rename ||
        checked.planned.no_change || checked.observed_revision != checked.planned.source_revision ||
        checked.target_filesystem_device != checked.observed_revision.device) {
        return std::unexpected(
            publication_error(core::ErrorCode::invalid_argument,
                              "This executor requires one ready same-filesystem rename",
                              checked.planned.source_raw_path, checked.planned.target_raw_path));
    }

    auto source = open_locked_source(checked.planned.source_raw_path, checked.observed_revision,
                                     cancellation, checked.planned.target_raw_path);
    if (!source) {
        return std::unexpected(std::move(source.error()));
    }
    const auto target_path = std::filesystem::path{checked.planned.target_raw_path};
    auto fresh_target =
        walk_directory(target_path.parent_path().native(), true, checked.planned.source_raw_path,
                       checked.planned.target_raw_path);
    if (!fresh_target) {
        return std::unexpected(std::move(fresh_target.error()));
    }
    if (fresh_target->missing_raw_paths != checked.missing_directory_raw_paths) {
        return std::unexpected(
            publication_error(core::ErrorCode::conflict,
                              "Target directory topology changed after filesystem preflight",
                              checked.planned.source_raw_path, checked.planned.target_raw_path));
    }
    struct stat target_parent_status{};
    if (::fstat(fresh_target->descriptor.get(), &target_parent_status) != 0) {
        return std::unexpected(system_error("Observing the target filesystem failed", errno,
                                            checked.planned.source_raw_path,
                                            checked.planned.target_raw_path));
    }
    if (static_cast<std::uint64_t>(target_parent_status.st_dev) !=
            checked.target_filesystem_device ||
        ::faccessat(source->parent.get(), ".", W_OK | X_OK, 0) != 0 ||
        ::faccessat(fresh_target->descriptor.get(), ".", W_OK | X_OK, 0) != 0) {
        return std::unexpected(publication_error(
            core::ErrorCode::conflict,
            "Publication filesystem or directory permissions changed after preflight",
            checked.planned.source_raw_path, checked.planned.target_raw_path));
    }
    if (fresh_target->missing_raw_paths.empty()) {
        FilePublicationJournalRecord provisional{
            .id = core::StableId::random(),
            .state = State::planned,
            .publication = OutputPathPublicationKind::same_filesystem_rename,
            .source_raw_path = checked.planned.source_raw_path,
            .target_raw_path = checked.planned.target_raw_path,
            .prepared_raw_path = {},
            .expected_source_revision = checked.observed_revision,
            .prepared_revision = std::nullopt,
            .target_revision = std::nullopt,
            .occurrence_indexes = checked.planned.item_indexes,
            .planned_missing_directory_raw_paths = checked.missing_directory_raw_paths,
            .reverses_journal_id = std::nullopt,
            .failure = std::nullopt,
        };
        if (auto absent = require_target_absent(fresh_target->descriptor,
                                                target_path.filename().native(), provisional);
            !absent) {
            return std::unexpected(std::move(absent.error()));
        }
    }

    auto record =
        make_file_publication_journal_record(preflight, source_index, core::StableId::random());
    if (!record) {
        return std::unexpected(std::move(record.error()));
    }
    if (auto created = journal.create(*record); !created) {
        return std::unexpected(std::move(created.error()));
    }
    const auto finish_before_publication =
        [&](const core::Error& failure) -> core::Result<FilePublicationCommitResult> {
        auto terminal =
            terminal_transition(journal, *record, State::planned, std::nullopt, failure, true);
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(failure);
    };
    if (cancellation.is_cancellation_requested()) {
        return finish_before_publication(
            cancelled(record->source_raw_path, record->target_raw_path));
    }

    auto target_parent = create_planned_directories(*record);
    if (!target_parent) {
        return finish_before_publication(target_parent.error());
    }
    if (directory_creation_observer && !record->planned_missing_directory_raw_paths.empty()) {
        directory_creation_observer(record->planned_missing_directory_raw_paths);
    }
    struct stat created_parent_status{};
    if (::fstat(target_parent->descriptor.get(), &created_parent_status) != 0 ||
        static_cast<std::uint64_t>(created_parent_status.st_dev) !=
            checked.target_filesystem_device) {
        const auto failure = publication_error(
            core::ErrorCode::conflict, "Created target parent is not on the preflight filesystem",
            record->source_raw_path, record->target_raw_path, record->id);
        return finish_before_publication(failure);
    }
    return execute_created_same_filesystem_rename(std::move(*record), *source,
                                                  target_parent->descriptor, journal,
                                                  dependent_state_committer, cancellation);
}

core::Result<FilePublicationCommitResult> undo_same_filesystem_publication(
    const core::StableId& journal_id, FilePublicationJournal& journal,
    const FilePublicationDependentStateCommitter& dependent_state_committer,
    const core::CancellationToken& cancellation) {
    if (journal_id.is_nil() || !dependent_state_committer) {
        return std::unexpected(publication_error(
            core::ErrorCode::invalid_argument,
            "File-publication undo requires an operation and state committer", {}));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled({}));
    }
    auto loaded = journal.load(journal_id);
    if (!loaded) {
        return std::unexpected(std::move(loaded.error()));
    }
    if (!*loaded) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::not_found,
            .message = "File publication was not found for undo",
            .context = {{"journal_id", journal_id.to_string()}},
        });
    }
    const auto& original = **loaded;
    if (original.publication != OutputPathPublicationKind::same_filesystem_rename ||
        !preserves_source_bytes(original) || original.state != State::complete ||
        !original.target_revision || original.reverses_journal_id) {
        return std::unexpected(
            publication_error(core::ErrorCode::conflict,
                              "Only a completed original same-filesystem publication can be undone",
                              original.source_raw_path, original.target_raw_path, original.id));
    }
    auto reversals = journal.load_reversals(original.id);
    if (!reversals) {
        return std::unexpected(std::move(reversals.error()));
    }
    for (const auto& reversal : *reversals) {
        if (reversal.state == State::rolled_back) {
            continue;
        }
        if (reversal.state == State::complete && reversal.target_revision) {
            if (auto topology = verify_published_topology(reversal, *reversal.target_revision);
                !topology) {
                return std::unexpected(std::move(topology.error()));
            }
            return commit_result(reversal, *reversal.target_revision);
        }
        return std::unexpected(publication_error(
            core::ErrorCode::conflict,
            reversal.state == State::needs_reconciliation
                ? "File-publication undo requires manual reconciliation"
                : "File-publication undo must finish startup recovery before retry",
            reversal.source_raw_path, reversal.target_raw_path, reversal.id));
    }

    auto source = open_locked_source(original.target_raw_path, *original.target_revision,
                                     cancellation, original.source_raw_path, original.id);
    if (!source) {
        return std::unexpected(std::move(source.error()));
    }
    const auto target_path = std::filesystem::path{original.source_raw_path};
    auto target_parent =
        walk_directory(target_path.parent_path().native(), false, original.target_raw_path,
                       original.source_raw_path, original.id);
    if (!target_parent) {
        return std::unexpected(std::move(target_parent.error()));
    }
    struct stat target_parent_status{};
    if (::fstat(target_parent->descriptor.get(), &target_parent_status) != 0 ||
        static_cast<std::uint64_t>(target_parent_status.st_dev) != source->revision.device ||
        ::faccessat(source->parent.get(), ".", W_OK | X_OK, 0) != 0 ||
        ::faccessat(target_parent->descriptor.get(), ".", W_OK | X_OK, 0) != 0) {
        return std::unexpected(
            publication_error(core::ErrorCode::conflict,
                              "Undo filesystem or directory permissions changed after publication",
                              original.target_raw_path, original.source_raw_path, original.id));
    }
    FilePublicationJournalRecord reversal{
        .id = core::StableId::random(),
        .state = State::planned,
        .publication = OutputPathPublicationKind::same_filesystem_rename,
        .source_raw_path = original.target_raw_path,
        .target_raw_path = original.source_raw_path,
        .prepared_raw_path = {},
        .expected_source_revision = source->revision,
        .prepared_revision = std::nullopt,
        .target_revision = std::nullopt,
        .occurrence_indexes = original.occurrence_indexes,
        .planned_missing_directory_raw_paths = {},
        .reverses_journal_id = original.id,
        .failure = std::nullopt,
    };
    if (auto absent = require_target_absent(target_parent->descriptor,
                                            target_path.filename().native(), reversal);
        !absent) {
        return std::unexpected(std::move(absent.error()));
    }
    const auto source_path = std::filesystem::path{reversal.source_raw_path};
    if (auto current_source =
            require_locked_source_entry(*source, source_path.filename().native(), reversal);
        !current_source) {
        return std::unexpected(std::move(current_source.error()));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(reversal.source_raw_path, reversal.target_raw_path));
    }
    if (auto created = journal.create(reversal); !created) {
        return std::unexpected(std::move(created.error()));
    }
    return execute_created_same_filesystem_rename(std::move(reversal), *source,
                                                  target_parent->descriptor, journal,
                                                  dependent_state_committer, cancellation);
}

core::Result<FilePublicationCommitResult> commit_cross_filesystem_publication(
    const OutputPathPreflight& preflight, const std::size_t source_index,
    FilePublicationJournal& journal,
    const FilePublicationDependentStateCommitter& dependent_state_committer,
    const core::CancellationToken& cancellation,
    const FilePublicationDirectoryCreationObserver& directory_creation_observer) {
    if (cancellation.is_cancellation_requested()) {
        const auto source = source_index < preflight.sources.size()
                                ? preflight.sources[source_index].planned.source_raw_path
                                : std::string{};
        return std::unexpected(cancelled(source));
    }
    if (!dependent_state_committer || !preflight.ready() ||
        source_index >= preflight.sources.size()) {
        return std::unexpected(publication_error(
            core::ErrorCode::invalid_argument,
            "Cross-filesystem publication requires a ready source and state committer", {}));
    }
    const auto& checked = preflight.sources[source_index];
    if (checked.publication != OutputPathPublicationKind::cross_filesystem_copy ||
        checked.planned.no_change || checked.observed_revision != checked.planned.source_revision ||
        checked.target_filesystem_device == checked.observed_revision.device) {
        return std::unexpected(
            publication_error(core::ErrorCode::invalid_argument,
                              "This executor requires one ready cross-filesystem move",
                              checked.planned.source_raw_path, checked.planned.target_raw_path));
    }

    auto source = open_locked_source(checked.planned.source_raw_path, checked.observed_revision,
                                     cancellation, checked.planned.target_raw_path);
    if (!source) {
        return std::unexpected(std::move(source.error()));
    }
    const auto target_path = std::filesystem::path{checked.planned.target_raw_path};
    auto fresh_target =
        walk_directory(target_path.parent_path().native(), true, checked.planned.source_raw_path,
                       checked.planned.target_raw_path);
    if (!fresh_target) {
        return std::unexpected(std::move(fresh_target.error()));
    }
    if (fresh_target->missing_raw_paths != checked.missing_directory_raw_paths) {
        return std::unexpected(
            publication_error(core::ErrorCode::conflict,
                              "Target directory topology changed after cross-filesystem preflight",
                              checked.planned.source_raw_path, checked.planned.target_raw_path));
    }
    struct stat target_parent_status{};
    if (::fstat(fresh_target->descriptor.get(), &target_parent_status) != 0 ||
        static_cast<std::uint64_t>(target_parent_status.st_dev) !=
            checked.target_filesystem_device ||
        ::faccessat(source->parent.get(), ".", W_OK | X_OK, 0) != 0 ||
        ::faccessat(fresh_target->descriptor.get(), ".", W_OK | X_OK, 0) != 0) {
        return std::unexpected(publication_error(
            core::ErrorCode::conflict,
            "Cross-filesystem target or directory permissions changed after preflight",
            checked.planned.source_raw_path, checked.planned.target_raw_path));
    }
    if (fresh_target->missing_raw_paths.empty()) {
        const auto provisional_id = core::StableId::random();
        FilePublicationJournalRecord provisional{
            .id = provisional_id,
            .state = State::planned,
            .publication = OutputPathPublicationKind::cross_filesystem_copy,
            .source_raw_path = checked.planned.source_raw_path,
            .target_raw_path = checked.planned.target_raw_path,
            .prepared_raw_path =
                file_publication_prepared_path(checked.planned.target_raw_path, provisional_id)
                    .native(),
            .expected_source_revision = checked.observed_revision,
            .prepared_revision = std::nullopt,
            .target_revision = std::nullopt,
            .occurrence_indexes = checked.planned.item_indexes,
            .planned_missing_directory_raw_paths = {},
            .reverses_journal_id = std::nullopt,
            .failure = std::nullopt,
        };
        if (auto absent = require_target_absent(fresh_target->descriptor,
                                                target_path.filename().native(), provisional);
            !absent) {
            return std::unexpected(std::move(absent.error()));
        }
    }

    auto record =
        make_file_publication_journal_record(preflight, source_index, core::StableId::random());
    if (!record) {
        return std::unexpected(std::move(record.error()));
    }
    if (auto created = journal.create(*record); !created) {
        return std::unexpected(std::move(created.error()));
    }
    const auto finish_planned =
        [&](const core::Error& failure) -> core::Result<FilePublicationCommitResult> {
        auto persisted_source = observe_direct_revision(
            record->source_raw_path, record->source_raw_path, record->target_raw_path, record->id);
        auto prepared = observe_direct_revision(record->prepared_raw_path, record->source_raw_path,
                                                record->target_raw_path, record->id);
        auto target = observe_direct_revision(record->target_raw_path, record->source_raw_path,
                                              record->target_raw_path, record->id);
        const bool rolled_back = persisted_source && prepared && target && *persisted_source &&
                                 **persisted_source == record->expected_source_revision &&
                                 !*prepared && !*target;
        auto terminal = cross_terminal_transition(journal, *record, State::planned, std::nullopt,
                                                  std::nullopt, failure, rolled_back);
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(failure);
    };
    if (cancellation.is_cancellation_requested()) {
        return finish_planned(cancelled(record->source_raw_path, record->target_raw_path));
    }
    auto target_parent = create_planned_directories(*record);
    if (!target_parent) {
        return finish_planned(target_parent.error());
    }
    if (directory_creation_observer && !record->planned_missing_directory_raw_paths.empty()) {
        directory_creation_observer(record->planned_missing_directory_raw_paths);
    }
    if (::fstat(target_parent->descriptor.get(), &target_parent_status) != 0 ||
        static_cast<std::uint64_t>(target_parent_status.st_dev) !=
            checked.target_filesystem_device) {
        return finish_planned(publication_error(
            core::ErrorCode::conflict, "Created copy target parent is on another filesystem",
            record->source_raw_path, record->target_raw_path, record->id));
    }
    if (auto absent = require_target_absent(target_parent->descriptor,
                                            target_path.filename().native(), *record);
        !absent) {
        return finish_planned(absent.error());
    }
    auto prepared =
        copy_source_to_prepared(*source, target_parent->descriptor, *record, cancellation);
    if (!prepared) {
        return finish_planned(prepared.error());
    }
    if (auto prepared_state =
            cross_transition(journal, *record, State::planned, State::target_prepared,
                             prepared->revision, std::nullopt);
        !prepared_state) {
        const auto removed = remove_descriptor_entry(
            prepared->descriptor, target_parent->descriptor,
            std::filesystem::path{record->prepared_raw_path}.filename().native(),
            prepared->revision, *record, "prepared target copy");
        auto terminal =
            cross_terminal_transition(journal, *record, State::planned, std::nullopt, std::nullopt,
                                      prepared_state.error(), removed.has_value());
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(removed ? prepared_state.error() : removed.error());
    }
    record->state = State::target_prepared;
    record->prepared_revision = prepared->revision;
    return execute_prepared_publication(std::move(*record), *source, target_parent->descriptor,
                                        *prepared, journal, dependent_state_committer,
                                        cancellation);
}

core::Result<FilePublicationCommitResult> commit_destination_artifact_publication(
    const OutputPathPreflight& preflight, const std::size_t source_index,
    FilePublicationJournal& journal,
    const FilePublicationDestinationArtifactPreparer& artifact_preparer,
    const FilePublicationDependentStateCommitter& dependent_state_committer,
    const core::CancellationToken& cancellation,
    const FilePublicationDirectoryCreationObserver& directory_creation_observer) {
    if (cancellation.is_cancellation_requested()) {
        const auto source = source_index < preflight.sources.size()
                                ? preflight.sources[source_index].planned.source_raw_path
                                : std::string{};
        return std::unexpected(cancelled(source));
    }
    if (!artifact_preparer || !dependent_state_committer || !preflight.ready() ||
        source_index >= preflight.sources.size()) {
        return std::unexpected(publication_error(
            core::ErrorCode::invalid_argument,
            "Destination-artifact publication requires a ready source, preparer, and state "
            "committer",
            {}));
    }
    const auto& checked = preflight.sources[source_index];
    const bool valid_topology =
        (checked.publication == OutputPathPublicationKind::same_filesystem_rename &&
         checked.target_filesystem_device == checked.observed_revision.device) ||
        (checked.publication == OutputPathPublicationKind::cross_filesystem_copy &&
         checked.target_filesystem_device != checked.observed_revision.device);
    if (checked.publication == OutputPathPublicationKind::no_change || checked.planned.no_change ||
        checked.observed_revision != checked.planned.source_revision || !valid_topology) {
        return std::unexpected(publication_error(
            core::ErrorCode::invalid_argument,
            "Destination-artifact executor requires one ready changed output path",
            checked.planned.source_raw_path, checked.planned.target_raw_path));
    }

    auto source = open_locked_source(checked.planned.source_raw_path, checked.observed_revision,
                                     cancellation, checked.planned.target_raw_path);
    if (!source) {
        return std::unexpected(std::move(source.error()));
    }
    const auto target_path = std::filesystem::path{checked.planned.target_raw_path};
    auto fresh_target =
        walk_directory(target_path.parent_path().native(), true, checked.planned.source_raw_path,
                       checked.planned.target_raw_path);
    if (!fresh_target) {
        return std::unexpected(std::move(fresh_target.error()));
    }
    if (fresh_target->missing_raw_paths != checked.missing_directory_raw_paths) {
        return std::unexpected(publication_error(
            core::ErrorCode::conflict,
            "Target directory topology changed after destination-artifact preflight",
            checked.planned.source_raw_path, checked.planned.target_raw_path));
    }
    struct stat target_parent_status{};
    if (::fstat(fresh_target->descriptor.get(), &target_parent_status) != 0 ||
        static_cast<std::uint64_t>(target_parent_status.st_dev) !=
            checked.target_filesystem_device ||
        ::faccessat(source->parent.get(), ".", W_OK | X_OK, 0) != 0 ||
        ::faccessat(fresh_target->descriptor.get(), ".", W_OK | X_OK, 0) != 0) {
        return std::unexpected(publication_error(
            core::ErrorCode::conflict,
            "Destination-artifact filesystem or directory permissions changed after preflight",
            checked.planned.source_raw_path, checked.planned.target_raw_path));
    }
    const auto journal_id = core::StableId::random();
    auto record = make_destination_artifact_journal_record(preflight, source_index, journal_id);
    if (!record) {
        return std::unexpected(std::move(record.error()));
    }
    if (fresh_target->missing_raw_paths.empty()) {
        if (auto absent = require_target_absent(fresh_target->descriptor,
                                                target_path.filename().native(), *record);
            !absent) {
            return std::unexpected(std::move(absent.error()));
        }
        auto prepared = observe_direct_revision(record->prepared_raw_path, record->source_raw_path,
                                                record->target_raw_path, record->id);
        if (!prepared) {
            return std::unexpected(std::move(prepared.error()));
        }
        if (*prepared) {
            return std::unexpected(publication_error(
                core::ErrorCode::conflict, "Destination-artifact path already exists",
                record->source_raw_path, record->target_raw_path, record->id));
        }
    }
    if (auto created = journal.create(*record); !created) {
        return std::unexpected(std::move(created.error()));
    }
    const auto finish_planned =
        [&](const core::Error& failure) -> core::Result<FilePublicationCommitResult> {
        auto persisted_source = observe_direct_revision(
            record->source_raw_path, record->source_raw_path, record->target_raw_path, record->id);
        auto prepared = observe_direct_revision(record->prepared_raw_path, record->source_raw_path,
                                                record->target_raw_path, record->id);
        auto target = observe_direct_revision(record->target_raw_path, record->source_raw_path,
                                              record->target_raw_path, record->id);
        const bool rolled_back = persisted_source && prepared && target && *persisted_source &&
                                 **persisted_source == record->expected_source_revision &&
                                 !*prepared && !*target;
        auto terminal = cross_terminal_transition(journal, *record, State::planned, std::nullopt,
                                                  std::nullopt, failure, rolled_back);
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(failure);
    };
    if (cancellation.is_cancellation_requested()) {
        return finish_planned(cancelled(record->source_raw_path, record->target_raw_path));
    }
    auto target_parent = create_planned_directories(*record);
    if (!target_parent) {
        return finish_planned(target_parent.error());
    }
    if (directory_creation_observer && !record->planned_missing_directory_raw_paths.empty()) {
        directory_creation_observer(record->planned_missing_directory_raw_paths);
    }
    if (::fstat(target_parent->descriptor.get(), &target_parent_status) != 0 ||
        static_cast<std::uint64_t>(target_parent_status.st_dev) !=
            checked.target_filesystem_device) {
        return finish_planned(
            publication_error(core::ErrorCode::conflict,
                              "Created destination-artifact parent is on another filesystem",
                              record->source_raw_path, record->target_raw_path, record->id));
    }
    if (auto absent = require_target_absent(target_parent->descriptor,
                                            target_path.filename().native(), *record);
        !absent) {
        return finish_planned(absent.error());
    }
    auto prepared = prepare_destination_artifact(*source, target_parent->descriptor, *record,
                                                 artifact_preparer, cancellation);
    if (!prepared) {
        return finish_planned(prepared.error());
    }
    if (auto prepared_state =
            cross_transition(journal, *record, State::planned, State::target_prepared,
                             prepared->revision, std::nullopt);
        !prepared_state) {
        const auto removed = remove_descriptor_entry(
            prepared->descriptor, target_parent->descriptor,
            std::filesystem::path{record->prepared_raw_path}.filename().native(),
            prepared->revision, *record, "prepared destination artifact");
        auto terminal =
            cross_terminal_transition(journal, *record, State::planned, std::nullopt, std::nullopt,
                                      prepared_state.error(), removed.has_value());
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(removed ? prepared_state.error() : removed.error());
    }
    record->state = State::target_prepared;
    record->prepared_revision = prepared->revision;
    return execute_prepared_publication(std::move(*record), *source, target_parent->descriptor,
                                        *prepared, journal, dependent_state_committer,
                                        cancellation);
}

core::Result<std::vector<FilePublicationRecoveryResult>> recover_same_filesystem_publications(
    FilePublicationJournal& journal,
    const FilePublicationDependentStateCommitter& dependent_state_committer,
    const core::CancellationToken& cancellation) {
    if (!dependent_state_committer) {
        return std::unexpected(publication_error(
            core::ErrorCode::invalid_argument,
            "File-publication recovery requires a dependent-state committer", {}));
    }
    auto records = journal.load_incomplete();
    if (!records) {
        return std::unexpected(std::move(records.error()));
    }
    std::vector<FilePublicationRecoveryResult> results;
    results.reserve(records->size());
    for (auto& record : *records) {
        if (record.publication != OutputPathPublicationKind::same_filesystem_rename ||
            prepared_lifecycle(record)) {
            continue;
        }
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled(record.source_raw_path, record.target_raw_path));
        }
        if (record.state == State::needs_reconciliation) {
            results.push_back({.journal_id = record.id,
                               .outcome = FilePublicationRecoveryOutcome::needs_reconciliation,
                               .issue = record.failure});
            continue;
        }
        auto source = observe_direct_revision(record.source_raw_path, record.source_raw_path,
                                              record.target_raw_path, record.id);
        auto target = observe_direct_revision(record.target_raw_path, record.source_raw_path,
                                              record.target_raw_path, record.id);
        if (!source || !target) {
            auto issue = !source ? std::move(source.error()) : std::move(target.error());
            auto marked = mark_reconciliation(journal, record, issue);
            if (!marked) {
                return std::unexpected(std::move(marked.error()));
            }
            results.push_back({.journal_id = record.id,
                               .outcome = FilePublicationRecoveryOutcome::needs_reconciliation,
                               .issue = issue});
            continue;
        }
        const bool source_matches = *source && **source == record.expected_source_revision;
        const bool target_matches = *target && **target == record.expected_source_revision;
        if (source_matches == target_matches) {
            auto issue =
                publication_error(core::ErrorCode::conflict,
                                  "Interrupted rename has ambiguous source and target identities",
                                  record.source_raw_path, record.target_raw_path, record.id);
            auto marked = mark_reconciliation(journal, record, issue);
            if (!marked) {
                return std::unexpected(std::move(marked.error()));
            }
            results.push_back({.journal_id = record.id,
                               .outcome = FilePublicationRecoveryOutcome::needs_reconciliation,
                               .issue = issue});
            continue;
        }
        const auto lock_path = source_matches ? record.source_raw_path : record.target_raw_path;
        auto locked = open_locked_source(lock_path, record.expected_source_revision, cancellation,
                                         record.target_raw_path, record.id);
        if (!locked) {
            auto issue = std::move(locked.error());
            if (issue.code == core::ErrorCode::cancelled) {
                return std::unexpected(std::move(issue));
            }
            auto marked = mark_reconciliation(journal, record, issue);
            if (!marked) {
                return std::unexpected(std::move(marked.error()));
            }
            results.push_back({.journal_id = record.id,
                               .outcome = FilePublicationRecoveryOutcome::needs_reconciliation,
                               .issue = issue});
            continue;
        }
        source = observe_direct_revision(record.source_raw_path, record.source_raw_path,
                                         record.target_raw_path, record.id);
        target = observe_direct_revision(record.target_raw_path, record.source_raw_path,
                                         record.target_raw_path, record.id);
        if (!source || !target || static_cast<bool>(*source) == static_cast<bool>(*target) ||
            (*source && **source != record.expected_source_revision) ||
            (*target && **target != record.expected_source_revision)) {
            auto issue =
                publication_error(core::ErrorCode::conflict,
                                  "Rename topology changed while startup recovery locked the file",
                                  record.source_raw_path, record.target_raw_path, record.id);
            auto marked = mark_reconciliation(journal, record, issue);
            if (!marked) {
                return std::unexpected(std::move(marked.error()));
            }
            results.push_back({.journal_id = record.id,
                               .outcome = FilePublicationRecoveryOutcome::needs_reconciliation,
                               .issue = issue});
            continue;
        }
        if (record.state == State::planned && *source) {
            auto issue =
                publication_error(core::ErrorCode::cancelled,
                                  "Recovered a journaled rename before atomic publication",
                                  record.source_raw_path, record.target_raw_path, record.id);
            auto terminal =
                terminal_transition(journal, record, State::planned, std::nullopt, issue, true);
            if (!terminal) {
                return std::unexpected(std::move(terminal.error()));
            }
            results.push_back({.journal_id = record.id,
                               .outcome = FilePublicationRecoveryOutcome::rolled_back,
                               .issue = std::nullopt});
            continue;
        }
        if (record.state == State::planned) {
            auto published = transition(journal, record, State::planned, State::target_published,
                                        record.expected_source_revision);
            if (!published) {
                return std::unexpected(std::move(published.error()));
            }
            record.state = State::target_published;
            record.target_revision = record.expected_source_revision;
        }
        if (!*target || !record.target_revision || **target != *record.target_revision) {
            auto issue =
                publication_error(core::ErrorCode::conflict,
                                  "Interrupted rename target differs from its journal identity",
                                  record.source_raw_path, record.target_raw_path, record.id);
            auto marked = mark_reconciliation(journal, record, issue);
            if (!marked) {
                return std::unexpected(std::move(marked.error()));
            }
            results.push_back({.journal_id = record.id,
                               .outcome = FilePublicationRecoveryOutcome::needs_reconciliation,
                               .issue = issue});
            continue;
        }
        if (record.state == State::dependent_state_committed) {
            auto completed = transition(journal, record, State::dependent_state_committed,
                                        State::complete, *record.target_revision);
            if (!completed) {
                return std::unexpected(std::move(completed.error()));
            }
            results.push_back({.journal_id = record.id,
                               .outcome = FilePublicationRecoveryOutcome::completed,
                               .issue = std::nullopt});
            continue;
        }
        if (record.state != State::target_published) {
            auto issue =
                publication_error(core::ErrorCode::invariant,
                                  "Same-filesystem recovery encountered an invalid journal state",
                                  record.source_raw_path, record.target_raw_path, record.id);
            auto marked = mark_reconciliation(journal, record, issue);
            if (!marked) {
                return std::unexpected(std::move(marked.error()));
            }
            results.push_back({.journal_id = record.id,
                               .outcome = FilePublicationRecoveryOutcome::needs_reconciliation,
                               .issue = issue});
            continue;
        }
        const auto recovered_result = commit_result(record, *record.target_revision);
        if (auto dependent = dependent_state_committer(recovered_result); !dependent) {
            const auto& issue = dependent.error();
            const auto rolled_back = rollback_rename(record, *record.target_revision);
            auto terminal =
                terminal_transition(journal, record, State::target_published,
                                    record.target_revision, issue, rolled_back.has_value());
            if (!terminal) {
                return std::unexpected(std::move(terminal.error()));
            }
            results.push_back({
                .journal_id = record.id,
                .outcome = rolled_back ? FilePublicationRecoveryOutcome::rolled_back
                                       : FilePublicationRecoveryOutcome::needs_reconciliation,
                .issue =
                    rolled_back ? std::nullopt : std::optional<core::Error>{rolled_back.error()},
            });
            continue;
        }
        if (auto verified = verify_published_topology(record, *record.target_revision); !verified) {
            auto marked = mark_reconciliation(journal, record, verified.error());
            if (!marked) {
                return std::unexpected(std::move(marked.error()));
            }
            results.push_back({.journal_id = record.id,
                               .outcome = FilePublicationRecoveryOutcome::needs_reconciliation,
                               .issue = verified.error()});
            continue;
        }
        if (auto dependent = transition(journal, record, State::target_published,
                                        State::dependent_state_committed, *record.target_revision);
            !dependent) {
            return std::unexpected(std::move(dependent.error()));
        }
        if (auto completed = transition(journal, record, State::dependent_state_committed,
                                        State::complete, *record.target_revision);
            !completed) {
            return std::unexpected(std::move(completed.error()));
        }
        results.push_back({.journal_id = record.id,
                           .outcome = FilePublicationRecoveryOutcome::completed,
                           .issue = std::nullopt});
    }
    return results;
}

core::Result<std::vector<FilePublicationRecoveryResult>> recover_cross_filesystem_publications(
    FilePublicationJournal& journal,
    const FilePublicationDependentStateCommitter& dependent_state_committer,
    const core::CancellationToken& cancellation) {
    if (!dependent_state_committer) {
        return std::unexpected(publication_error(
            core::ErrorCode::invalid_argument,
            "Cross-filesystem recovery requires a dependent-state committer", {}));
    }
    auto records = journal.load_incomplete();
    if (!records) {
        return std::unexpected(std::move(records.error()));
    }
    std::vector<FilePublicationRecoveryResult> results;
    results.reserve(records->size());
    const auto reconcile = [&](const FilePublicationJournalRecord& record,
                               core::Error issue) -> core::Result<void> {
        if (auto marked = mark_cross_reconciliation(journal, record, issue); !marked) {
            return marked;
        }
        results.push_back({.journal_id = record.id,
                           .outcome = FilePublicationRecoveryOutcome::needs_reconciliation,
                           .issue = std::move(issue)});
        return {};
    };

    for (auto& record : *records) {
        if (!prepared_lifecycle(record)) {
            continue;
        }
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled(record.source_raw_path, record.target_raw_path));
        }
        if (record.state == State::needs_reconciliation) {
            results.push_back({.journal_id = record.id,
                               .outcome = FilePublicationRecoveryOutcome::needs_reconciliation,
                               .issue = record.failure});
            continue;
        }

        auto source = observe_direct_revision(record.source_raw_path, record.source_raw_path,
                                              record.target_raw_path, record.id);
        auto prepared = observe_direct_revision(record.prepared_raw_path, record.source_raw_path,
                                                record.target_raw_path, record.id);
        auto target = observe_direct_revision(record.target_raw_path, record.source_raw_path,
                                              record.target_raw_path, record.id);
        if (!source || !prepared || !target) {
            auto issue = !source     ? std::move(source.error())
                         : !prepared ? std::move(prepared.error())
                                     : std::move(target.error());
            if (auto marked = reconcile(record, std::move(issue)); !marked) {
                return std::unexpected(std::move(marked.error()));
            }
            continue;
        }

        if (record.state == State::planned) {
            if (*source && **source == record.expected_source_revision && !*prepared && !*target) {
                const auto issue = publication_error(
                    core::ErrorCode::cancelled,
                    "Recovered a journaled copy before a prepared target became durable",
                    record.source_raw_path, record.target_raw_path, record.id);
                if (auto terminal = cross_terminal_transition(
                        journal, record, State::planned, std::nullopt, std::nullopt, issue, true);
                    !terminal) {
                    return std::unexpected(std::move(terminal.error()));
                }
                results.push_back({.journal_id = record.id,
                                   .outcome = FilePublicationRecoveryOutcome::rolled_back,
                                   .issue = std::nullopt});
                continue;
            }
            if (!*source || **source != record.expected_source_revision || !*prepared || *target) {
                auto issue = publication_error(
                    core::ErrorCode::conflict,
                    "Interrupted copy has ambiguous source, prepared, and target identities",
                    record.source_raw_path, record.target_raw_path, record.id);
                if (auto marked = reconcile(record, std::move(issue)); !marked) {
                    return std::unexpected(std::move(marked.error()));
                }
                continue;
            }
            if (!preserves_source_bytes(record)) {
                auto issue = publication_error(
                    core::ErrorCode::conflict,
                    "Interrupted destination-artifact preparation lacks a durable verified "
                    "revision",
                    record.source_raw_path, record.target_raw_path, record.id);
                if (auto marked = reconcile(record, std::move(issue)); !marked) {
                    return std::unexpected(std::move(marked.error()));
                }
                continue;
            }
            auto locked_source =
                open_locked_source(record.source_raw_path, record.expected_source_revision,
                                   cancellation, record.target_raw_path, record.id);
            auto locked_prepared =
                open_locked_source(record.prepared_raw_path, **prepared, cancellation,
                                   record.target_raw_path, record.id);
            if (!locked_source || !locked_prepared) {
                auto issue = !locked_source ? std::move(locked_source.error())
                                            : std::move(locked_prepared.error());
                if (issue.code == core::ErrorCode::cancelled) {
                    return std::unexpected(std::move(issue));
                }
                if (auto marked = reconcile(record, std::move(issue)); !marked) {
                    return std::unexpected(std::move(marked.error()));
                }
                continue;
            }
            if (auto exact =
                    verify_exact_copy(*locked_source, *locked_prepared, record, cancellation);
                !exact) {
                auto issue = std::move(exact.error());
                if (issue.code == core::ErrorCode::cancelled) {
                    return std::unexpected(std::move(issue));
                }
                if (auto marked = reconcile(record, std::move(issue)); !marked) {
                    return std::unexpected(std::move(marked.error()));
                }
                continue;
            }
            if (::fsync(locked_prepared->source.get()) != 0 ||
                ::fsync(locked_prepared->parent.get()) != 0) {
                auto issue =
                    system_error("Making the recovered prepared copy durable failed", errno,
                                 record.source_raw_path, record.target_raw_path, record.id);
                if (auto marked = reconcile(record, std::move(issue)); !marked) {
                    return std::unexpected(std::move(marked.error()));
                }
                continue;
            }
            if (auto adopted =
                    cross_transition(journal, record, State::planned, State::target_prepared,
                                     locked_prepared->revision, std::nullopt);
                !adopted) {
                return std::unexpected(std::move(adopted.error()));
            }
            record.state = State::target_prepared;
            record.prepared_revision = locked_prepared->revision;
            prepared = std::optional{locked_prepared->revision};
        }

        if (record.state == State::target_prepared) {
            if (!record.prepared_revision || !*source ||
                **source != record.expected_source_revision) {
                auto issue = publication_error(
                    core::ErrorCode::conflict,
                    "Prepared-copy recovery no longer has the original source identity",
                    record.source_raw_path, record.target_raw_path, record.id);
                if (auto marked = reconcile(record, std::move(issue)); !marked) {
                    return std::unexpected(std::move(marked.error()));
                }
                continue;
            }
            if (!*prepared && !*target) {
                const auto issue =
                    publication_error(core::ErrorCode::cancelled,
                                      "Recovered after a prepared copy had already been cleaned up",
                                      record.source_raw_path, record.target_raw_path, record.id);
                if (auto terminal = cross_terminal_transition(
                        journal, record, State::target_prepared, record.prepared_revision,
                        std::nullopt, issue, true);
                    !terminal) {
                    return std::unexpected(std::move(terminal.error()));
                }
                results.push_back({.journal_id = record.id,
                                   .outcome = FilePublicationRecoveryOutcome::rolled_back,
                                   .issue = std::nullopt});
                continue;
            }
            if (*prepared && **prepared == *record.prepared_revision && !*target) {
                auto locked_source =
                    open_locked_source(record.source_raw_path, record.expected_source_revision,
                                       cancellation, record.target_raw_path, record.id);
                auto locked_prepared =
                    open_locked_source(record.prepared_raw_path, *record.prepared_revision,
                                       cancellation, record.target_raw_path, record.id);
                if (!locked_source || !locked_prepared) {
                    auto issue = !locked_source ? std::move(locked_source.error())
                                                : std::move(locked_prepared.error());
                    if (issue.code == core::ErrorCode::cancelled) {
                        return std::unexpected(std::move(issue));
                    }
                    if (auto marked = reconcile(record, std::move(issue)); !marked) {
                        return std::unexpected(std::move(marked.error()));
                    }
                    continue;
                }
                if (preserves_source_bytes(record)) {
                    if (auto exact = verify_exact_copy(*locked_source, *locked_prepared, record,
                                                       cancellation);
                        !exact) {
                        auto issue = std::move(exact.error());
                        if (issue.code == core::ErrorCode::cancelled) {
                            return std::unexpected(std::move(issue));
                        }
                        if (auto marked = reconcile(record, std::move(issue)); !marked) {
                            return std::unexpected(std::move(marked.error()));
                        }
                        continue;
                    }
                }
                const auto prepared_path = std::filesystem::path{record.prepared_raw_path};
                const auto target_path = std::filesystem::path{record.target_raw_path};
                if (auto absent = require_target_absent(locked_prepared->parent,
                                                        target_path.filename().native(), record);
                    !absent) {
                    if (auto marked = reconcile(record, std::move(absent.error())); !marked) {
                        return std::unexpected(std::move(marked.error()));
                    }
                    continue;
                }
                if (auto published = rename_no_replace(
                        locked_prepared->parent, prepared_path.filename().native(),
                        locked_prepared->parent, target_path.filename().native(), record);
                    !published) {
                    if (auto marked = reconcile(record, std::move(published.error())); !marked) {
                        return std::unexpected(std::move(marked.error()));
                    }
                    continue;
                }
                if (auto synced = sync_publication_directories(locked_prepared->parent,
                                                               locked_prepared->parent, record);
                    !synced) {
                    return std::unexpected(std::move(synced.error()));
                }
                prepared = std::optional<core::LocalSourceRevision>{};
                target = record.prepared_revision;
            } else if (*prepared || !*target || **target != *record.prepared_revision) {
                auto issue =
                    publication_error(core::ErrorCode::conflict,
                                      "Prepared-copy publication has ambiguous recovery identities",
                                      record.source_raw_path, record.target_raw_path, record.id);
                if (auto marked = reconcile(record, std::move(issue)); !marked) {
                    return std::unexpected(std::move(marked.error()));
                }
                continue;
            }
            if (auto verified = verify_cross_published_topology(record, *record.prepared_revision,
                                                                *record.prepared_revision);
                !verified) {
                if (auto marked = reconcile(record, std::move(verified.error())); !marked) {
                    return std::unexpected(std::move(marked.error()));
                }
                continue;
            }
            if (auto published = cross_transition(journal, record, State::target_prepared,
                                                  State::target_published, record.prepared_revision,
                                                  record.prepared_revision);
                !published) {
                return std::unexpected(std::move(published.error()));
            }
            record.state = State::target_published;
            record.target_revision = record.prepared_revision;
        }

        if (record.state == State::target_published) {
            if (!record.prepared_revision || !record.target_revision) {
                auto issue =
                    publication_error(core::ErrorCode::invariant,
                                      "Published-copy recovery is missing durable target evidence",
                                      record.source_raw_path, record.target_raw_path, record.id);
                if (auto marked = reconcile(record, std::move(issue)); !marked) {
                    return std::unexpected(std::move(marked.error()));
                }
                continue;
            }
            auto locked_source =
                open_locked_source(record.source_raw_path, record.expected_source_revision,
                                   cancellation, record.target_raw_path, record.id);
            auto locked_target =
                open_locked_source(record.target_raw_path, *record.target_revision, cancellation,
                                   record.source_raw_path, record.id);
            if (!locked_source || !locked_target) {
                auto issue = !locked_source ? std::move(locked_source.error())
                                            : std::move(locked_target.error());
                if (issue.code == core::ErrorCode::cancelled) {
                    return std::unexpected(std::move(issue));
                }
                if (auto marked = reconcile(record, std::move(issue)); !marked) {
                    return std::unexpected(std::move(marked.error()));
                }
                continue;
            }
            if (preserves_source_bytes(record)) {
                if (auto exact =
                        verify_exact_copy(*locked_source, *locked_target, record, cancellation);
                    !exact) {
                    auto issue = std::move(exact.error());
                    if (issue.code == core::ErrorCode::cancelled) {
                        return std::unexpected(std::move(issue));
                    }
                    if (auto marked = reconcile(record, std::move(issue)); !marked) {
                        return std::unexpected(std::move(marked.error()));
                    }
                    continue;
                }
            }
            if (auto verified = verify_cross_published_topology(record, *record.prepared_revision,
                                                                *record.target_revision);
                !verified) {
                if (auto marked = reconcile(record, std::move(verified.error())); !marked) {
                    return std::unexpected(std::move(marked.error()));
                }
                continue;
            }
            const auto recovered_result = commit_result(record, *record.target_revision);
            if (auto dependent = dependent_state_committer(recovered_result); !dependent) {
                const auto rolled_back = rollback_locked_cross_filesystem_target(
                    record, locked_target->source, locked_target->parent, *record.target_revision);
                const auto issue = rolled_back ? dependent.error() : rolled_back.error();
                if (auto terminal = cross_terminal_transition(
                        journal, record, State::target_published, record.prepared_revision,
                        record.target_revision, issue, rolled_back.has_value());
                    !terminal) {
                    return std::unexpected(std::move(terminal.error()));
                }
                results.push_back({
                    .journal_id = record.id,
                    .outcome = rolled_back ? FilePublicationRecoveryOutcome::rolled_back
                                           : FilePublicationRecoveryOutcome::needs_reconciliation,
                    .issue = rolled_back ? std::nullopt : std::optional<core::Error>{issue},
                });
                continue;
            }
            const auto source_name =
                std::filesystem::path{record.source_raw_path}.filename().native();
            const auto target_name =
                std::filesystem::path{record.target_raw_path}.filename().native();
            auto current_source = require_locked_source_entry(*locked_source, source_name, record);
            auto current_target =
                require_descriptor_entry(locked_target->source, locked_target->parent, target_name,
                                         *record.target_revision, record, "published target");
            if (!current_source || !current_target) {
                auto issue = !current_source ? std::move(current_source.error())
                                             : std::move(current_target.error());
                if (auto marked = reconcile(record, std::move(issue)); !marked) {
                    return std::unexpected(std::move(marked.error()));
                }
                continue;
            }
            if (auto dependent = cross_transition(journal, record, State::target_published,
                                                  State::dependent_state_committed,
                                                  record.prepared_revision, record.target_revision);
                !dependent) {
                return std::unexpected(std::move(dependent.error()));
            }
            record.state = State::dependent_state_committed;
        }

        if (record.state == State::dependent_state_committed) {
            source = observe_direct_revision(record.source_raw_path, record.source_raw_path,
                                             record.target_raw_path, record.id);
            prepared = observe_direct_revision(record.prepared_raw_path, record.source_raw_path,
                                               record.target_raw_path, record.id);
            target = observe_direct_revision(record.target_raw_path, record.source_raw_path,
                                             record.target_raw_path, record.id);
            if (!source || !prepared || !target) {
                auto issue = !source     ? std::move(source.error())
                             : !prepared ? std::move(prepared.error())
                                         : std::move(target.error());
                if (auto marked = reconcile(record, std::move(issue)); !marked) {
                    return std::unexpected(std::move(marked.error()));
                }
                continue;
            }
            if (!record.prepared_revision || !record.target_revision || *prepared || !*target ||
                **target != *record.target_revision ||
                (*source && **source != record.expected_source_revision)) {
                auto issue =
                    publication_error(core::ErrorCode::conflict,
                                      "Committed copy has ambiguous source-removal identities",
                                      record.source_raw_path, record.target_raw_path, record.id);
                if (auto marked = reconcile(record, std::move(issue)); !marked) {
                    return std::unexpected(std::move(marked.error()));
                }
                continue;
            }
            std::optional<LockedSource> locked_source;
            if (*source) {
                auto opened =
                    open_locked_source(record.source_raw_path, record.expected_source_revision,
                                       cancellation, record.target_raw_path, record.id);
                if (!opened) {
                    auto issue = std::move(opened.error());
                    if (issue.code == core::ErrorCode::cancelled) {
                        return std::unexpected(std::move(issue));
                    }
                    if (auto marked = reconcile(record, std::move(issue)); !marked) {
                        return std::unexpected(std::move(marked.error()));
                    }
                    continue;
                }
                locked_source.emplace(std::move(*opened));
            }
            auto locked_target =
                open_locked_source(record.target_raw_path, *record.target_revision, cancellation,
                                   record.source_raw_path, record.id);
            if (!locked_target) {
                auto issue = std::move(locked_target.error());
                if (issue.code == core::ErrorCode::cancelled) {
                    return std::unexpected(std::move(issue));
                }
                if (auto marked = reconcile(record, std::move(issue)); !marked) {
                    return std::unexpected(std::move(marked.error()));
                }
                continue;
            }
            if (locked_source) {
                if (preserves_source_bytes(record)) {
                    if (auto exact =
                            verify_exact_copy(*locked_source, *locked_target, record, cancellation);
                        !exact) {
                        auto issue = std::move(exact.error());
                        if (issue.code == core::ErrorCode::cancelled) {
                            return std::unexpected(std::move(issue));
                        }
                        if (auto marked = reconcile(record, std::move(issue)); !marked) {
                            return std::unexpected(std::move(marked.error()));
                        }
                        continue;
                    }
                }
                if (auto retained = retain_source_entry(
                        locked_source->source, locked_source->parent,
                        std::filesystem::path{record.source_raw_path}.filename().native(),
                        record.expected_source_revision, record);
                    !retained) {
                    return std::unexpected(std::move(retained.error()));
                }
            }
            if (auto verified =
                    verify_cross_source_removed_topology(record, *record.target_revision);
                !verified) {
                if (auto marked = reconcile(record, std::move(verified.error())); !marked) {
                    return std::unexpected(std::move(marked.error()));
                }
                continue;
            }
            if (auto removed = cross_transition(journal, record, State::dependent_state_committed,
                                                State::source_removed, record.prepared_revision,
                                                record.target_revision);
                !removed) {
                return std::unexpected(std::move(removed.error()));
            }
            record.state = State::source_removed;
        }

        if (record.state == State::source_removed) {
            if (!record.target_revision) {
                auto issue =
                    publication_error(core::ErrorCode::invariant,
                                      "Source-removed recovery is missing durable target evidence",
                                      record.source_raw_path, record.target_raw_path, record.id);
                if (auto marked = reconcile(record, std::move(issue)); !marked) {
                    return std::unexpected(std::move(marked.error()));
                }
                continue;
            }
            if (auto verified =
                    verify_cross_source_removed_topology(record, *record.target_revision);
                !verified) {
                if (auto marked = reconcile(record, std::move(verified.error())); !marked) {
                    return std::unexpected(std::move(marked.error()));
                }
                continue;
            }
            if (auto completed =
                    cross_transition(journal, record, State::source_removed, State::complete,
                                     record.prepared_revision, record.target_revision);
                !completed) {
                return std::unexpected(std::move(completed.error()));
            }
            results.push_back({.journal_id = record.id,
                               .outcome = FilePublicationRecoveryOutcome::completed,
                               .issue = std::nullopt});
            continue;
        }

        auto issue =
            publication_error(core::ErrorCode::invariant,
                              "Cross-filesystem recovery encountered an invalid journal state",
                              record.source_raw_path, record.target_raw_path, record.id);
        if (auto marked = reconcile(record, std::move(issue)); !marked) {
            return std::unexpected(std::move(marked.error()));
        }
    }
    return results;
}

namespace {

using BackupState = MetadataOperationBackupState;

[[nodiscard]] core::Result<void> mark_backup(FilePublicationJournal& journal,
                                             const core::StableId& id, const BackupState from,
                                             const BackupState to,
                                             std::optional<core::StableId> undo_id = std::nullopt,
                                             std::optional<core::Error> failure = std::nullopt) {
    return journal.transition_backup(id, FilePublicationBackupTransition{
                                             .expected_state = from,
                                             .state = to,
                                             .undo_id = std::move(undo_id),
                                             .failure = std::move(failure),
                                         });
}

// Removes `raw_path` when it is still `expected`.
[[nodiscard]] core::Result<void> remove_verified(const std::string& raw_path,
                                                 const core::LocalSourceRevision& expected,
                                                 const FilePublicationJournalRecord& record,
                                                 const core::CancellationToken& cancellation,
                                                 const std::string_view description) {
    auto locked =
        open_locked_source(raw_path, expected, cancellation, record.target_raw_path, record.id);
    if (!locked) {
        return std::unexpected(std::move(locked.error()));
    }
    return remove_descriptor_entry(locked->source, locked->parent,
                                   std::filesystem::path{raw_path}.filename().native(), expected,
                                   record, description);
}

// ADR-0266: where a retained source is kept -- moved where undo copies are,
// or beside the source it was -- and its identity there.
[[nodiscard]] std::filesystem::path kept_path_of(const FilePublicationBackupRecord& backup) {
    return backup.kept_raw_path.empty()
               ? file_publication_retained_path(backup.publication.source_raw_path,
                                                backup.publication.id)
               : std::filesystem::path{backup.kept_raw_path};
}

[[nodiscard]] core::LocalSourceRevision
kept_identity_of(const FilePublicationBackupRecord& backup) {
    return backup.kept_revision.value_or(backup.publication.expected_source_revision);
}

// Whether two files hold the same bytes: a source put back by copy, found
// after a crash beside the copy it was made from.
[[nodiscard]] bool same_bytes(const std::string& first, const std::string& second) {
    std::ifstream a{first, std::ios::binary};
    std::ifstream b{second, std::ios::binary};
    if (!a || !b) {
        return false;
    }
    std::array<char, 1U << 16U> left{};
    std::array<char, 1U << 16U> right{};
    while (true) {
        a.read(left.data(), left.size());
        b.read(right.data(), right.size());
        if (a.gcount() != b.gcount() ||
            !std::equal(left.begin(), left.begin() + a.gcount(), right.begin())) {
            return false;
        }
        if (a.gcount() == 0) {
            return a.eof() && b.eof();
        }
    }
}

// The undo of a retained publication from wherever it stands: the source
// back in place, then the target gone, then the state that follows files.
[[nodiscard]] core::Result<FilePublicationCommitResult>
finish_publication_undo(const FilePublicationBackupRecord& backup, FilePublicationJournal& journal,
                        const FilePublicationDependentStateCommitter& dependent_state_committer,
                        const core::CancellationToken& cancellation) {
    const auto& record = backup.publication;
    const auto retained = kept_path_of(backup);
    const auto kept_identity = kept_identity_of(backup);
    const auto reconcile = [&](core::Error issue) -> core::Result<FilePublicationCommitResult> {
        auto marked = mark_backup(journal, record.id, BackupState::undoing,
                                  BackupState::needs_reconciliation, backup.undo_id, issue);
        return std::unexpected(marked ? std::move(issue) : std::move(marked.error()));
    };
    auto source = observe_direct_revision(record.source_raw_path, record.source_raw_path,
                                          record.target_raw_path, record.id);
    auto kept = observe_direct_revision(retained.native(), record.source_raw_path,
                                        record.target_raw_path, record.id);
    if (!source || !kept) {
        return std::unexpected(!source ? std::move(source.error()) : std::move(kept.error()));
    }
    // Put back by a copy, where the source is kept on another filesystem:
    // the kept one stays until the undo is done.
    bool copied_back = false;
    if (!*source) {
        if (!*kept || !core::same_file_after_rename(retained.native(), kept_identity, **kept)) {
            return reconcile(publication_error(
                core::ErrorCode::conflict, "The retained source is gone or changed",
                record.source_raw_path, record.target_raw_path, record.id));
        }
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled(record.source_raw_path, record.target_raw_path));
        }
        const std::filesystem::path source_path{record.source_raw_path};
        auto source_parent =
            walk_directory(source_path.parent_path().native(), false, record.source_raw_path,
                           record.target_raw_path, record.id);
        if (!source_parent) {
            return std::unexpected(std::move(source_parent.error()));
        }
        struct stat kept_status{};
        struct stat parent_status{};
        if (::stat(retained.c_str(), &kept_status) != 0 ||
            ::fstat(source_parent->descriptor.get(), &parent_status) != 0) {
            return std::unexpected(system_error("Observing the retained source failed", errno,
                                                record.source_raw_path, record.target_raw_path,
                                                record.id));
        }
        std::string from_name = retained.filename().native();
        std::optional<Descriptor> from_parent;
        if (kept_status.st_dev == parent_status.st_dev) {
            auto parent = walk_directory(retained.parent_path().native(), false,
                                         record.source_raw_path, record.target_raw_path, record.id);
            if (!parent) {
                return std::unexpected(std::move(parent.error()));
            }
            from_parent.emplace(std::move(parent->descriptor));
        } else {
            // ADR-0266: copied beside the source, then renamed into place.
            from_name = ".trackknife-" + record.id.to_string() + ".restore";
            const auto staged = (source_path.parent_path() / from_name).native();
            struct stat existing{};
            if (::lstat(staged.c_str(), &existing) == 0 && S_ISREG(existing.st_mode)) {
                static_cast<void>(::unlink(staged.c_str()));
            }
            if (auto made = copy_file_verified(retained.native(), staged); !made) {
                return std::unexpected(std::move(made.error()));
            }
            copied_back = true;
        }
        const auto& from = from_parent ? *from_parent : source_parent->descriptor;
        if (auto restored = rename_no_replace(from, from_name, source_parent->descriptor,
                                              source_path.filename().native(), record);
            !restored) {
            if (copied_back) {
                static_cast<void>(::unlink((source_path.parent_path() / from_name).c_str()));
            }
            return std::unexpected(std::move(restored.error()));
        }
        if (::fsync(source_parent->descriptor.get()) != 0 ||
            (from_parent && ::fsync(from_parent->get()) != 0)) {
            return std::unexpected(system_error("Syncing the restored source failed", errno,
                                                record.source_raw_path, record.target_raw_path,
                                                record.id));
        }
    } else if (*kept && same_bytes(record.source_raw_path, retained.native())) {
        // Copied back before a crash; the kept one goes once the undo is done.
        copied_back = true;
    } else if (**source != record.expected_source_revision || *kept) {
        return reconcile(publication_error(
            core::ErrorCode::conflict, "The source path holds another file than the original",
            record.source_raw_path, record.target_raw_path, record.id));
    }
    // The source is back: the target goes, if it is still what was written.
    auto target = observe_direct_revision(record.target_raw_path, record.source_raw_path,
                                          record.target_raw_path, record.id);
    if (!target) {
        return std::unexpected(std::move(target.error()));
    }
    if (*target) {
        if (**target != *record.target_revision) {
            return reconcile(publication_error(
                core::ErrorCode::conflict, "The published file changed after it was written",
                record.source_raw_path, record.target_raw_path, record.id));
        }
        if (auto removed = remove_verified(record.target_raw_path, *record.target_revision, record,
                                           cancellation, "published target");
            !removed) {
            return std::unexpected(std::move(removed.error()));
        }
    }
    // The directories it made, deepest first, when nothing else is in them.
    for (auto made = record.planned_missing_directory_raw_paths.rbegin();
         made != record.planned_missing_directory_raw_paths.rend(); ++made) {
        static_cast<void>(::rmdir(made->c_str()));
    }
    FilePublicationCommitResult result{
        .journal_id = *backup.undo_id,
        .content = record.content,
        .source_raw_path = record.target_raw_path,
        .target_raw_path = record.source_raw_path,
        .source_revision = *record.target_revision,
        .target_revision = record.expected_source_revision,
        .occurrence_indexes = record.occurrence_indexes,
        .notes = {},
    };
    if (auto dependent = dependent_state_committer(result); !dependent) {
        // Still undoing: recovery follows the files again.
        return std::unexpected(std::move(dependent.error()));
    }
    if (copied_back) {
        if (auto removed = remove_verified(retained.native(), kept_identity, record, cancellation,
                                           "retained source");
            !removed) {
            return reconcile(removed.error());
        }
    }
    if (auto undone = mark_backup(journal, record.id, BackupState::undoing, BackupState::undone,
                                  backup.undo_id);
        !undone) {
        return std::unexpected(std::move(undone.error()));
    }
    return result;
}

} // namespace

core::Result<FilePublicationCommitResult>
undo_retained_publication(const core::StableId& journal_id, FilePublicationJournal& journal,
                          const FilePublicationDependentStateCommitter& dependent_state_committer,
                          const core::CancellationToken& cancellation) {
    if (journal_id.is_nil() || !dependent_state_committer) {
        return std::unexpected(publication_error(
            core::ErrorCode::invalid_argument,
            "Undoing a publication requires an operation and state committer", {}));
    }
    auto loaded = journal.load_backup(journal_id);
    if (!loaded) {
        return std::unexpected(std::move(loaded.error()));
    }
    if (!*loaded) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::not_found,
            .message = "The publication kept no source to restore",
            .context = {{"journal_id", journal_id.to_string()}},
        });
    }
    auto backup = std::move(**loaded);
    const auto& record = backup.publication;
    if (backup.state != BackupState::retained || record.state != State::complete ||
        !record.target_revision) {
        return std::unexpected(publication_error(
            core::ErrorCode::conflict, "The retained source is not available for undo",
            record.source_raw_path, record.target_raw_path, record.id));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(record.source_raw_path, record.target_raw_path));
    }
    // All as it was left, or nothing is touched.
    auto source = observe_direct_revision(record.source_raw_path, record.source_raw_path,
                                          record.target_raw_path, record.id);
    auto kept = observe_direct_revision(kept_path_of(backup).native(), record.source_raw_path,
                                        record.target_raw_path, record.id);
    auto target = observe_direct_revision(record.target_raw_path, record.source_raw_path,
                                          record.target_raw_path, record.id);
    if (!source || !kept || !target) {
        return std::unexpected(!source ? std::move(source.error())
                               : !kept ? std::move(kept.error())
                                       : std::move(target.error()));
    }
    if (*source || !*kept ||
        !core::same_file_after_rename(kept_path_of(backup).native(), kept_identity_of(backup),
                                      **kept) ||
        !*target || **target != *record.target_revision) {
        auto issue = publication_error(
            core::ErrorCode::conflict,
            "The files changed after the publication, so it cannot be undone safely",
            record.source_raw_path, record.target_raw_path, record.id);
        auto marked = mark_backup(journal, record.id, BackupState::retained,
                                  BackupState::needs_reconciliation, std::nullopt, issue);
        return std::unexpected(marked ? std::move(issue) : std::move(marked.error()));
    }
    backup.undo_id = core::StableId::random();
    if (auto begun = mark_backup(journal, record.id, BackupState::retained, BackupState::undoing,
                                 backup.undo_id);
        !begun) {
        return std::unexpected(std::move(begun.error()));
    }
    backup.state = BackupState::undoing;
    return finish_publication_undo(backup, journal, dependent_state_committer, cancellation);
}

core::Result<FilePublicationCommitResult>
undo_file_publication(const core::StableId& journal_id, FilePublicationJournal& journal,
                      const FilePublicationDependentStateCommitter& dependent_state_committer,
                      const core::CancellationToken& cancellation) {
    auto loaded = journal.load(journal_id);
    if (!loaded) {
        return std::unexpected(std::move(loaded.error()));
    }
    if (*loaded && publication_retains_source(**loaded)) {
        return undo_retained_publication(journal_id, journal, dependent_state_committer,
                                         cancellation);
    }
    return undo_same_filesystem_publication(journal_id, journal, dependent_state_committer,
                                            cancellation);
}

core::Result<void> release_publication_backup(const core::StableId& journal_id,
                                              FilePublicationJournal& journal,
                                              const core::CancellationToken& cancellation) {
    auto loaded = journal.load_backup(journal_id);
    if (!loaded) {
        return std::unexpected(std::move(loaded.error()));
    }
    if (!*loaded || (**loaded).state != BackupState::retained) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "The publication has no retained source to release",
            .context = {{"journal_id", journal_id.to_string()}},
        });
    }
    const auto& record = (**loaded).publication;
    const auto retained = kept_path_of(**loaded);
    const auto kept_identity = kept_identity_of(**loaded);
    auto kept = observe_direct_revision(retained.native(), record.source_raw_path,
                                        record.target_raw_path, record.id);
    if (!kept) {
        return std::unexpected(std::move(kept.error()));
    }
    if (*kept && !core::same_file_after_rename(retained.native(), kept_identity, **kept)) {
        auto issue = publication_error(core::ErrorCode::conflict,
                                       "The retained source has an unexpected identity",
                                       record.source_raw_path, record.target_raw_path, record.id);
        auto marked = mark_backup(journal, record.id, BackupState::retained,
                                  BackupState::needs_reconciliation, std::nullopt, issue);
        return std::unexpected(marked ? std::move(issue) : std::move(marked.error()));
    }
    if (*kept) {
        if (auto removed =
                remove_verified(retained.native(), **kept, record, cancellation, "retained source");
            !removed) {
            return std::unexpected(std::move(removed.error()));
        }
    }
    return mark_backup(journal, record.id, BackupState::retained, BackupState::released);
}

core::Result<std::vector<MetadataBackupMaintenanceResult>> maintain_publication_backups(
    FilePublicationJournal& journal, const MetadataBackupRetentionPolicy& policy,
    const std::int64_t now_unix_seconds, const core::CancellationToken& cancellation) {
    auto backups = journal.load_backups();
    if (!backups) {
        return std::unexpected(std::move(backups.error()));
    }
    std::vector<MetadataBackupMaintenanceResult> results;
    std::size_t kept = 0U;
    std::uint64_t kept_bytes = 0U;
    // Newest first: kept while within age, count and bytes.
    for (const auto& backup : *backups) {
        if (backup.state != BackupState::retained) {
            continue;
        }
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled({}));
        }
        const auto bytes = backup.publication.expected_source_revision.size;
        const auto young =
            now_unix_seconds - backup.completed_at_unix_seconds <= policy.maximum_age_seconds;
        if (young && kept < policy.maximum_entries &&
            kept_bytes + bytes <= policy.maximum_total_bytes) {
            ++kept;
            kept_bytes += bytes;
            results.push_back({.journal_id = backup.publication.id,
                               .outcome = MetadataBackupMaintenanceOutcome::retained,
                               .issue = std::nullopt});
            continue;
        }
        auto released = release_publication_backup(backup.publication.id, journal, cancellation);
        results.push_back({.journal_id = backup.publication.id,
                           .outcome = released
                                          ? MetadataBackupMaintenanceOutcome::released
                                          : MetadataBackupMaintenanceOutcome::needs_reconciliation,
                           .issue = released ? std::nullopt : std::optional{released.error()}});
    }
    return results;
}

core::Result<std::vector<FilePublicationRecoveryResult>>
recover_publication_undos(FilePublicationJournal& journal,
                          const FilePublicationDependentStateCommitter& dependent_state_committer,
                          const core::CancellationToken& cancellation) {
    auto backups = journal.load_backups();
    if (!backups) {
        return std::unexpected(std::move(backups.error()));
    }
    std::vector<FilePublicationRecoveryResult> results;
    for (const auto& backup : *backups) {
        if (backup.state != BackupState::undoing || !backup.undo_id ||
            !backup.publication.target_revision) {
            continue;
        }
        auto finished =
            finish_publication_undo(backup, journal, dependent_state_committer, cancellation);
        if (!finished && finished.error().code == core::ErrorCode::cancelled) {
            return std::unexpected(std::move(finished.error()));
        }
        results.push_back({.journal_id = backup.publication.id,
                           .outcome = finished
                                          ? FilePublicationRecoveryOutcome::completed
                                          : FilePublicationRecoveryOutcome::needs_reconciliation,
                           .issue = finished ? std::nullopt : std::optional{finished.error()}});
    }
    return results;
}

} // namespace trackknife::operations
