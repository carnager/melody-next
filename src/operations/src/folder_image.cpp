// SPDX-License-Identifier: GPL-3.0-only
#include "trackknife/core/posix.hpp"

#include "trackknife/operations/metadata_commit.hpp"

#include "backup_links.hpp"
#include "trackknife/core/atomic_rename.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits.h>
#include <mutex>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace trackknife::operations {
namespace {
using State = MetadataOperationJournalState;
std::mutex folder_mutex;

struct Descriptor {
    int fd{-1};
    explicit Descriptor(int value) : fd(value) {}
    ~Descriptor() {
        if (fd >= 0)
            ::close(fd);
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
};
core::Error error(std::string message) {
    return {.code = core::ErrorCode::conflict, .message = std::move(message), .context = {}};
}
core::Error io_error() { return error(std::string{"Folder image: "} + std::strerror(errno)); }

// Open every directory component without following symlinks, retaining the
// directory descriptor for all mutations. Never reinterpret a raw path as UTF-8.
core::Result<int> open_parent(const std::string& path) {
    const auto parent = std::filesystem::path{path}.parent_path();
    if (!parent.is_absolute())
        return std::unexpected(error("Folder image paths must be absolute"));
    int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return std::unexpected(io_error());
    for (const auto& component : parent.relative_path()) {
        if (component == "." || component == ".." || component.empty()) {
            ::close(fd);
            return std::unexpected(error("Folder image paths must not traverse directories"));
        }
        const int next =
            ::openat(fd, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ::close(fd);
        if (next < 0)
            return std::unexpected(io_error());
        fd = next;
    }
    return fd;
}
std::string basename(const std::string& path) {
    return std::filesystem::path{path}.filename().native();
}
std::string descriptor_path(int fd, const std::string& name) {
#ifdef __APPLE__
    std::array<char, PATH_MAX> path{};
    if (::fcntl(fd, F_GETPATH, path.data()) == 0)
        return (std::filesystem::path{path.data()} / name).native();
    return "/dev/fd/" + std::to_string(fd) + "/" + name;
#else
    return "/proc/self/fd/" + std::to_string(fd) + "/" + name;
#endif
}
core::Result<std::optional<core::LocalSourceRevision>> revision(int fd, const std::string& name) {
    struct stat st{};
    if (::fstatat(fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT)
            return std::nullopt;
        return std::unexpected(io_error());
    }
    if (!S_ISREG(st.st_mode))
        return std::unexpected(error("Folder image target is not a regular file"));
    auto value = core::observe_local_source_revision(descriptor_path(fd, name));
    if (!value)
        return std::unexpected(value.error());
    return *value;
}
core::Result<void> step(MetadataOperationJournal& journal, MetadataOperationJournalRecord& record,
                        State state) {
    auto result = journal.transition(
        record.id,
        {.expected_state = record.state,
         .state = state,
         .prepared_revision = record.prepared_revision,
         .published_revision = record.published_revision,
         .failure = state == State::rolled_back
                        ? std::optional{error("Interrupted folder image publication rolled back")}
                        : std::nullopt});
    if (result)
        record.state = state;
    return result;
}
core::Result<void> remove_matching(int fd, const std::string& name,
                                   const std::optional<core::LocalSourceRevision>& first,
                                   const std::optional<core::LocalSourceRevision>& second = {}) {
    auto found = revision(fd, name);
    if (!found)
        return std::unexpected(found.error());
    if (!*found)
        return {};
    if ((!first || **found != *first) && (!second || **found != *second))
        return std::unexpected(error("Folder image recovery retained an unrecognized artifact"));
    if (::unlinkat(fd, name.c_str(), 0) != 0)
        return std::unexpected(io_error());
    return {};
}
// The folder is locked against other saves. Where the filesystem has no
// flock (some SMB and FUSE mounts), the in-process lock and the revision
// checks before every step carry it, as for media files (ADR-0111).
core::Result<void> lock_directory(int fd) {
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0 || errno == ENOLCK || errno == ENOTSUP ||
        errno == EOPNOTSUPP)
        return {};
    return std::unexpected(io_error());
}
bool read_all(int fd, std::vector<char>& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count =
            ::pread(fd, bytes.data() + offset, bytes.size() - offset, static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        offset += static_cast<std::size_t>(count);
    }
    char extra = 0;
    return ::pread(fd, &extra, 1, static_cast<off_t>(offset)) == 0;
}
// ADR-0248: the backup as a verified copy where the filesystem refuses the
// hard link, with the original's permissions and time.
core::Result<core::LocalSourceRevision> copy_backup(int fd, const std::string& name,
                                                    const std::string& backup) {
    constexpr off_t limit = 256 * 1024 * 1024;
    Descriptor source{::openat(fd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)};
    struct stat status{};
    if (source.fd < 0 || ::fstat(source.fd, &status) != 0)
        return std::unexpected(io_error());
    if (status.st_size < 0 || status.st_size > limit)
        return std::unexpected(error("Folder image is too large to back up by copy"));
    std::vector<char> original(static_cast<std::size_t>(status.st_size));
    if (!read_all(source.fd, original))
        return std::unexpected(error("Folder image changed while backed up"));
    Descriptor copy{
        ::openat(fd, backup.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)};
    if (copy.fd < 0)
        return std::unexpected(io_error());
    const auto discard = [&](core::Error issue) -> core::Result<core::LocalSourceRevision> {
        static_cast<void>(::unlinkat(fd, backup.c_str(), 0));
        return std::unexpected(std::move(issue));
    };
    for (std::size_t offset = 0; offset < original.size();) {
        const auto count = ::write(copy.fd, original.data() + offset, original.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return discard(io_error());
        offset += static_cast<std::size_t>(count);
    }
    if (::fchmod(copy.fd, status.st_mode & 07777) != 0 || ::fsync(copy.fd) != 0)
        return discard(io_error());
    const int written = std::exchange(copy.fd, -1);
    if (::close(written) != 0)
        return discard(io_error());
    // By path, after the close: the time an SMB mount then keeps. sshfs
    // refuses it, and the copy keeps its own.
    const std::array times{status.st_atim, status.st_mtim};
    static_cast<void>(::utimensat(fd, backup.c_str(), times.data(), AT_SYMLINK_NOFOLLOW));
    Descriptor reread{::openat(fd, backup.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)};
    std::vector<char> copied(original.size());
    struct stat after{};
    if (reread.fd < 0 || !read_all(reread.fd, copied) || copied != original ||
        ::fstat(source.fd, &after) != 0 || after.st_size != status.st_size ||
        after.st_mtim.tv_sec != status.st_mtim.tv_sec ||
        after.st_mtim.tv_nsec != status.st_mtim.tv_nsec)
        return discard(error("Folder image backup copy failed verification"));
    auto identity = revision(fd, backup);
    if (!identity || !*identity)
        return discard(!identity ? identity.error() : error("Folder image backup vanished"));
    return **identity;
}
// Filesystems without renameat2 exchange (SMB, CIFS, FUSE) refuse with these.
bool exchange_refused(int number) {
    return number == EINVAL || number == ENOTSUP || number == EOPNOTSUPP || number == ENOSYS;
}
core::Result<MetadataRecoveryResult> recover_locked(MetadataOperationJournalRecord record,
                                                    MetadataOperationJournal& journal, int fd) {
    const auto target = basename(record.source_raw_path);
    const auto prepared = basename(record.prepared_raw_path);
    const auto backup = basename(record.backup_raw_path);
    const bool existed = record.changes.front().original_present;
    const auto original = existed ? std::optional{record.expected_revision} : std::nullopt;
    // The backup: the original's own inode, or a copy of it (ADR-0248).
    const auto kept = existed ? std::optional{backup_identity(record)} : std::nullopt;
    auto current = revision(fd, target);
    auto old = revision(fd, backup);
    const auto reconcile = [&](core::Error issue) -> core::Result<MetadataRecoveryResult> {
        auto changed =
            journal.transition(record.id, {.expected_state = record.state,
                                           .state = State::needs_reconciliation,
                                           .prepared_revision = record.prepared_revision,
                                           .published_revision = record.published_revision,
                                           .failure = issue});
        if (!changed)
            return std::unexpected(changed.error());
        return MetadataRecoveryResult{.journal_id = record.id,
                                      .outcome = MetadataRecoveryOutcome::needs_reconciliation,
                                      .issue = std::move(issue)};
    };
    if (!current || !old)
        return reconcile(!current ? current.error() : old.error());
    const bool unchanged = *current == original;
    const bool published =
        *current && record.prepared_revision &&
        // Renamed into place, perhaps renumbered by that rename (ADR-0249).
        core::same_file_after_rename(record.source_raw_path, *record.prepared_revision, **current);
    if (!unchanged && !published)
        return reconcile(error("Folder image changed outside this operation"));
    if (published) {
        if (*old != kept)
            return reconcile(error("Folder image recovery backup is missing or changed"));
        if (existed) {
            auto original_image = metadata::read_artwork_image_file(descriptor_path(fd, backup));
            if (!original_image || record.changes.front().original_values !=
                                       std::vector<std::string>{metadata::artwork_fingerprint_hex(
                                           original_image->content_fingerprint)})
                return reconcile(error("Folder image backup failed hash verification"));
        }
        auto image = metadata::read_artwork_image_file(descriptor_path(fd, target));
        if (!image || metadata::artwork_fingerprint_hex(image->content_fingerprint) !=
                          record.changes.front().planned_values.front())
            return reconcile(error("Published folder image failed hash verification"));
        auto cleaned = remove_matching(fd, prepared, original);
        if (!cleaned)
            return reconcile(cleaned.error());
        if (::fsync(fd) != 0)
            return reconcile(io_error());
        record.published_revision = **current;
        if (record.state == State::prepared) {
            auto advanced = step(journal, record, State::published);
            if (!advanced)
                return std::unexpected(advanced.error());
        }
        auto completed = step(journal, record, State::complete);
        if (!completed)
            return std::unexpected(completed.error());
        return MetadataRecoveryResult{.journal_id = record.id,
                                      .outcome = MetadataRecoveryOutcome::completed,
                                      .issue = std::nullopt};
    }
    // A crash before recording a prepared revision leaves unknown bytes. Keep
    // those artifacts for reconciliation instead of guessing their ownership.
    auto cleaned = remove_matching(fd, prepared, record.prepared_revision, original);
    if (!cleaned)
        return reconcile(cleaned.error());
    if (record.state == State::prepared && existed && !record.backup_revision) {
        // A copy made but not yet journaled (ADR-0248): the image is
        // untouched, and the copy at this operation's own path is its debris.
        if (::unlinkat(fd, backup.c_str(), 0) != 0 && errno != ENOENT)
            return reconcile(io_error());
    } else {
        cleaned = remove_matching(fd, backup, kept);
        if (!cleaned)
            return reconcile(cleaned.error());
    }
    if (::fsync(fd) != 0)
        return reconcile(io_error());
    auto rolled_back = step(journal, record, State::rolled_back);
    if (!rolled_back)
        return std::unexpected(rolled_back.error());
    return MetadataRecoveryResult{.journal_id = record.id,
                                  .outcome = MetadataRecoveryOutcome::rolled_back,
                                  .issue = std::nullopt};
}
} // namespace

core::Result<MetadataRecoveryResult>
recover_folder_image(const MetadataOperationJournalRecord& record,
                     MetadataOperationJournal& journal,
                     const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested())
        return std::unexpected(error("Folder image recovery cancelled"));
    const std::scoped_lock lock{folder_mutex};
    const auto parent = std::filesystem::path{record.source_raw_path}.parent_path();
    const auto stem = ".trackknife-" + record.id.to_string() + ".metadata-";
    if (record.content_kind != MetadataOperationContentKind::folder_image ||
        record.changes.size() != 1 || record.changes.front().planned_values.size() != 1 ||
        record.prepared_raw_path != (parent / (stem + "prepared")).native() ||
        record.backup_raw_path != (parent / (stem + "backup")).native())
        return std::unexpected(error("Invalid folder image recovery evidence"));
    auto opened = open_parent(record.source_raw_path);
    if (!opened)
        return std::unexpected(opened.error());
    Descriptor directory{*opened};
    if (auto locked = lock_directory(directory.fd); !locked)
        return std::unexpected(locked.error());
    return recover_locked(record, journal, directory.fd);
}

core::Result<MetadataCommitResult>
commit_folder_image(const metadata::FolderImageWritePlan& plan, MetadataOperationJournal& journal,
                    const core::CancellationToken& cancellation) {
    const std::scoped_lock lock{folder_mutex};
    if (cancellation.is_cancellation_requested())
        return std::unexpected(core::Error{.code = core::ErrorCode::cancelled,
                                           .message = "Folder image publication cancelled",
                                           .context = {}});
    if (plan.raw_path.empty() || plan.raw_path.find('\0') != std::string::npos)
        return std::unexpected(error("Invalid folder image path"));
    auto opened = open_parent(plan.raw_path);
    if (!opened)
        return std::unexpected(opened.error());
    Descriptor directory{*opened};
    if (auto locked = lock_directory(directory.fd); !locked)
        return std::unexpected(locked.error());
    const auto name = basename(plan.raw_path);
    auto current = revision(directory.fd, name);
    if (!current)
        return std::unexpected(current.error());
    auto incomplete = journal.load_incomplete_for_source(plan.raw_path);
    if (!incomplete)
        return std::unexpected(incomplete.error());
    for (const auto& entry : *incomplete)
        if (entry.source_raw_path == plan.raw_path)
            return std::unexpected(error("Folder image requires recovery before another save"));
    // Repeated tracks in the same folder share a destination. A previous
    // successful publication of exactly these bytes is an idempotent success.
    if (*current) {
        auto image = metadata::read_artwork_image_file(descriptor_path(directory.fd, name));
        if (!image)
            return std::unexpected(image.error());
        if (image->content_fingerprint == plan.image.content_fingerprint) {
            return MetadataCommitResult{.journal_id = {},
                                        .source_raw_path = plan.raw_path,
                                        .backup_raw_path = {},
                                        .previous_revision = **current,
                                        .published_revision = **current,
                                        .document = {},
                                        .occurrence_indexes = {},
                                        .content_kind = MetadataOperationContentKind::folder_image};
        }
        if (!plan.original || image->source_revision != plan.original->source_revision ||
            image->content_fingerprint != plan.original->content_fingerprint)
            return std::unexpected(error("Folder image changed after review"));
        struct stat st{};
        if (::fstatat(directory.fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 ||
            st.st_nlink != 1)
            return std::unexpected(
                error("Folder image replacement requires a single-link regular file"));
    } else if (plan.original)
        return std::unexpected(error("Folder image vanished after review"));
    auto bytes = metadata::read_artwork_image_bytes(plan.image, 16U * 1024U * 1024U, cancellation);
    if (!bytes)
        return std::unexpected(bytes.error());
    MetadataOperationJournalRecord record;
    record.id = core::StableId::random();
    record.source_raw_path = plan.raw_path;
    const auto parent = std::filesystem::path{plan.raw_path}.parent_path();
    const auto stem = ".trackknife-" + record.id.to_string() + ".metadata-";
    record.prepared_raw_path = (parent / (stem + "prepared")).native();
    record.backup_raw_path = (parent / (stem + "backup")).native();
    record.expected_revision = current->value_or(core::LocalSourceRevision{});
    record.occurrence_indexes = {0};
    record.content_kind = MetadataOperationContentKind::folder_image;
    record.changes.push_back(
        {.field_index = 0,
         .canonical_name = "folderimage",
         .property_name = "folderimage",
         .original_present = current->has_value(),
         .original_values = plan.original
                                ? std::vector<std::string>{metadata::artwork_fingerprint_hex(
                                      plan.original->content_fingerprint)}
                                : std::vector<std::string>{},
         .kind = metadata::StagedMetadataPatchKind::replace_values,
         .planned_values = {metadata::artwork_fingerprint_hex(plan.image.content_fingerprint)},
         .item_indexes = {0},
         .exact_native_name = std::nullopt});
    auto created = journal.create(record);
    if (!created)
        return std::unexpected(created.error());
    const auto prepared = basename(record.prepared_raw_path);
    const auto backup = basename(record.backup_raw_path);
    Descriptor output{::openat(directory.fd, prepared.c_str(),
                               O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644)};
    const auto fail = [&](core::Error issue) -> core::Result<MetadataCommitResult> {
        auto recovered = recover_locked(record, journal, directory.fd);
        if (!recovered)
            return std::unexpected(recovered.error());
        return std::unexpected(std::move(issue));
    };
    if (output.fd < 0)
        return fail(io_error());
    std::size_t offset = 0;
    while (offset < bytes->size()) {
        const auto count = ::write(output.fd, bytes->data() + offset, bytes->size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return fail(io_error());
        offset += static_cast<std::size_t>(count);
    }
    if (*current) {
        struct stat original_status{};
        if (::fstatat(directory.fd, name.c_str(), &original_status, AT_SYMLINK_NOFOLLOW) != 0 ||
            ::fchmod(output.fd, original_status.st_mode & 0777) != 0)
            return fail(io_error());
    }
    if (::fsync(output.fd) != 0)
        return fail(io_error());
    auto prepared_revision = revision(directory.fd, prepared);
    if (!prepared_revision || !*prepared_revision)
        return fail(error("Prepared folder image vanished"));
    record.prepared_revision = **prepared_revision;
    if (::fsync(directory.fd) != 0)
        return fail(io_error());
    auto advanced = step(journal, record, State::prepared);
    if (!advanced)
        return fail(advanced.error());
    if (cancellation.is_cancellation_requested())
        return fail(core::Error{.code = core::ErrorCode::cancelled,
                                .message = "Folder image publication cancelled",
                                .context = {}});
    auto fresh = revision(directory.fd, name);
    if (!fresh || *fresh != *current)
        return fail(error("Folder image changed before publication"));
    const bool without_links = detail::filesystem_without_links_simulated();
    if (*current) {
        if (without_links ||
            ::linkat(directory.fd, name.c_str(), directory.fd, backup.c_str(), 0) != 0) {
            if (!without_links && !detail::hard_link_refused(errno))
                return fail(io_error());
            auto copied = copy_backup(directory.fd, name, backup);
            if (!copied)
                return fail(copied.error());
            auto recorded =
                journal.transition(record.id, {.expected_state = State::prepared,
                                               .state = State::prepared,
                                               .prepared_revision = record.prepared_revision,
                                               .published_revision = std::nullopt,
                                               .failure = std::nullopt,
                                               .backup_revision = *copied});
            if (!recorded) {
                static_cast<void>(::unlinkat(directory.fd, backup.c_str(), 0));
                return fail(recorded.error());
            }
            record.backup_revision = *copied;
        }
        if (::fsync(directory.fd) != 0)
            return fail(io_error());
        const bool exchanged =
            !without_links && core::rename_with_flags(directory.fd, prepared.c_str(), directory.fd,
                                          name.c_str(), RENAME_EXCHANGE) == 0;
        if (exchanged) {
            auto displaced = revision(directory.fd, prepared);
            if (!displaced || *displaced != *current) {
                // Keep all evidence; never replace an unrecognized raced-in file.
                return fail(error("Folder image changed during publication; recovery required"));
            }
        } else {
            if (!without_links && !exchange_refused(errno))
                return fail(io_error());
            // No exchange here: with the folder locked and the image checked
            // once more, a plain rename replaces it atomically, as a tag save
            // publishes; the backup holds the original either way.
            auto again = revision(directory.fd, name);
            if (!again || *again != *current)
                return fail(error("Folder image changed during publication; recovery required"));
            if (::renameat(directory.fd, prepared.c_str(), directory.fd, name.c_str()) != 0)
                return fail(io_error());
        }
    } else if (auto published =
                   core::publish_no_replace_at(directory.fd, prepared, directory.fd, name);
               !published) {
        return fail(published.error());
    }
    auto recovered = recover_locked(record, journal, directory.fd);
    if (!recovered)
        return std::unexpected(recovered.error());
    if (recovered->outcome != MetadataRecoveryOutcome::completed)
        return std::unexpected(
            recovered->issue.value_or(error("Folder image publication did not complete")));
    return MetadataCommitResult{.journal_id = record.id,
                                .source_raw_path = plan.raw_path,
                                .backup_raw_path = record.backup_raw_path,
                                .previous_revision = record.expected_revision,
                                .published_revision = *record.prepared_revision,
                                .document = {},
                                .occurrence_indexes = {},
                                .content_kind = MetadataOperationContentKind::folder_image};
}
} // namespace trackknife::operations
