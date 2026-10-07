// SPDX-License-Identifier: GPL-3.0-only
#include "trackknife/operations/undo_copies.hpp"

#include "trackknife/operations/file_publication_journal.hpp"
#include "trackknife/operations/metadata_journal.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

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

namespace {

[[nodiscard]] core::Error io_failure(const std::string& what, const std::string& path) {
    return core::Error{.code = core::ErrorCode::io,
                       .message = what + ": " + std::strerror(errno),
                       .context = {{.key = "path", .value = path}}};
}

class File final {
  public:
    explicit File(const int descriptor) : descriptor_(descriptor) {}
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    ~File() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }
    [[nodiscard]] int get() const { return descriptor_; }
    [[nodiscard]] bool valid() const { return descriptor_ >= 0; }
    [[nodiscard]] bool close() { return ::close(std::exchange(descriptor_, -1)) == 0; }

  private:
    int descriptor_;
};

[[nodiscard]] ssize_t read_some(const int descriptor, char* data, const std::size_t size,
                                const off_t offset) {
    ssize_t count = -1;
    do {
        count = ::pread(descriptor, data, size, offset);
    } while (count < 0 && errno == EINTR);
    return count;
}

void sync_folder(const std::string& path) {
    File folder{::open(std::filesystem::path{path}.parent_path().c_str(),
                       O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (folder.valid()) {
        static_cast<void>(::fsync(folder.get()));
    }
}

[[nodiscard]] core::Result<std::optional<core::LocalSourceRevision>>
observed(const std::string& path) {
    auto revision = core::observe_local_source_revision(path);
    if (revision) {
        return std::optional{*revision};
    }
    if (revision.error().code == core::ErrorCode::not_found) {
        return std::optional<core::LocalSourceRevision>{};
    }
    return std::unexpected(std::move(revision.error()));
}

// The same file a copy was made of: as big, as old (copies keep the time).
[[nodiscard]] bool copy_of(const core::LocalSourceRevision& found,
                           const core::LocalSourceRevision& kept) {
    return found.size == kept.size &&
           found.modification_time_seconds == kept.modification_time_seconds &&
           found.modification_time_nanoseconds == kept.modification_time_nanoseconds;
}

struct Moved {
    std::string path;
    core::LocalSourceRevision revision;
    // Copied, so the old one is still to be removed once this is recorded.
    bool copied{false};
};

// A kept copy at `from`, with identity `kept`, to `to`.
[[nodiscard]] core::Result<std::optional<Moved>>
move_kept(const std::string& from, const core::LocalSourceRevision& kept, const std::string& to) {
    auto found = observed(from);
    if (!found) {
        return std::unexpected(std::move(found.error()));
    }
    if (!*found) {
        return std::optional<Moved>{};
    }
    if (!core::same_file_after_rename(from, kept, **found)) {
        return std::unexpected(core::Error{.code = core::ErrorCode::conflict,
                                           .message = "an undo copy has changed",
                                           .context = {{.key = "path", .value = from}}});
    }
    // Named for its operation: whatever is there is this one's, from a crash.
    struct stat existing{};
    if (::lstat(to.c_str(), &existing) == 0 && S_ISREG(existing.st_mode)) {
        static_cast<void>(::unlink(to.c_str()));
    }
    if (::rename(from.c_str(), to.c_str()) == 0) {
        sync_folder(from);
        sync_folder(to);
        auto revision = core::observe_local_source_revision(to);
        if (!revision) {
            return std::unexpected(std::move(revision.error()));
        }
        return std::optional{Moved{.path = to, .revision = *revision, .copied = false}};
    }
    if (errno != EXDEV) {
        return std::unexpected(io_failure("moving an undo copy failed", from));
    }
    auto copied = copy_file_verified(from, to);
    if (!copied) {
        return std::unexpected(std::move(copied.error()));
    }
    return std::optional{Moved{.path = to, .revision = *copied, .copied = true}};
}

// The old place of a copy now recorded at `kept_path`, emptied: the
// original renamed away already, or a copy of it removed.
void remove_old(const std::string& old_path, const std::string& kept_path,
                const core::LocalSourceRevision& kept) {
    if (old_path == kept_path) {
        return;
    }
    auto found = observed(old_path);
    if (found && *found && copy_of(**found, kept)) {
        if (::unlink(old_path.c_str()) == 0) {
            sync_folder(old_path);
        }
    }
}

} // namespace

core::Result<core::LocalSourceRevision> copy_file_verified(const std::string& from,
                                                           const std::string& to) {
    File source{::open(from.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    struct stat status{};
    if (!source.valid() || ::fstat(source.get(), &status) != 0) {
        return std::unexpected(io_failure("opening an undo copy failed", from));
    }
    File copy{::open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (!copy.valid()) {
        return std::unexpected(io_failure("creating an undo copy failed", to));
    }
    const auto discard = [&to](core::Error issue) -> core::Result<core::LocalSourceRevision> {
        static_cast<void>(::unlink(to.c_str()));
        return std::unexpected(std::move(issue));
    };
    constexpr std::size_t chunk = 1U << 20U;
    std::vector<char> buffer(chunk);
    std::vector<char> compared(chunk);
    const auto size = static_cast<off_t>(status.st_size);
    for (off_t offset = 0; offset < size;) {
        const auto wanted = static_cast<std::size_t>(std::min<off_t>(size - offset, chunk));
        const auto count = read_some(source.get(), buffer.data(), wanted, offset);
        if (count <= 0) {
            return discard(io_failure("reading for an undo copy failed", from));
        }
        for (ssize_t written = 0; written < count;) {
            const auto step = ::write(copy.get(), buffer.data() + written,
                                      static_cast<std::size_t>(count - written));
            if (step < 0 && errno == EINTR) {
                continue;
            }
            if (step <= 0) {
                return discard(io_failure("writing an undo copy failed", to));
            }
            written += step;
        }
        offset += count;
    }
    if (::fchmod(copy.get(), status.st_mode & 07777) != 0 || ::fsync(copy.get()) != 0 ||
        !copy.close()) {
        return discard(io_failure("finishing an undo copy failed", to));
    }
    const std::array times{status.st_atim, status.st_mtim};
    static_cast<void>(::utimensat(AT_FDCWD, to.c_str(), times.data(), AT_SYMLINK_NOFOLLOW));
    File written{::open(to.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (!written.valid()) {
        return discard(io_failure("reopening an undo copy failed", to));
    }
    for (off_t offset = 0; offset <= size;) {
        const auto wanted = static_cast<std::size_t>(std::min<off_t>(size - offset + 1, chunk));
        const auto original = read_some(source.get(), buffer.data(), wanted, offset);
        const auto copied = read_some(written.get(), compared.data(), wanted, offset);
        if (original < 0 || copied < 0 || original != copied ||
            !std::equal(buffer.begin(), buffer.begin() + original, compared.begin())) {
            return discard(core::Error{.code = core::ErrorCode::conflict,
                                       .message = "an undo copy differs from its original",
                                       .context = {{.key = "path", .value = to}}});
        }
        if (original == 0) {
            break;
        }
        offset += original;
    }
    sync_folder(to);
    auto identity = core::observe_local_source_revision(to);
    if (!identity) {
        return discard(std::move(identity.error()));
    }
    return *identity;
}

core::Result<std::size_t> keep_undo_copies_in_place(MetadataOperationJournal& metadata,
                                                    FilePublicationJournal& files) {
    std::size_t moved = 0U;
    std::optional<core::Error> first_issue;
    const auto remember = [&first_issue](core::Error issue) {
        if (!first_issue) {
            first_issue = std::move(issue);
        }
    };
    // One copy: to `target`, recorded by `record`, the old place emptied.
    const auto relocate = [&](const std::string& current, const core::LocalSourceRevision& kept,
                              const std::string& target, const std::string& beside,
                              const auto& record) {
        if (current == target) {
            // In place; a copy a crash left at its old place goes.
            remove_old(beside, current, kept);
            return;
        }
        auto taken = move_kept(current, kept, target);
        if (!taken) {
            remember(std::move(taken.error()));
            return;
        }
        if (!*taken) {
            return;
        }
        if (auto recorded = record((**taken).path, (**taken).revision); !recorded) {
            // Not recorded: back as it was.
            if ((**taken).copied) {
                static_cast<void>(::unlink((**taken).path.c_str()));
            } else {
                static_cast<void>(::rename((**taken).path.c_str(), current.c_str()));
            }
            remember(std::move(recorded.error()));
            return;
        }
        if ((**taken).copied) {
            remove_old(current, (**taken).path, (**taken).revision);
        }
        ++moved;
    };

    auto backups = metadata.load_backups();
    if (!backups) {
        return std::unexpected(std::move(backups.error()));
    }
    for (const auto& backup : *backups) {
        if (backup.state != MetadataOperationBackupState::retained) {
            continue;
        }
        const auto& record = backup.operation;
        const auto name = std::filesystem::path{record.backup_raw_path}.filename().native();
        const auto beside =
            (std::filesystem::path{record.source_raw_path}.parent_path() / name).native();
        relocate(record.backup_raw_path, backup_identity(record),
                 undo_copy_path(record.source_raw_path, name), beside,
                 [&](const std::string& path, const core::LocalSourceRevision& revision) {
                     return metadata.relocate_backup(record.id, path, revision);
                 });
    }

    auto retained = files.load_backups();
    if (!retained) {
        return std::unexpected(std::move(retained.error()));
    }
    for (const auto& backup : *retained) {
        if (backup.state != MetadataOperationBackupState::retained) {
            continue;
        }
        const auto& record = backup.publication;
        const auto beside = file_publication_retained_path(record.source_raw_path, record.id);
        const auto current = backup.kept_raw_path.empty() ? beside.native() : backup.kept_raw_path;
        relocate(current, backup.kept_revision.value_or(record.expected_source_revision),
                 undo_copy_path(record.source_raw_path, beside.filename().native()),
                 beside.native(),
                 [&](const std::string& path, const core::LocalSourceRevision& revision) {
                     return files.relocate_kept(record.id, path, revision);
                 });
    }
    if (first_issue) {
        return std::unexpected(std::move(*first_issue));
    }
    return moved;
}

} // namespace trackknife::operations
