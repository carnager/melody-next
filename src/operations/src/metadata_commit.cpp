// SPDX-License-Identifier: GPL-3.0-only
#include "trackknife/core/posix.hpp"

#include "trackknife/operations/metadata_commit.hpp"

#include "backup_links.hpp"

#include "trackknife/formats/cue_sheet.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/metadata/artwork_writers.hpp"
#include "trackknife/metadata/flac_mapping.hpp"
#include "trackknife/metadata/flac_writer.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/loudness_sidecar.hpp"
#include "trackknife/metadata/mp3_writer.hpp"
#include "trackknife/operations/cue_replay_gain_apply.hpp"
#include "trackknife/operations/loudness_sidecar_apply.hpp"
#include "trackknife/operations/undo_copies.hpp"

#include <charconv>
#include <fstream>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#ifndef __APPLE__
#include <sys/syscall.h>
#endif
#include <sys/types.h>
#include <sys/xattr.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1U << 1U)
#endif

namespace trackknife::operations {
namespace {

constexpr std::size_t maximum_xattr_name_bytes = 1U * 1024U * 1024U;
constexpr std::size_t maximum_xattr_count = 4'096U;
constexpr std::size_t maximum_xattr_value_bytes = 16U * 1024U * 1024U;
constexpr std::size_t maximum_total_xattr_bytes = 64U * 1024U * 1024U;
constexpr auto lock_retry_interval = std::chrono::milliseconds{10};

using State = MetadataOperationJournalState;
using BackupState = MetadataOperationBackupState;

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
    [[nodiscard]] bool close() noexcept {
        if (!valid()) {
            return true;
        }
        const auto value = std::exchange(value_, -1);
        return ::close(value) == 0;
    }

  private:
    int value_{-1};
};

struct PhysicalKey {
    std::uint64_t device{0U};
    std::uint64_t inode{0U};

    friend bool operator==(const PhysicalKey&, const PhysicalKey&) = default;
};

struct PhysicalKeyHash {
    [[nodiscard]] std::size_t operator()(const PhysicalKey& key) const noexcept {
        const auto first = std::hash<std::uint64_t>{}(key.device);
        const auto second = std::hash<std::uint64_t>{}(key.inode);
        return first ^ (second + 0x9E3779B9U + (first << 6U) + (first >> 2U));
    }
};

class ProcessSourceLock final {
  public:
    ProcessSourceLock(std::shared_ptr<std::timed_mutex> mutex,
                      std::unique_lock<std::timed_mutex> lock)
        : mutex_{std::move(mutex)}, lock_{std::move(lock)} {}
    ProcessSourceLock(ProcessSourceLock&&) noexcept = default;
    ProcessSourceLock& operator=(ProcessSourceLock&&) noexcept = default;
    ProcessSourceLock(const ProcessSourceLock&) = delete;
    ProcessSourceLock& operator=(const ProcessSourceLock&) = delete;

  private:
    std::shared_ptr<std::timed_mutex> mutex_;
    std::unique_lock<std::timed_mutex> lock_;
};

std::mutex process_lock_registry_mutex;
std::unordered_map<PhysicalKey, std::weak_ptr<std::timed_mutex>, PhysicalKeyHash>
    process_lock_registry;

[[nodiscard]] core::Error
operation_error(const core::ErrorCode code, std::string message, const std::string& source_raw_path,
                const std::optional<core::StableId>& journal_id = std::nullopt) {
    core::Error result{
        .code = code,
        .message = std::move(message),
        .context = {{.key = "source", .value = core::escape_raw_path(source_raw_path)}},
    };
    if (journal_id) {
        result.context.push_back({.key = "journal_id", .value = journal_id->to_string()});
    }
    return result;
}

[[nodiscard]] core::Error
system_error(const std::string_view operation, const int number, const std::string& source_raw_path,
             const std::optional<core::StableId>& journal_id = std::nullopt) {
    return operation_error(core::ErrorCode::io,
                           std::string{operation} + ": " +
                               std::error_code{number, std::generic_category()}.message(),
                           source_raw_path, journal_id);
}

[[nodiscard]] core::Error cancelled(const std::string& source_raw_path) {
    return operation_error(core::ErrorCode::cancelled,
                           "metadata commit was cancelled before atomic publication",
                           source_raw_path);
}

[[nodiscard]] core::Result<ProcessSourceLock>
acquire_process_lock(const core::LocalSourceRevision& revision,
                     const core::CancellationToken& cancellation,
                     const std::string& source_raw_path) {
    const PhysicalKey key{.device = revision.device, .inode = revision.inode};
    std::shared_ptr<std::timed_mutex> mutex;
    {
        std::scoped_lock registry_lock{process_lock_registry_mutex};
        for (auto iterator = process_lock_registry.begin();
             iterator != process_lock_registry.end();) {
            if (iterator->second.expired()) {
                iterator = process_lock_registry.erase(iterator);
            } else {
                ++iterator;
            }
        }
        auto& retained = process_lock_registry[key];
        mutex = retained.lock();
        if (!mutex) {
            mutex = std::make_shared<std::timed_mutex>();
            retained = mutex;
        }
    }
    std::unique_lock<std::timed_mutex> lock{*mutex, std::defer_lock};
    while (!lock.try_lock_for(lock_retry_interval)) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled(source_raw_path));
        }
    }
    return ProcessSourceLock{std::move(mutex), std::move(lock)};
}

[[nodiscard]] core::Result<Descriptor>
open_and_lock_file(const std::string& raw_path, const core::CancellationToken& cancellation,
                   const std::string& source_raw_path,
                   const std::optional<core::StableId>& journal_id = std::nullopt) {
    Descriptor descriptor{::open(raw_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (!descriptor.valid()) {
        return std::unexpected(
            system_error("opening mutation source failed", errno, source_raw_path, journal_id));
    }
    // Exclusive when the filesystem can express it; NFS needs write-open
    // descriptors for exclusive flock emulation, so read-only descriptors
    // degrade to shared or unlocked there. Revision revalidation before
    // every mutation carries correctness either way (ADR-0111).
    auto operation = LOCK_EX | LOCK_NB;
    while (::flock(descriptor.get(), operation) != 0) {
        const auto number = errno;
        if (number == EBADF && (operation & LOCK_EX) != 0) {
            operation = LOCK_SH | LOCK_NB;
            continue;
        }
        if (number == ENOLCK || number == EOPNOTSUPP || number == ENOTSUP ||
            (number == EBADF && (operation & LOCK_SH) != 0)) {
            return descriptor;
        }
        if (number != EWOULDBLOCK && number != EAGAIN && number != EINTR) {
            return std::unexpected(system_error("locking mutation source failed", number,
                                                source_raw_path, journal_id));
        }
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled(source_raw_path));
        }
        std::this_thread::sleep_for(lock_retry_interval);
    }
    return descriptor;
}

[[nodiscard]] core::LocalSourceRevision revision_from_stat(const struct stat& status) {
    return core::LocalSourceRevision{
        .device = static_cast<std::uint64_t>(status.st_dev),
        .inode = static_cast<std::uint64_t>(status.st_ino),
        .size = static_cast<std::uint64_t>(status.st_size),
        .modification_time_seconds = static_cast<std::int64_t>(status.st_mtim.tv_sec),
        .modification_time_nanoseconds = static_cast<std::int64_t>(status.st_mtim.tv_nsec),
    };
}

[[nodiscard]] core::Result<struct stat>
locked_status(const Descriptor& descriptor, const std::string& source_raw_path,
              const std::optional<core::StableId>& journal_id = std::nullopt) {
    struct stat status{};
    if (::fstat(descriptor.get(), &status) != 0) {
        return std::unexpected(
            system_error("observing locked source failed", errno, source_raw_path, journal_id));
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0) {
        return std::unexpected(operation_error(core::ErrorCode::unsupported,
                                               "metadata commit requires a regular file",
                                               source_raw_path, journal_id));
    }
    return status;
}

[[nodiscard]] core::Result<struct stat>
require_direct_single_link_source(const Descriptor& descriptor, const std::string& source_raw_path,
                                  const core::LocalSourceRevision& expected_revision) {
    struct stat path_status{};
    if (::lstat(source_raw_path.c_str(), &path_status) != 0) {
        return std::unexpected(
            system_error("observing metadata source path failed", errno, source_raw_path));
    }
    auto descriptor_status = locked_status(descriptor, source_raw_path);
    if (!descriptor_status) {
        return std::unexpected(std::move(descriptor_status.error()));
    }
    if (!S_ISREG(path_status.st_mode) || path_status.st_nlink != 1 ||
        path_status.st_dev != descriptor_status->st_dev ||
        path_status.st_ino != descriptor_status->st_ino) {
        return std::unexpected(operation_error(
            core::ErrorCode::unsupported,
            "metadata commit does not yet support symlink or hard-linked source paths",
            source_raw_path));
    }
    if (revision_from_stat(*descriptor_status) != expected_revision) {
        return std::unexpected(operation_error(
            core::ErrorCode::conflict, "metadata source changed after the write plan was previewed",
            source_raw_path));
    }
    return *descriptor_status;
}

struct ExtendedAttribute {
    std::string name;
    std::vector<unsigned char> value;

    friend bool operator==(const ExtendedAttribute&, const ExtendedAttribute&) = default;
};

// Tag commits keep source and prepared copy on one filesystem, so a
// filesystem without extended attributes has none to lose on either side;
// the unsupported listing degrades preservation silently (ADR-0111).
struct ExtendedAttributeListing {
    std::vector<ExtendedAttribute> attributes;
    bool supported{true};
};

[[nodiscard]] core::Result<ExtendedAttributeListing>
read_extended_attributes(const Descriptor& descriptor, const std::string& source_raw_path,
                         const std::optional<core::StableId>& journal_id = std::nullopt) {
    const auto listed = core::list_extended_attributes(descriptor.get(), nullptr, 0U);
    if (listed < 0 && (errno == ENOTSUP || errno == EOPNOTSUPP)) {
        return ExtendedAttributeListing{.attributes = {}, .supported = false};
    }
    if (listed < 0) {
        return std::unexpected(
            system_error("listing extended attributes failed", errno, source_raw_path, journal_id));
    }
    if (static_cast<std::size_t>(listed) > maximum_xattr_name_bytes) {
        return std::unexpected(operation_error(core::ErrorCode::limit_exceeded,
                                               "extended-attribute names exceed the commit limit",
                                               source_raw_path, journal_id));
    }
    std::vector<char> names(static_cast<std::size_t>(listed));
    if (listed > 0) {
        const auto repeated =
            core::list_extended_attributes(descriptor.get(), names.data(), names.size());
        if (repeated < 0 || repeated != listed) {
            return std::unexpected(
                operation_error(repeated < 0 ? core::ErrorCode::io : core::ErrorCode::conflict,
                                repeated < 0 ? "reading extended-attribute names failed"
                                             : "extended attributes changed while being read",
                                source_raw_path, journal_id));
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
            return std::unexpected(operation_error(core::ErrorCode::backend,
                                                   "extended-attribute name list is malformed",
                                                   source_raw_path, journal_id));
        }
        std::string name{names.begin() + static_cast<std::ptrdiff_t>(offset), end};
        const auto value_size =
            core::get_extended_attribute(descriptor.get(), name.c_str(), nullptr, 0U);
        if (value_size < 0) {
            return std::unexpected(system_error("sizing extended attribute failed", errno,
                                                source_raw_path, journal_id));
        }
        if (static_cast<std::size_t>(value_size) > maximum_xattr_value_bytes ||
            static_cast<std::size_t>(value_size) > maximum_total_xattr_bytes - total_value_bytes) {
            return std::unexpected(operation_error(core::ErrorCode::limit_exceeded,
                                                   "extended-attribute values exceed commit limits",
                                                   source_raw_path, journal_id));
        }
        std::vector<unsigned char> value(static_cast<std::size_t>(value_size));
        if (value_size > 0) {
            const auto read = core::get_extended_attribute(descriptor.get(), name.c_str(),
                                                           value.data(), value.size());
            if (read < 0 || read != value_size) {
                return std::unexpected(
                    operation_error(read < 0 ? core::ErrorCode::io : core::ErrorCode::conflict,
                                    read < 0 ? "reading extended attribute failed"
                                             : "extended attribute changed while being read",
                                    source_raw_path, journal_id));
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

[[nodiscard]] core::Result<void>
apply_filesystem_metadata(const Descriptor& source, const struct stat& source_status,
                          const ExtendedAttributeListing& source_attributes,
                          const Descriptor& prepared, const std::string& source_raw_path,
                          const core::StableId& journal_id) {
    auto prepared_attributes = read_extended_attributes(prepared, source_raw_path, journal_id);
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
                return std::unexpected(system_error("removing unowned extended attribute failed",
                                                    errno, source_raw_path, journal_id));
            }
        }
    }
    struct stat prepared_status{};
    if (::fstat(prepared.get(), &prepared_status) != 0) {
        return std::unexpected(system_error("observing prepared ownership failed", errno,
                                            source_raw_path, journal_id));
    }
    // Filesystems with server-side identity mapping refuse ownership
    // changes; source and prepared copy share the filesystem, so the
    // published file keeps the identity the filesystem enforces anyway.
    bool ownership_preserved = true;
    if (prepared_status.st_uid != source_status.st_uid ||
        prepared_status.st_gid != source_status.st_gid) {
        if (::fchown(prepared.get(), source_status.st_uid, source_status.st_gid) != 0) {
            if (errno == EPERM || errno == ENOTSUP || errno == EOPNOTSUPP) {
                ownership_preserved = false;
            } else {
                return std::unexpected(system_error("preserving prepared ownership failed", errno,
                                                    source_raw_path, journal_id));
            }
        }
    }
    if (::fchmod(prepared.get(), source_status.st_mode & 07777) != 0) {
        return std::unexpected(system_error("preserving prepared permissions failed", errno,
                                            source_raw_path, journal_id));
    }
    if (prepared_attributes->supported) {
        for (const auto& attribute : source_attributes.attributes) {
            const auto* data = attribute.value.empty() ? nullptr : attribute.value.data();
            if (core::set_extended_attribute(prepared.get(), attribute.name.c_str(), data,
                                             attribute.value.size()) != 0) {
                return std::unexpected(system_error("preserving extended attribute failed", errno,
                                                    source_raw_path, journal_id));
            }
        }
    }
    auto source_after = read_extended_attributes(source, source_raw_path, journal_id);
    auto prepared_after = read_extended_attributes(prepared, source_raw_path, journal_id);
    if (!source_after || !prepared_after) {
        return std::unexpected(!source_after ? std::move(source_after.error())
                                             : std::move(prepared_after.error()));
    }
    if ((source_after->supported && source_attributes.supported &&
         source_after->attributes != source_attributes.attributes) ||
        (prepared_after->supported && source_attributes.supported &&
         prepared_after->attributes != source_attributes.attributes)) {
        return std::unexpected(operation_error(core::ErrorCode::conflict,
                                               "filesystem metadata changed during preparation",
                                               source_raw_path, journal_id));
    }
    if (::fstat(prepared.get(), &prepared_status) != 0 ||
        (ownership_preserved && (prepared_status.st_uid != source_status.st_uid ||
                                 prepared_status.st_gid != source_status.st_gid)) ||
        (prepared_status.st_mode & 07777) != (source_status.st_mode & 07777)) {
        return std::unexpected(operation_error(core::ErrorCode::conflict,
                                               "prepared ownership or permissions differ",
                                               source_raw_path, journal_id));
    }
    return {};
}

[[nodiscard]] std::filesystem::path parent_directory(const std::string& raw_path) {
    auto parent = std::filesystem::path{raw_path}.parent_path();
    return parent.empty() ? std::filesystem::path{"."} : parent;
}

[[nodiscard]] core::Result<void> fsync_parent(const std::string& raw_path,
                                              const std::string& source_raw_path,
                                              const core::StableId& journal_id) {
    const auto directory = parent_directory(raw_path);
    Descriptor descriptor{
        ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (!descriptor.valid() || ::fsync(descriptor.get()) != 0) {
        return std::unexpected(system_error("syncing metadata source directory failed", errno,
                                            source_raw_path, journal_id));
    }
    return {};
}

// The prepared copy beside the source -- the one rename that publishes it
// must stay in its folder -- and the backup where this engine keeps undo
// copies (ADR-0266): its folder, or beside the source.
[[nodiscard]] std::pair<std::string, std::string> sibling_paths(const std::string& source_raw_path,
                                                                const core::StableId& journal_id) {
    const auto parent = std::filesystem::path{source_raw_path}.parent_path();
    const auto stem = ".trackknife-" + journal_id.to_string() + ".metadata-";
    return {(parent / (stem + "prepared")).native(),
            undo_copy_path(source_raw_path, stem + "backup")};
}

// The prepared copy where it must be, and the backup named for this
// operation, wherever it was kept.
[[nodiscard]] bool expected_sibling_paths(const MetadataOperationJournalRecord& record) {
    const auto parent = std::filesystem::path{record.source_raw_path}.parent_path();
    const auto stem = ".trackknife-" + record.id.to_string() + ".metadata-";
    return record.prepared_raw_path == (parent / (stem + "prepared")).native() &&
           std::filesystem::path{record.backup_raw_path}.filename() == stem + "backup";
}

// ADR-0266: beside the source only while an original is put back -- the
// original made ready, and the file it replaces set aside.
[[nodiscard]] std::string restore_sibling(const MetadataOperationJournalRecord& record) {
    return (std::filesystem::path{record.source_raw_path}.parent_path() /
            (".trackknife-" + record.id.to_string() + ".metadata-restore"))
        .native();
}

[[nodiscard]] std::string aside_sibling(const MetadataOperationJournalRecord& record) {
    return (std::filesystem::path{record.source_raw_path}.parent_path() /
            (".trackknife-" + record.id.to_string() + ".metadata-aside"))
        .native();
}

[[nodiscard]] std::map<std::string, std::vector<std::string>>
effective_text(const metadata::MetadataDocument& document) {
    std::map<std::string, std::vector<std::string>> result;
    for (const auto& field : document.effective_fields()) {
        result.emplace(field.canonical_name, field.values);
    }
    return result;
}

[[nodiscard]] std::map<std::string, std::vector<std::string>>
effective_native_text(const metadata::MetadataDocument& document) {
    std::map<std::string, std::vector<std::string>> result;
    for (const auto& field : document.effective_native_fields()) {
        result.emplace(metadata::canonicalize_native_field_name(field.native_name), field.values);
    }
    return result;
}

[[nodiscard]] std::optional<std::vector<std::string>>
addressed_values(const std::map<std::string, std::vector<std::string>>& fields,
                 const std::map<std::string, std::vector<std::string>>& native_fields,
                 const std::string& canonical_name,
                 const std::optional<std::string>& exact_native_name) {
    const auto& addressed_fields = exact_native_name ? native_fields : fields;
    const auto& addressed_name = exact_native_name ? *exact_native_name : canonical_name;
    const auto found = addressed_fields.find(addressed_name);
    return found == addressed_fields.end() ? std::nullopt
                                           : std::optional<std::vector<std::string>>{found->second};
}

// The first planned field the document does not hold as planned, said for a
// person: which field, what was planned, what was read. Empty when all match.
[[nodiscard]] std::optional<std::string>
planned_field_mismatch(const metadata::MetadataDocument& document,
                       const MetadataOperationJournalRecord& record) {
    const auto shown = [](const std::vector<std::string>& values) {
        std::string joined;
        for (const auto& value : values) {
            if (!joined.empty()) {
                joined += "; ";
            }
            joined += value;
        }
        constexpr std::size_t limit = 80U;
        if (joined.size() > limit) {
            joined = joined.substr(0U, limit) + "...";
        }
        return "\"" + joined + "\"";
    };
    const auto fields = effective_text(document);
    const auto native_fields = effective_native_text(document);
    for (const auto& change : record.changes) {
        const auto values = addressed_values(fields, native_fields, change.canonical_name,
                                             change.exact_native_name);
        const auto& name =
            change.exact_native_name ? *change.exact_native_name : change.canonical_name;
        if (change.kind == metadata::StagedMetadataPatchKind::remove_field) {
            if (values) {
                return name + ": planned removed, read " + shown(*values);
            }
        } else if (!values) {
            return name + ": planned " + shown(change.planned_values) + ", read nothing";
        } else if (*values != change.planned_values) {
            return name + ": planned " + shown(change.planned_values) + ", read " + shown(*values) +
                   (change.original_present && *values == change.original_values
                        ? " (the value before the save)"
                        : "");
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool planned_fields_match(const metadata::MetadataDocument& document,
                                        const MetadataOperationJournalRecord& record) {
    return !planned_field_mismatch(document, record);
}

[[nodiscard]] core::Result<void>
verify_plan_originals(const metadata::MetadataDocument& document,
                      const metadata::MetadataWritePlanSource& source_plan) {
    const auto fields = effective_text(document);
    const auto native_fields = effective_native_text(document);
    for (const auto& change : source_plan.changes) {
        const auto values = addressed_values(fields, native_fields, change.canonical_name,
                                             change.exact_native_name);
        const bool present = values.has_value();
        if (present != change.original_present || (present && *values != change.original_values)) {
            return std::unexpected(operation_error(
                core::ErrorCode::conflict,
                "fresh metadata differs from the previewed original values", source_plan.raw_path));
        }
    }
    return {};
}

[[nodiscard]] core::Result<MetadataOperationJournalRecord>
make_journal_record(const metadata::MetadataWritePlanSource& source_plan,
                    const core::StableId& journal_id) {
    const auto [prepared_path, backup_path] = sibling_paths(source_plan.raw_path, journal_id);
    MetadataOperationJournalRecord record{
        .id = journal_id,
        .state = State::planned,
        .source_raw_path = source_plan.raw_path,
        .prepared_raw_path = prepared_path,
        .backup_raw_path = backup_path,
        .expected_revision = *source_plan.observed_revision,
        .prepared_revision = std::nullopt,
        .published_revision = std::nullopt,
        .occurrence_indexes = source_plan.occurrence_indexes,
        .content_kind = MetadataOperationContentKind::text_fields,
        .changes = {},
        .artwork = std::nullopt,
        .failure = std::nullopt,
    };
    record.changes.reserve(source_plan.changes.size());
    for (const auto& change : source_plan.changes) {
        if (change.intents.empty() || change.conflicting_intents ||
            change.unresolved_non_embedded_target) {
            return std::unexpected(
                operation_error(core::ErrorCode::invalid_argument,
                                "metadata commit plan contains an unresolved physical change",
                                source_plan.raw_path, journal_id));
        }
        const auto& intent = change.intents.front();
        if (!std::ranges::all_of(change.intents, [&intent](const auto& candidate) {
                return candidate.kind == intent.kind && candidate.values == intent.values;
            })) {
            return std::unexpected(
                operation_error(core::ErrorCode::invalid_argument,
                                "metadata commit plan contains conflicting logical intents",
                                source_plan.raw_path, journal_id));
        }
        const auto mapping_native_name = change.exact_native_name && change.native_name.empty()
                                             ? std::string_view{change.display_name}
                                             : std::string_view{change.native_name};
        auto mapping =
            metadata::map_flac_text_field(change.canonical_name, change.display_name,
                                          mapping_native_name, intent.kind, intent.values);
        if (!mapping) {
            return std::unexpected(std::move(mapping.error()));
        }
        std::vector<std::size_t> item_indexes;
        item_indexes.reserve(change.intents.size());
        for (const auto& logical_intent : change.intents) {
            item_indexes.push_back(logical_intent.item_index);
        }
        record.changes.push_back(MetadataOperationJournalChange{
            .field_index = change.field_index,
            .canonical_name = change.canonical_name,
            .property_name = std::move(mapping->property_name),
            .original_present = change.original_present,
            .original_values = change.original_values,
            .kind = intent.kind,
            .planned_values = intent.values,
            .item_indexes = std::move(item_indexes),
            .exact_native_name = change.exact_native_name,
        });
    }
    return record;
}

[[nodiscard]] metadata::ArtworkInventoryPolicy embedded_artwork_policy() {
    auto policy = metadata::default_artwork_inventory_policy();
    policy.external_patterns.clear();
    return policy;
}

[[nodiscard]] core::Result<metadata::LocalArtworkInventory>
read_embedded_artwork_inventory(const std::string& raw_path,
                                const core::CancellationToken& cancellation = {}) {
    return metadata::read_local_artwork_inventory(raw_path, embedded_artwork_policy(),
                                                  cancellation);
}

[[nodiscard]] core::Result<MetadataOperationJournalRecord>
make_artwork_journal_record(const metadata::ArtworkWritePlanSource& source_plan,
                            const metadata::LocalArtworkInventory& inventory,
                            const core::StableId& journal_id) {
    auto projected = metadata::project_artwork_inventory(inventory, source_plan);
    if (!projected) {
        return std::unexpected(std::move(projected.error()));
    }
    auto original_fingerprint = metadata::fingerprint_embedded_artwork_inventory(inventory.items);
    auto planned_fingerprint = metadata::fingerprint_embedded_artwork_inventory(*projected);
    if (!original_fingerprint || !planned_fingerprint) {
        return std::unexpected(!original_fingerprint ? std::move(original_fingerprint.error())
                                                     : std::move(planned_fingerprint.error()));
    }
    const auto [prepared_path, backup_path] = sibling_paths(source_plan.raw_media_path, journal_id);
    return MetadataOperationJournalRecord{
        .id = journal_id,
        .state = State::planned,
        .source_raw_path = source_plan.raw_media_path,
        .prepared_raw_path = prepared_path,
        .backup_raw_path = backup_path,
        .expected_revision = *source_plan.observed_media_revision,
        .prepared_revision = std::nullopt,
        .published_revision = std::nullopt,
        .occurrence_indexes = source_plan.occurrence_indexes,
        .content_kind = MetadataOperationContentKind::embedded_artwork,
        .changes = {},
        .artwork =
            MetadataOperationJournalArtwork{
                .kind = source_plan.additional_changes.empty()
                            ? source_plan.change.kind
                            : metadata::ArtworkWritePlanIntentKind::batch,
                .target_ordinal =
                    source_plan.additional_changes.empty() ? source_plan.change.target_ordinal : 0U,
                .original_item_count = inventory.items.size(),
                .planned_item_count = projected->size(),
                .original_target_fingerprint =
                    (source_plan.change.kind == metadata::ArtworkWritePlanIntentKind::add ||
                     !source_plan.additional_changes.empty())
                        ? std::nullopt
                        : std::optional{source_plan.change.expected_target_fingerprint},
                .replacement_fingerprint =
                    (source_plan.change.replacement && source_plan.additional_changes.empty())
                        ? std::optional{source_plan.change.replacement->content_fingerprint}
                        : std::nullopt,
                .original_inventory_fingerprint = *original_fingerprint,
                .planned_inventory_fingerprint = *planned_fingerprint,
            },
        .failure = std::nullopt,
    };
}

enum class CarrierEvidenceSide;
[[nodiscard]] core::Result<void> verify_cue_carrier(const MetadataOperationJournalRecord& record,
                                                    const core::LocalSourceRevision& revision,
                                                    CarrierEvidenceSide side);
[[nodiscard]] core::Result<void>
verify_sidecar_carrier(const MetadataOperationJournalRecord& record,
                       const core::LocalSourceRevision& revision, CarrierEvidenceSide side);
enum class CarrierEvidenceSide { planned, original };

[[nodiscard]] core::Result<metadata::MetadataDocument>
verify_published_content(const MetadataOperationJournalRecord& record,
                         const core::LocalSourceRevision& revision,
                         const core::CancellationToken& cancellation = {}) {
    // ADR-0145: carrier kinds verify against their own parsers; the tag
    // reader neither applies nor produces a document for them.
    if (record.content_kind == MetadataOperationContentKind::cue_replay_gain) {
        auto verified = verify_cue_carrier(record, revision, CarrierEvidenceSide::planned);
        if (!verified) {
            return std::unexpected(std::move(verified.error()));
        }
        return metadata::MetadataDocument{};
    }
    if (record.content_kind == MetadataOperationContentKind::loudness_sidecar) {
        auto verified = verify_sidecar_carrier(record, revision, CarrierEvidenceSide::planned);
        if (!verified) {
            return std::unexpected(std::move(verified.error()));
        }
        return metadata::MetadataDocument{};
    }
    if (record.content_kind == MetadataOperationContentKind::folder_image) {
        auto image = metadata::read_artwork_image_file(record.source_raw_path, 16U * 1024U * 1024U,
                                                       cancellation);
        if (!image || image->source_revision != revision || record.changes.size() != 1 ||
            record.changes.front().planned_values !=
                std::vector<std::string>{
                    metadata::artwork_fingerprint_hex(image->content_fingerprint)})
            return std::unexpected(operation_error(
                core::ErrorCode::conflict, "Folder image hash changed", record.source_raw_path));
        return metadata::MetadataDocument{};
    }
    auto reread = metadata::read_local_metadata(record.source_raw_path, cancellation);
    if (!reread || reread->source_revision != revision) {
        return std::unexpected(
            !reread ? std::move(reread.error())
                    : operation_error(core::ErrorCode::conflict,
                                      "published metadata has an unexpected revision",
                                      record.source_raw_path, record.id));
    }
    if (const auto mismatch = record.changes.empty()
                                  ? std::nullopt
                                  : planned_field_mismatch(reread->document, record)) {
        return std::unexpected(operation_error(
            core::ErrorCode::conflict, "published tags failed verification (" + *mismatch + ")",
            record.source_raw_path, record.id));
    }
    if (record.content_kind == MetadataOperationContentKind::text_fields) {
        if (!planned_fields_match(reread->document, record)) {
            return std::unexpected(operation_error(core::ErrorCode::conflict,
                                                   "published metadata fields failed verification",
                                                   record.source_raw_path, record.id));
        }
        return std::move(reread->document);
    }
    if (!record.artwork) {
        return std::unexpected(operation_error(core::ErrorCode::invariant,
                                               "artwork journal evidence is missing",
                                               record.source_raw_path, record.id));
    }
    auto inventory = read_embedded_artwork_inventory(record.source_raw_path, cancellation);
    if (!inventory) {
        return std::unexpected(std::move(inventory.error()));
    }
    auto fingerprint = metadata::fingerprint_embedded_artwork_inventory(inventory->items);
    if (!fingerprint) {
        return std::unexpected(std::move(fingerprint.error()));
    }
    if (inventory->media_revision != revision ||
        inventory->items.size() != record.artwork->planned_item_count ||
        *fingerprint != record.artwork->planned_inventory_fingerprint) {
        return std::unexpected(operation_error(core::ErrorCode::conflict,
                                               "published embedded artwork failed verification",
                                               record.source_raw_path, record.id));
    }
    return std::move(reread->document);
}

[[nodiscard]] core::Result<void>
transition(MetadataOperationJournal& journal, const MetadataOperationJournalRecord& record,
           const State from, const State to,
           const std::optional<core::LocalSourceRevision>& prepared_revision,
           const std::optional<core::LocalSourceRevision>& published_revision,
           const std::optional<core::Error>& failure = std::nullopt) {
    return journal.transition(record.id, MetadataOperationJournalTransition{
                                             .expected_state = from,
                                             .state = to,
                                             .prepared_revision = prepared_revision,
                                             .published_revision = published_revision,
                                             .failure = failure,
                                         });
}

[[nodiscard]] core::Result<std::optional<core::LocalSourceRevision>>
optional_revision(const std::string& raw_path) {
    auto revision = core::observe_local_source_revision(raw_path);
    if (revision) {
        return std::optional{*revision};
    }
    if (revision.error().code == core::ErrorCode::not_found) {
        return std::optional<core::LocalSourceRevision>{};
    }
    return std::unexpected(std::move(revision.error()));
}

[[nodiscard]] core::Result<void>
unlink_if_matches(const std::string& raw_path,
                  const std::optional<core::LocalSourceRevision>& expected_revision,
                  const std::string& source_raw_path, const core::StableId& journal_id,
                  const bool permit_unobserved_owned_path = false) {
    auto observed = optional_revision(raw_path);
    if (!observed) {
        return std::unexpected(std::move(observed.error()));
    }
    if (!*observed) {
        return {};
    }
    if ((!expected_revision || **observed != *expected_revision) && !permit_unobserved_owned_path) {
        return std::unexpected(operation_error(core::ErrorCode::conflict,
                                               "recovery path has an unexpected identity",
                                               source_raw_path, journal_id));
    }
    struct stat status{};
    if (::lstat(raw_path.c_str(), &status) != 0 || !S_ISREG(status.st_mode) ||
        ::unlink(raw_path.c_str()) != 0) {
        return std::unexpected(system_error("removing owned recovery path failed", errno,
                                            source_raw_path, journal_id));
    }
    return fsync_parent(raw_path, source_raw_path, journal_id);
}

std::atomic<bool> copied_backups_for_testing{false};

using detail::hard_link_refused;

// False with errno set when the bytes could not all be written.
[[nodiscard]] bool write_all(const int descriptor, const char* data, std::size_t size) {
    while (size > 0U) {
        const auto written = ::write(descriptor, data, size);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            if (written == 0) {
                errno = EIO;
            }
            return false;
        }
        data += written;
        size -= static_cast<std::size_t>(written);
    }
    return true;
}

[[nodiscard]] ssize_t read_at(const int descriptor, char* data, const std::size_t size,
                              const off_t offset) {
    ssize_t count = 0;
    do {
        count = ::pread(descriptor, data, size, offset);
    } while (count < 0 && errno == EINTR);
    return count;
}

// ADR-0248: the backup as a copy of the locked source, for a filesystem
// that refuses the hard link. Byte for byte, with the original's ownership,
// permissions, extended attributes and -- where the filesystem lets it be
// set -- modification time, so a restore brings the file back as it was. It
// is read back and compared before it counts; its own identity is returned
// for the journal, since a copy is never the original inode.
[[nodiscard]] core::Result<core::LocalSourceRevision>
copy_file_verified(const MetadataOperationJournalRecord& record, const Descriptor& source,
                   const struct stat& source_status,
                   const ExtendedAttributeListing& source_attributes,
                   const std::string& target_raw_path,
                   const core::CancellationToken& cancellation) {
    Descriptor backup{::open(target_raw_path.c_str(),
                             O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (!backup.valid()) {
        return std::unexpected(system_error("creating metadata backup copy failed", errno,
                                            record.source_raw_path, record.id));
    }
    // Created exclusively above, so whatever stands at the path is this copy.
    const auto discard = [&record, &target_raw_path](core::Error issue) -> core::Result<core::LocalSourceRevision> {
        static_cast<void>(::unlink(target_raw_path.c_str()));
        static_cast<void>(fsync_parent(target_raw_path, record.source_raw_path, record.id));
        return std::unexpected(std::move(issue));
    };
    constexpr std::size_t chunk = 1U << 20U;
    std::vector<char> buffer(chunk);
    std::vector<char> compared(chunk);
    const auto size = static_cast<off_t>(source_status.st_size);
    for (off_t offset = 0; offset < size;) {
        if (cancellation.is_cancellation_requested()) {
            return discard(cancelled(record.source_raw_path));
        }
        const auto wanted = static_cast<std::size_t>(std::min<off_t>(size - offset, chunk));
        const auto count = read_at(source.get(), buffer.data(), wanted, offset);
        if (count <= 0) {
            return discard(count < 0 ? system_error("reading source for metadata backup failed",
                                                    errno, record.source_raw_path, record.id)
                                     : operation_error(core::ErrorCode::conflict,
                                                       "metadata source shrank while copied",
                                                       record.source_raw_path, record.id));
        }
        if (!write_all(backup.get(), buffer.data(), static_cast<std::size_t>(count))) {
            return discard(system_error("writing metadata backup copy failed", errno,
                                        record.source_raw_path, record.id));
        }
        offset += count;
    }
    auto applied = apply_filesystem_metadata(source, source_status, source_attributes, backup,
                                             record.source_raw_path, record.id);
    if (!applied) {
        return discard(std::move(applied.error()));
    }
    if (::fsync(backup.get()) != 0 || !backup.close()) {
        return discard(system_error("syncing metadata backup copy failed", errno,
                                    record.source_raw_path, record.id));
    }
    // Last, by path and after the close, as on an SMB mount only a time set
    // then holds. sshfs refuses it; the copy then keeps the time it has.
    const std::array times{source_status.st_atim, source_status.st_mtim};
    static_cast<void>(
        ::utimensat(AT_FDCWD, target_raw_path.c_str(), times.data(), AT_SYMLINK_NOFOLLOW));

    Descriptor written{::open(target_raw_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (!written.valid()) {
        return discard(system_error("reopening metadata backup copy failed", errno,
                                    record.source_raw_path, record.id));
    }
    for (off_t offset = 0; offset <= size;) {
        if (cancellation.is_cancellation_requested()) {
            return discard(cancelled(record.source_raw_path));
        }
        // One byte past the end proves the copy is no longer than the source.
        const auto wanted = static_cast<std::size_t>(std::min<off_t>(size - offset + 1, chunk));
        const auto original = read_at(source.get(), buffer.data(), wanted, offset);
        const auto copy = read_at(written.get(), compared.data(), wanted, offset);
        if (original < 0 || copy < 0) {
            return discard(system_error("verifying metadata backup copy failed", errno,
                                        record.source_raw_path, record.id));
        }
        if (original != copy ||
            !std::equal(buffer.begin(), buffer.begin() + original, compared.begin())) {
            return discard(operation_error(core::ErrorCode::conflict,
                                           "metadata backup copy differs from its source",
                                           record.source_raw_path, record.id));
        }
        if (original == 0) {
            break;
        }
        offset += original;
    }
    auto identity = core::observe_local_source_revision(target_raw_path);
    if (!identity) {
        return discard(std::move(identity.error()));
    }
    return *identity;
}

[[nodiscard]] core::Result<core::LocalSourceRevision>
copy_metadata_backup(const MetadataOperationJournalRecord& record, const Descriptor& source,
                     const struct stat& source_status,
                     const ExtendedAttributeListing& source_attributes,
                     const core::CancellationToken& cancellation) {
    return copy_file_verified(record, source, source_status, source_attributes,
                              record.backup_raw_path, cancellation);
}

// ADR-0266: an original put back without an exchange -- which NFS and SMB
// do not have -- by renames in the source's own folder: the kept original
// made ready beside it (linked, else copied and verified), the file there
// set aside, the original renamed into place. Each state is told apart by
// what stands at the three names, so a start after a crash finishes it or
// takes it back.

// Removes `raw_path` when it is the file `expected` was, perhaps renumbered
// by a rename (ADR-0249).
[[nodiscard]] core::Result<void> unlink_if_renamed_from(const std::string& raw_path,
                                                        const core::LocalSourceRevision& expected,
                                                        const MetadataOperationJournalRecord& record) {
    auto observed = optional_revision(raw_path);
    if (!observed) {
        return std::unexpected(std::move(observed.error()));
    }
    if (!*observed) {
        return {};
    }
    if (!core::same_file_after_rename(raw_path, expected, **observed)) {
        return std::unexpected(operation_error(core::ErrorCode::conflict,
                                               "set-aside file has an unexpected identity",
                                               record.source_raw_path, record.id));
    }
    if (::unlink(raw_path.c_str()) != 0) {
        return std::unexpected(system_error("removing set-aside file failed", errno,
                                            record.source_raw_path, record.id));
    }
    return fsync_parent(raw_path, record.source_raw_path, record.id);
}

// The kept original, ready beside the source under this operation's own
// name: moved there when it is kept on the source's filesystem -- itself,
// nothing copied -- else copied and verified. Already moved there, it is
// ready; a copy found there was this operation's, from before a crash.
[[nodiscard]] core::Result<void> stage_original(const MetadataOperationJournalRecord& record,
                                                const core::CancellationToken& cancellation) {
    const auto restore = restore_sibling(record);
    const bool kept_there = ::access(record.backup_raw_path.c_str(), F_OK) == 0;
    struct stat existing{};
    if (!kept_there && ::lstat(restore.c_str(), &existing) == 0) {
        return {};
    }
    if (::lstat(restore.c_str(), &existing) == 0) {
        if (!S_ISREG(existing.st_mode) || ::unlink(restore.c_str()) != 0) {
            return std::unexpected(operation_error(core::ErrorCode::conflict,
                                                   "an original being put back is in the way",
                                                   record.source_raw_path, record.id));
        }
    }
    Descriptor kept{::open(record.backup_raw_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    struct stat kept_status{};
    if (!kept.valid() || ::fstat(kept.get(), &kept_status) != 0) {
        return std::unexpected(system_error("opening the kept original failed", errno,
                                            record.source_raw_path, record.id));
    }
    if (revision_from_stat(kept_status) != backup_identity(record)) {
        return std::unexpected(operation_error(core::ErrorCode::conflict,
                                               "the kept original has changed",
                                               record.source_raw_path, record.id));
    }
    if (::rename(record.backup_raw_path.c_str(), restore.c_str()) == 0) {
        if (auto synced = fsync_parent(record.backup_raw_path, record.source_raw_path, record.id);
            !synced) {
            return synced;
        }
        return fsync_parent(restore, record.source_raw_path, record.id);
    }
    if (errno != EXDEV) {
        return std::unexpected(system_error("moving the kept original back failed", errno,
                                            record.source_raw_path, record.id));
    }
    auto attributes = read_extended_attributes(kept, record.source_raw_path, record.id);
    if (!attributes) {
        return std::unexpected(std::move(attributes.error()));
    }
    auto copied =
        copy_file_verified(record, kept, kept_status, *attributes, restore, cancellation);
    if (!copied) {
        return std::unexpected(std::move(copied.error()));
    }
    return fsync_parent(restore, record.source_raw_path, record.id);
}

// From the original made ready to it in place: the file at the source set
// aside, the original renamed in. What is at the source then.
[[nodiscard]] core::Result<core::LocalSourceRevision>
rename_original_in(const MetadataOperationJournalRecord& record) {
    const auto restore = restore_sibling(record);
    const auto aside = aside_sibling(record);
    if (::access(record.source_raw_path.c_str(), F_OK) == 0) {
        if (::rename(record.source_raw_path.c_str(), aside.c_str()) != 0) {
            return std::unexpected(system_error("setting the written file aside failed", errno,
                                                record.source_raw_path, record.id));
        }
        if (auto synced = fsync_parent(aside, record.source_raw_path, record.id); !synced) {
            return std::unexpected(std::move(synced.error()));
        }
    }
    if (::rename(restore.c_str(), record.source_raw_path.c_str()) != 0) {
        const auto number = errno;
        static_cast<void>(::rename(aside.c_str(), record.source_raw_path.c_str()));
        return std::unexpected(system_error("putting the original back failed", number,
                                            record.source_raw_path, record.id));
    }
    if (auto synced = fsync_parent(record.source_raw_path, record.source_raw_path, record.id);
        !synced) {
        return std::unexpected(std::move(synced.error()));
    }
    return core::observe_local_source_revision(record.source_raw_path);
}

[[nodiscard]] core::Result<core::LocalSourceRevision>
put_original_back(const MetadataOperationJournalRecord& record,
                  const core::CancellationToken& cancellation) {
    if (auto staged = stage_original(record, cancellation); !staged) {
        return std::unexpected(std::move(staged.error()));
    }
    return rename_original_in(record);
}

// The put-back taken back: the set-aside file in place again, and the
// original where it is kept -- moved back there, or, a copy of what is still
// kept there, removed.
[[nodiscard]] core::Result<void> take_back_put_back(const MetadataOperationJournalRecord& record) {
    const auto restore = restore_sibling(record);
    if (::rename(record.source_raw_path.c_str(), restore.c_str()) != 0 ||
        ::rename(aside_sibling(record).c_str(), record.source_raw_path.c_str()) != 0) {
        return std::unexpected(system_error("taking the put-back original back failed", errno,
                                            record.source_raw_path, record.id));
    }
    if (::access(record.backup_raw_path.c_str(), F_OK) == 0) {
        static_cast<void>(::unlink(restore.c_str()));
    } else if (::rename(restore.c_str(), record.backup_raw_path.c_str()) != 0) {
        return std::unexpected(system_error("keeping the original again failed", errno,
                                            record.source_raw_path, record.id));
    }
    return fsync_parent(record.source_raw_path, record.source_raw_path, record.id);
}

// An original made ready but not yet put back, kept again: moved back where
// it is kept, or, a copy of what is still kept there, removed.
[[nodiscard]] core::Result<void> unstage_original(const MetadataOperationJournalRecord& record) {
    const auto restore = restore_sibling(record);
    if (::access(restore.c_str(), F_OK) != 0) {
        return {};
    }
    if (::access(record.backup_raw_path.c_str(), F_OK) == 0) {
        if (::unlink(restore.c_str()) != 0) {
            return std::unexpected(system_error("removing an original made ready failed", errno,
                                                record.source_raw_path, record.id));
        }
    } else if (::rename(restore.c_str(), record.backup_raw_path.c_str()) != 0) {
        return std::unexpected(system_error("keeping the original again failed", errno,
                                            record.source_raw_path, record.id));
    }
    return fsync_parent(restore, record.source_raw_path, record.id);
}

// The put-back made final: the file it replaced, and the kept original, go.
[[nodiscard]] core::Result<void> finish_put_back(const MetadataOperationJournalRecord& record,
                                                 const core::LocalSourceRevision& replaced) {
    if (auto removed = unlink_if_renamed_from(aside_sibling(record), replaced, record); !removed) {
        return removed;
    }
    return unlink_if_matches(record.backup_raw_path, backup_identity(record),
                             record.source_raw_path, record.id);
}

// A failure whose rollback failed too: both, the cause first -- the
// rollback's refusal alone hides why there was one.
[[nodiscard]] core::Error not_rolled_back(const core::Error& failure, const core::Error& rollback) {
    auto combined = rollback;
    combined.message = failure.message + "; not rolled back: " + rollback.message;
    return combined;
}

[[nodiscard]] core::Result<void>
rollback_published(const MetadataOperationJournalRecord& record,
                   const core::LocalSourceRevision& published_revision) {
    auto source = optional_revision(record.source_raw_path);
    auto backup = optional_revision(record.backup_raw_path);
    if (!source || !backup) {
        return std::unexpected(!source ? std::move(source.error()) : std::move(backup.error()));
    }
    // The source was renamed into place, so it may carry a new inode number
    // (ADR-0249); the backup was not.
    if (!*source || !*backup ||
        !core::same_file_after_rename(record.source_raw_path, published_revision, **source) ||
        **backup != backup_identity(record)) {
        // Which one, and how: the message is all a remote user can send back.
        const auto differs = [&record](const char* what,
                                       const std::optional<core::LocalSourceRevision>& found,
                                       const core::LocalSourceRevision& expected) -> std::string {
            if (!found) {
                return std::string{what} + " missing";
            }
            return *found == expected ||
                           (std::string_view{what} == "source" &&
                            core::same_file_after_rename(record.source_raw_path, expected, *found))
                       ? std::string{}
                       : std::string{what} + " " + core::describe_revision_change(expected, *found);
        };
        auto detail = differs("source", *source, published_revision);
        const auto backup_detail = differs("backup", *backup, backup_identity(record));
        if (!backup_detail.empty()) {
            detail += (detail.empty() ? "" : "; ") + backup_detail;
        }
        return std::unexpected(operation_error(
            core::ErrorCode::conflict,
            "published metadata source cannot be rolled back from the recorded identities (" +
                detail + ")",
            record.source_raw_path, record.id));
    }
    struct stat source_status{};
    struct stat backup_status{};
    if (::lstat(record.source_raw_path.c_str(), &source_status) != 0 ||
        ::lstat(record.backup_raw_path.c_str(), &backup_status) != 0) {
        return std::unexpected(system_error("observing metadata rollback topology failed", errno,
                                            record.source_raw_path, record.id));
    }
    // The backup may have a second name for a moment: NFS keeps a replaced
    // file still open under a hidden .nfs name until it is closed. It is
    // this operation's own file, under its own name; the source is checked.
    if (!S_ISREG(source_status.st_mode) || !S_ISREG(backup_status.st_mode) ||
        source_status.st_nlink != 1) {
        return std::unexpected(
            operation_error(core::ErrorCode::conflict,
                            "metadata rollback refuses changed symlink or hard-link topology",
                            record.source_raw_path, record.id));
    }

    // ADR-0266: put back by renames in the source's folder, wherever the
    // backup is kept and whether or not the filesystem can exchange.
    auto restored = put_original_back(record, {});
    if (!restored) {
        return std::unexpected(std::move(restored.error()));
    }
    return finish_put_back(record, published_revision);
}

[[nodiscard]] core::Result<void>
record_terminal_failure(MetadataOperationJournal& journal,
                        const MetadataOperationJournalRecord& record, const State current_state,
                        const std::optional<core::LocalSourceRevision>& prepared_revision,
                        const std::optional<core::LocalSourceRevision>& published_revision,
                        const core::Error& failure, const bool reconciled) {
    return transition(journal, record, current_state,
                      reconciled ? State::rolled_back : State::needs_reconciliation,
                      prepared_revision, published_revision, failure);
}

// ADR-0145: carrier evidence for CUE-sheet and loudness-sidecar rewrites.
// Both kinds reuse the text change rows; the carrier-internal identity
// lives in exact_native_name.

constexpr std::string_view cue_album_slug = "cue-album";

[[nodiscard]] std::string cue_track_slug(const std::size_t file_index,
                                         const std::size_t track_index) {
    return "cue-track:" + std::to_string(file_index) + ":" + std::to_string(track_index);
}

// nullopt = invalid slug; inner nullopt = album scope.
[[nodiscard]] std::optional<std::optional<std::pair<std::size_t, std::size_t>>>
parse_cue_slug(const std::optional<std::string>& slug) {
    if (!slug) {
        return std::nullopt;
    }
    if (*slug == cue_album_slug) {
        return std::optional<std::optional<std::pair<std::size_t, std::size_t>>>{
            std::optional<std::pair<std::size_t, std::size_t>>{}};
    }
    constexpr std::string_view prefix{"cue-track:"};
    if (!slug->starts_with(prefix)) {
        return std::nullopt;
    }
    const auto body = std::string_view{*slug}.substr(prefix.size());
    const auto colon = body.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t file_index = 0U;
    std::size_t track_index = 0U;
    const auto file_text = body.substr(0U, colon);
    const auto track_text = body.substr(colon + 1U);
    const auto file_parsed =
        std::from_chars(file_text.data(), file_text.data() + file_text.size(), file_index);
    const auto track_parsed =
        std::from_chars(track_text.data(), track_text.data() + track_text.size(), track_index);
    if (file_parsed.ec != std::errc{} || file_parsed.ptr != file_text.data() + file_text.size() ||
        track_parsed.ec != std::errc{} ||
        track_parsed.ptr != track_text.data() + track_text.size()) {
        return std::nullopt;
    }
    return std::optional{std::optional{std::pair{file_index, track_index}}};
}

[[nodiscard]] std::string sidecar_entry_slug(const metadata::StagedLogicalIdentity& identity) {
    const auto part = [](const auto& value) {
        return value ? std::to_string(*value) : std::string{"-"};
    };
    return "entry:" + part(identity.stream_index) + ":" + part(identity.subsong_index) + ":" +
           part(identity.start_sample) + ":" + part(identity.end_sample);
}

[[nodiscard]] std::optional<metadata::StagedLogicalIdentity>
parse_sidecar_slug(const std::optional<std::string>& slug) {
    if (!slug || !slug->starts_with("entry:")) {
        return std::nullopt;
    }
    std::array<std::optional<std::int64_t>, 4> parts;
    std::string_view body = std::string_view{*slug}.substr(6U);
    for (std::size_t index = 0U; index < parts.size(); ++index) {
        const auto colon = body.find(':');
        if ((index + 1U < parts.size()) == (colon == std::string_view::npos)) {
            return std::nullopt;
        }
        const auto token = colon == std::string_view::npos ? body : body.substr(0U, colon);
        if (token != "-") {
            std::int64_t value = 0;
            const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
                return std::nullopt;
            }
            parts[index] = value;
        }
        if (colon != std::string_view::npos) {
            body = body.substr(colon + 1U);
        }
    }
    return metadata::StagedLogicalIdentity{
        .stream_index = parts[0] ? std::optional{static_cast<int>(*parts[0])} : std::nullopt,
        .subsong_index = parts[1] ? std::optional{static_cast<int>(*parts[1])} : std::nullopt,
        .start_sample = parts[2],
        .end_sample = parts[3],
    };
}

[[nodiscard]] std::string_view carrier_display_name(const std::string& canonical_name) {
    if (canonical_name == "replaygaintrackgain") {
        return "REPLAYGAIN_TRACK_GAIN";
    }
    if (canonical_name == "replaygaintrackpeak") {
        return "REPLAYGAIN_TRACK_PEAK";
    }
    if (canonical_name == "replaygainalbumgain") {
        return "REPLAYGAIN_ALBUM_GAIN";
    }
    return "REPLAYGAIN_ALBUM_PEAK";
}

[[nodiscard]] bool carrier_gain_name(const std::string& canonical_name) {
    return canonical_name == "replaygaintrackgain" || canonical_name == "replaygainalbumgain";
}

// Canonicalizes one staged loudness value to its published carrier text.
[[nodiscard]] core::Result<std::pair<double, std::string>>
canonical_loudness_value(const std::string& canonical_name, const std::string& staged,
                         const std::string& raw_path) {
    const auto gain = carrier_gain_name(canonical_name);
    const auto parsed = gain ? formats::parse_replay_gain_decibels(staged)
                             : formats::parse_replay_gain_peak(staged);
    const auto zero_peak =
        !gain && !parsed && staged.find_first_not_of("0. \t") == std::string::npos;
    if (!parsed && !zero_peak) {
        return std::unexpected(operation_error(
            core::ErrorCode::invariant,
            "a planned loudness value stopped being parseable before commit", raw_path));
    }
    const auto value = parsed ? *parsed : 0.0;
    return std::pair{value, gain ? formats::replay_gain_decibel_text(value)
                                 : formats::replay_gain_peak_text(value)};
}

[[nodiscard]] bool carrier_evidence_matches(const std::vector<std::string>& values,
                                            const MetadataOperationJournalChange& change,
                                            const CarrierEvidenceSide side) {
    if (side == CarrierEvidenceSide::planned) {
        if (change.kind == metadata::StagedMetadataPatchKind::remove_field) {
            return values.empty();
        }
        return values == change.planned_values;
    }
    if (!change.original_present) {
        return values.empty();
    }
    return values == change.original_values;
}

[[nodiscard]] core::Result<std::string> read_carrier_bytes(const std::string& raw_path,
                                                           const std::size_t maximum_bytes,
                                                           const core::StableId& journal_id) {
    std::error_code size_error;
    const auto size = std::filesystem::file_size(std::filesystem::path{raw_path}, size_error);
    if (size_error) {
        return std::unexpected(operation_error(
            core::ErrorCode::io, "the carrier file size could not be read", raw_path, journal_id));
    }
    if (size > maximum_bytes) {
        return std::unexpected(operation_error(core::ErrorCode::limit_exceeded,
                                               "the carrier file exceeds the byte limit", raw_path,
                                               journal_id));
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    std::ifstream input{std::filesystem::path{raw_path}, std::ios::binary};
    if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) && !bytes.empty()) {
        return std::unexpected(operation_error(
            core::ErrorCode::io, "the carrier file could not be read", raw_path, journal_id));
    }
    return bytes;
}

[[nodiscard]] std::vector<std::string>
cue_scope_values(const formats::CueSheet& sheet,
                 const std::optional<std::pair<std::size_t, std::size_t>>& track,
                 const std::string_view rem_name) {
    std::vector<std::string> values;
    const auto collect = [&values, rem_name](const formats::CueMetadata& scope) {
        for (const auto& remark : scope.remarks) {
            if (remark.name == rem_name) {
                values.push_back(remark.value);
            }
        }
    };
    if (!track) {
        collect(sheet.metadata);
        return values;
    }
    if (track->first < sheet.files.size() &&
        track->second < sheet.files[track->first].tracks.size()) {
        collect(sheet.files[track->first].tracks[track->second].metadata);
    }
    return values;
}

core::Result<void> verify_cue_carrier(const MetadataOperationJournalRecord& record,
                                      const core::LocalSourceRevision& revision,
                                      const CarrierEvidenceSide side) {
    auto observed = core::observe_local_source_revision(record.source_raw_path);
    if (!observed || *observed != revision) {
        return std::unexpected(!observed
                                   ? std::move(observed.error())
                                   : operation_error(core::ErrorCode::conflict,
                                                     "the CUE carrier has an unexpected revision",
                                                     record.source_raw_path, record.id));
    }
    auto bytes = read_carrier_bytes(record.source_raw_path, formats::CueParseLimits{}.source_bytes,
                                    record.id);
    if (!bytes) {
        return std::unexpected(std::move(bytes.error()));
    }
    auto sheet = formats::parse_cue_sheet(*bytes);
    if (!sheet) {
        return std::unexpected(std::move(sheet.error()));
    }
    for (const auto& change : record.changes) {
        const auto scope = parse_cue_slug(change.exact_native_name);
        if (!scope) {
            return std::unexpected(operation_error(core::ErrorCode::invariant,
                                                   "CUE journal evidence has an invalid identity",
                                                   record.source_raw_path, record.id));
        }
        const auto values = cue_scope_values(*sheet, *scope, change.property_name);
        if (!carrier_evidence_matches(values, change, side)) {
            return std::unexpected(operation_error(core::ErrorCode::conflict,
                                                   "CUE carrier content failed verification",
                                                   record.source_raw_path, record.id));
        }
    }
    return {};
}

[[nodiscard]] std::vector<std::string>
sidecar_member_values(const metadata::LoudnessSidecar& sidecar,
                      const metadata::StagedLogicalIdentity& identity,
                      const std::string& canonical_name) {
    for (const auto& entry : sidecar.entries) {
        const auto same = entry.stream_index == identity.stream_index &&
                          entry.subsong_index == identity.subsong_index &&
                          entry.start_sample == identity.start_sample &&
                          entry.end_sample == identity.end_sample;
        if (!same) {
            continue;
        }
        const auto& member = canonical_name == "replaygaintrackgain"   ? entry.track_gain_db
                             : canonical_name == "replaygaintrackpeak" ? entry.track_peak
                             : canonical_name == "replaygainalbumgain" ? entry.album_gain_db
                                                                       : entry.album_peak;
        if (!member) {
            return {};
        }
        return {carrier_gain_name(canonical_name) ? formats::replay_gain_decibel_text(*member)
                                                  : formats::replay_gain_peak_text(*member)};
    }
    return {};
}

core::Result<void> verify_sidecar_carrier(const MetadataOperationJournalRecord& record,
                                          const core::LocalSourceRevision& revision,
                                          const CarrierEvidenceSide side) {
    auto observed = core::observe_local_source_revision(record.source_raw_path);
    if (!observed || *observed != revision) {
        return std::unexpected(
            !observed ? std::move(observed.error())
                      : operation_error(core::ErrorCode::conflict,
                                        "the sidecar carrier has an unexpected revision",
                                        record.source_raw_path, record.id));
    }
    auto bytes = read_carrier_bytes(record.source_raw_path,
                                    metadata::LoudnessSidecarLimits{}.source_bytes, record.id);
    if (!bytes) {
        return std::unexpected(std::move(bytes.error()));
    }
    auto sidecar = metadata::parse_loudness_sidecar(*bytes);
    if (!sidecar) {
        return std::unexpected(std::move(sidecar.error()));
    }
    for (const auto& change : record.changes) {
        const auto identity = parse_sidecar_slug(change.exact_native_name);
        if (!identity) {
            return std::unexpected(operation_error(
                core::ErrorCode::invariant, "sidecar journal evidence has an invalid identity",
                record.source_raw_path, record.id));
        }
        const auto values = sidecar_member_values(*sidecar, *identity, change.canonical_name);
        if (!carrier_evidence_matches(values, change, side)) {
            return std::unexpected(operation_error(core::ErrorCode::conflict,
                                                   "sidecar carrier content failed verification",
                                                   record.source_raw_path, record.id));
        }
    }
    return {};
}

[[nodiscard]] MetadataOperationJournalChange
carrier_change(const metadata::MetadataWritePlanLoudnessField& field, std::string identity_slug,
               std::vector<std::string> original_values, std::vector<std::string> planned_values) {
    return MetadataOperationJournalChange{
        .field_index = field.field_index,
        .canonical_name = field.canonical_name,
        .property_name = std::string(carrier_display_name(field.canonical_name)),
        .original_present = !original_values.empty(),
        .original_values = std::move(original_values),
        .kind = field.kind,
        .planned_values = std::move(planned_values),
        .item_indexes = field.item_indexes,
        .exact_native_name = std::move(identity_slug),
    };
}

[[nodiscard]] core::Result<MetadataCommitResult>
verified_commit_result(const MetadataOperationJournalRecord& record,
                       const core::LocalSourceRevision& published_revision,
                       metadata::MetadataDocument document) {
    return MetadataCommitResult{
        .journal_id = record.id,
        .source_raw_path = record.source_raw_path,
        .backup_raw_path = record.backup_raw_path,
        .previous_revision = record.expected_revision,
        .published_revision = published_revision,
        .document = std::move(document),
        .occurrence_indexes = record.occurrence_indexes,
        .content_kind = record.content_kind,
    };
}

[[nodiscard]] bool original_fields_match(const metadata::MetadataDocument& document,
                                         const MetadataOperationJournalRecord& record) {
    const auto fields = effective_text(document);
    const auto native_fields = effective_native_text(document);
    for (const auto& change : record.changes) {
        const auto values = addressed_values(fields, native_fields, change.canonical_name,
                                             change.exact_native_name);
        if (!change.original_present) {
            if (values) {
                return false;
            }
        } else if (!values || *values != change.original_values) {
            return false;
        }
    }
    return true;
}

// What was put back is the original: `restored` is the file at the source
// now -- the kept original itself, or a copy of it (ADR-0266).
[[nodiscard]] core::Result<metadata::MetadataDocument>
verify_original_content(const MetadataOperationJournalRecord& record,
                        const core::LocalSourceRevision& restored,
                        const core::CancellationToken& cancellation = {}) {
    if (record.content_kind == MetadataOperationContentKind::cue_replay_gain) {
        auto verified =
            verify_cue_carrier(record, restored, CarrierEvidenceSide::original);
        if (!verified) {
            return std::unexpected(std::move(verified.error()));
        }
        return metadata::MetadataDocument{};
    }
    if (record.content_kind == MetadataOperationContentKind::loudness_sidecar) {
        auto verified =
            verify_sidecar_carrier(record, restored, CarrierEvidenceSide::original);
        if (!verified) {
            return std::unexpected(std::move(verified.error()));
        }
        return metadata::MetadataDocument{};
    }
    if (record.content_kind == MetadataOperationContentKind::folder_image) {
        auto image = metadata::read_artwork_image_file(record.source_raw_path, 16U * 1024U * 1024U,
                                                       cancellation);
        if (!image || image->source_revision != restored ||
            record.changes.size() != 1 ||
            record.changes.front().original_values !=
                std::vector<std::string>{
                    metadata::artwork_fingerprint_hex(image->content_fingerprint)})
            return std::unexpected(operation_error(core::ErrorCode::conflict,
                                                   "Original folder image hash changed",
                                                   record.source_raw_path));
        return metadata::MetadataDocument{};
    }
    auto reread = metadata::read_local_metadata(record.source_raw_path, cancellation);
    if (!reread || reread->source_revision != restored) {
        return std::unexpected(!reread
                                   ? std::move(reread.error())
                                   : operation_error(core::ErrorCode::conflict,
                                                     "restored metadata has an unexpected revision",
                                                     record.source_raw_path, record.id));
    }
    if (!record.changes.empty() && !original_fields_match(reread->document, record)) {
        return std::unexpected(operation_error(core::ErrorCode::conflict,
                                               "restored tags failed verification",
                                               record.source_raw_path, record.id));
    }
    if (record.content_kind == MetadataOperationContentKind::text_fields) {
        if (!original_fields_match(reread->document, record)) {
            return std::unexpected(operation_error(
                core::ErrorCode::conflict, "restored metadata fields failed undo verification",
                record.source_raw_path, record.id));
        }
        return std::move(reread->document);
    }
    if (!record.artwork) {
        return std::unexpected(operation_error(core::ErrorCode::invariant,
                                               "artwork journal evidence is missing",
                                               record.source_raw_path, record.id));
    }
    auto inventory = read_embedded_artwork_inventory(record.source_raw_path, cancellation);
    if (!inventory) {
        return std::unexpected(std::move(inventory.error()));
    }
    auto fingerprint = metadata::fingerprint_embedded_artwork_inventory(inventory->items);
    if (!fingerprint) {
        return std::unexpected(std::move(fingerprint.error()));
    }
    if (inventory->media_revision != restored ||
        inventory->items.size() != record.artwork->original_item_count ||
        *fingerprint != record.artwork->original_inventory_fingerprint) {
        return std::unexpected(operation_error(core::ErrorCode::conflict,
                                               "restored embedded artwork failed undo verification",
                                               record.source_raw_path, record.id));
    }
    return std::move(reread->document);
}

[[nodiscard]] core::Result<void>
transition_backup(MetadataOperationJournal& journal, const core::StableId& journal_id,
                  const BackupState from, const BackupState to,
                  const std::optional<core::StableId>& undo_id = std::nullopt,
                  const std::optional<core::Error>& failure = std::nullopt) {
    return journal.transition_backup(
        journal_id,
        MetadataOperationBackupTransition{
            .expected_state = from, .state = to, .undo_id = undo_id, .failure = failure});
}

[[nodiscard]] core::Result<void>
verify_direct_single_link(const std::string& raw_path, const Descriptor& descriptor,
                          const core::LocalSourceRevision& expected,
                          const MetadataOperationJournalRecord& record,
                          const std::string_view description) {
    struct stat path_status{};
    struct stat descriptor_status{};
    if (::lstat(raw_path.c_str(), &path_status) != 0 ||
        ::fstat(descriptor.get(), &descriptor_status) != 0) {
        return std::unexpected(
            system_error(std::string{"observing "} + std::string{description} + " failed", errno,
                         record.source_raw_path, record.id));
    }
    if (!S_ISREG(path_status.st_mode) || path_status.st_nlink != 1 ||
        path_status.st_dev != descriptor_status.st_dev ||
        path_status.st_ino != descriptor_status.st_ino ||
        revision_from_stat(descriptor_status) != expected) {
        return std::unexpected(operation_error(
            core::ErrorCode::conflict,
            std::string{description} + " no longer has its recorded identity and topology",
            record.source_raw_path, record.id));
    }
    return {};
}

[[nodiscard]] core::Result<MetadataCommitResult>
finish_metadata_undo(MetadataOperationBackupRecord backup, MetadataOperationJournal& journal,
                     const MetadataDependentStateCommitter& dependent_state_committer,
                     const core::CancellationToken& cancellation) {
    const auto& record = backup.operation;
    if (backup.state != BackupState::undoing || !backup.undo_id || !record.published_revision) {
        return std::unexpected(operation_error(core::ErrorCode::invalid_argument,
                                               "metadata undo has incomplete journal evidence",
                                               record.source_raw_path, record.id));
    }

    auto process_lock =
        acquire_process_lock(*record.published_revision, cancellation, record.source_raw_path);
    if (!process_lock) {
        if (process_lock.error().code == core::ErrorCode::cancelled) {
            static_cast<void>(
                transition_backup(journal, record.id, BackupState::undoing, BackupState::retained));
        }
        return std::unexpected(std::move(process_lock.error()));
    }
    auto source_revision = optional_revision(record.source_raw_path);
    auto backup_revision = optional_revision(record.backup_raw_path);
    if (!source_revision || !backup_revision) {
        auto issue = !source_revision ? std::move(source_revision.error())
                                      : std::move(backup_revision.error());
        static_cast<void>(transition_backup(journal, record.id, BackupState::undoing,
                                            BackupState::needs_reconciliation, backup.undo_id,
                                            issue));
        return std::unexpected(std::move(issue));
    }

    const auto published = *record.published_revision;
    const auto reconcile = [&](core::Error issue) -> core::Result<MetadataCommitResult> {
        auto marked = transition_backup(journal, record.id, BackupState::undoing,
                                        BackupState::needs_reconciliation, backup.undo_id, issue);
        return std::unexpected(marked ? std::move(issue) : std::move(marked.error()));
    };
    auto aside_revision = optional_revision(aside_sibling(record));
    if (!aside_revision) {
        return reconcile(std::move(aside_revision.error()));
    }
    const auto set_aside = [&](const std::optional<core::LocalSourceRevision>& found) {
        return found && core::same_file_after_rename(aside_sibling(record), published, *found);
    };
    // ADR-0266, by what stands at the names: not begun (the written file in
    // place, the kept original intact), half done (the written file set
    // aside, the original beside it), or put back (the written file aside).
    const bool staged = ::access(restore_sibling(record).c_str(), F_OK) == 0;
    const bool not_begun =
        *source_revision &&
        core::same_file_after_rename(record.source_raw_path, published, **source_revision) &&
        ((*backup_revision && **backup_revision == backup_identity(record)) || staged) &&
        !*aside_revision;
    const bool half_done = !*source_revision && set_aside(*aside_revision) && staged;
    const bool put_back = *source_revision && set_aside(*aside_revision);
    if (!not_begun && !half_done && !put_back) {
        return reconcile(operation_error(
            core::ErrorCode::conflict,
            "metadata undo has ambiguous source or retained-backup identities",
            record.source_raw_path, record.id));
    }
    if (not_begun) {
        auto source_descriptor = open_and_lock_file(record.source_raw_path, cancellation,
                                                    record.source_raw_path, record.id);
        if (!source_descriptor) {
            auto issue = std::move(source_descriptor.error());
            if (issue.code == core::ErrorCode::cancelled) {
                static_cast<void>(transition_backup(journal, record.id, BackupState::undoing,
                                                    BackupState::retained));
                return std::unexpected(std::move(issue));
            }
            return reconcile(std::move(issue));
        }
        if (auto verified = verify_direct_single_link(record.source_raw_path, *source_descriptor,
                                                      **source_revision, record,
                                                      "metadata undo source");
            !verified) {
            return reconcile(verified.error());
        }
    }

    std::optional<core::LocalSourceRevision> restored_revision;
    if (not_begun) {
        auto restored = put_original_back(record, cancellation);
        if (!restored) {
            // Nothing set aside: the written file is where it was.
            auto current = optional_revision(record.source_raw_path);
            const bool unchanged =
                current && *current &&
                core::same_file_after_rename(record.source_raw_path, published, **current);
            static_cast<void>(::unlink(restore_sibling(record).c_str()));
            if (unchanged) {
                auto settled = transition_backup(journal, record.id, BackupState::undoing,
                                                 BackupState::retained);
                if (!settled) {
                    return std::unexpected(settled.error());
                }
                return std::unexpected(restored.error());
            }
            return reconcile(restored.error());
        }
        restored_revision = *restored;
    } else if (half_done) {
        auto restored = rename_original_in(record);
        if (!restored) {
            return reconcile(restored.error());
        }
        restored_revision = *restored;
    } else {
        restored_revision = **source_revision;
    }

    const auto restore_publication =
        [&](const core::Error& issue) -> core::Result<MetadataCommitResult> {
        if (auto taken = take_back_put_back(record); !taken) {
            return reconcile(taken.error());
        }
        auto retained =
            transition_backup(journal, record.id, BackupState::undoing, BackupState::retained);
        if (!retained) {
            return std::unexpected(retained.error());
        }
        return std::unexpected(issue);
    };

    auto reread = verify_original_content(record, *restored_revision, cancellation);
    if (!reread) {
        return restore_publication(reread.error());
    }
    MetadataCommitResult result{
        .journal_id = *backup.undo_id,
        .source_raw_path = record.source_raw_path,
        .backup_raw_path = record.backup_raw_path,
        .previous_revision = published,
        .published_revision = *restored_revision,
        .document = *reread,
        .occurrence_indexes = record.occurrence_indexes,
    };
    if (auto dependent = dependent_state_committer(result); !dependent) {
        return restore_publication(dependent.error());
    }
    auto final_revision = core::observe_local_source_revision(record.source_raw_path);
    if (!final_revision || *final_revision != *restored_revision) {
        return reconcile(!final_revision
                             ? final_revision.error()
                             : operation_error(core::ErrorCode::conflict,
                                               "metadata source changed during undo state commit",
                                               record.source_raw_path, record.id));
    }
    if (auto finished = finish_put_back(record, published); !finished) {
        return reconcile(finished.error());
    }
    auto completed = transition_backup(journal, record.id, BackupState::undoing,
                                       BackupState::undone, backup.undo_id);
    if (!completed) {
        return std::unexpected(std::move(completed.error()));
    }
    return result;
}

[[nodiscard]] core::Result<MetadataCommitResult> publish_prepared_metadata_copy(
    const MetadataOperationJournalRecord& journaled_record, const Descriptor& source_descriptor,
    const struct stat& source_status, const ExtendedAttributeListing& source_attributes,
    const core::LocalSourceRevision& initial_prepared_revision,
    const metadata::MetadataDocument& prepared_document, MetadataOperationJournal& journal,
    const MetadataDependentStateCommitter& dependent_state_committer,
    const core::CancellationToken& cancellation) {
    // Gains the backup's identity once a copied backup is journaled.
    auto record = journaled_record;
    auto prepared_descriptor = open_and_lock_file(record.prepared_raw_path, cancellation,
                                                  record.source_raw_path, record.id);
    if (!prepared_descriptor) {
        const auto& failure = prepared_descriptor.error();
        const auto cleaned = unlink_if_matches(record.prepared_raw_path, initial_prepared_revision,
                                               record.source_raw_path, record.id);
        const bool restored = cleaned.has_value();
        auto terminal =
            record_terminal_failure(journal, record, State::planned, initial_prepared_revision,
                                    std::nullopt, failure, restored);
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(restored ? failure : cleaned.error());
    }
    auto metadata_applied =
        apply_filesystem_metadata(source_descriptor, source_status, source_attributes,
                                  *prepared_descriptor, record.source_raw_path, record.id);
    if (metadata_applied && ::fsync(prepared_descriptor->get()) != 0) {
        metadata_applied = std::unexpected(system_error("syncing prepared metadata file failed",
                                                        errno, record.source_raw_path, record.id));
    }
    // Last, after ownership, permissions and extended attributes -- which on
    // an SMB mount are data too, and move the time again: a time of its own,
    // before the copy's identity is recorded for every later check.
    if (metadata_applied) {
        core::settle_written_file_time(record.prepared_raw_path);
    }
    if (!metadata_applied) {
        const auto& failure = metadata_applied.error();
        const auto cleaned = unlink_if_matches(record.prepared_raw_path, initial_prepared_revision,
                                               record.source_raw_path, record.id);
        const bool restored = cleaned.has_value();
        auto terminal =
            record_terminal_failure(journal, record, State::planned, initial_prepared_revision,
                                    std::nullopt, failure, restored);
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(restored ? failure : cleaned.error());
    }
    auto prepared_status = locked_status(*prepared_descriptor, record.source_raw_path, record.id);
    if (!prepared_status) {
        const auto& failure = prepared_status.error();
        const auto cleaned = unlink_if_matches(record.prepared_raw_path, initial_prepared_revision,
                                               record.source_raw_path, record.id);
        auto terminal =
            record_terminal_failure(journal, record, State::planned, initial_prepared_revision,
                                    std::nullopt, failure, cleaned.has_value());
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(cleaned ? failure : cleaned.error());
    }
    const auto prepared_revision = revision_from_stat(*prepared_status);
    auto prepared_transition = transition(journal, record, State::planned, State::prepared,
                                          prepared_revision, std::nullopt);
    if (!prepared_transition) {
        const auto& failure = prepared_transition.error();
        const auto cleaned = unlink_if_matches(record.prepared_raw_path, prepared_revision,
                                               record.source_raw_path, record.id);
        if (cleaned) {
            static_cast<void>(record_terminal_failure(
                journal, record, State::planned, prepared_revision, std::nullopt, failure, true));
        } else {
            static_cast<void>(record_terminal_failure(journal, record, State::planned,
                                                      prepared_revision, std::nullopt,
                                                      cleaned.error(), false));
        }
        return std::unexpected(failure);
    }

    if (cancellation.is_cancellation_requested()) {
        const auto failure = cancelled(record.source_raw_path);
        const auto cleaned = unlink_if_matches(record.prepared_raw_path, prepared_revision,
                                               record.source_raw_path, record.id);
        auto terminal = record_terminal_failure(journal, record, State::prepared, prepared_revision,
                                                std::nullopt, failure, cleaned.has_value());
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(cleaned ? failure : cleaned.error());
    }

    auto current_source = core::observe_local_source_revision(record.source_raw_path);
    std::optional<core::Error> backup_failure;
    bool copy_cleaned = true;
    if (!current_source) {
        backup_failure = std::move(current_source.error());
    } else if (*current_source != record.expected_revision) {
        backup_failure = operation_error(
            core::ErrorCode::conflict,
            "metadata source changed before publication (" +
                core::describe_revision_change(record.expected_revision, *current_source) + ")",
            record.source_raw_path, record.id);
    } else if (copied_backups_for_testing.load(std::memory_order_relaxed) ||
               ::link(record.source_raw_path.c_str(), record.backup_raw_path.c_str()) != 0) {
        const auto number =
            copied_backups_for_testing.load(std::memory_order_relaxed) ? ENOTSUP : errno;
        if (!hard_link_refused(number)) {
            backup_failure = system_error("creating metadata backup failed", number,
                                          record.source_raw_path, record.id);
        } else if (auto copied = copy_metadata_backup(record, source_descriptor, source_status,
                                                      source_attributes, cancellation);
                   !copied) {
            backup_failure = std::move(copied.error());
        } else if (auto recorded = journal.transition(record.id,
                                                      MetadataOperationJournalTransition{
                                                          .expected_state = State::prepared,
                                                          .state = State::prepared,
                                                          .prepared_revision = prepared_revision,
                                                          .published_revision = std::nullopt,
                                                          .failure = std::nullopt,
                                                          .backup_revision = *copied,
                                                      });
                   !recorded) {
            backup_failure = std::move(recorded.error());
            copy_cleaned = unlink_if_matches(record.backup_raw_path, *copied,
                                             record.source_raw_path, record.id)
                               .has_value();
        } else {
            record.backup_revision = *copied;
        }
    }
    if (backup_failure) {
        const auto& failure = *backup_failure;
        const auto cleaned = unlink_if_matches(record.prepared_raw_path, prepared_revision,
                                               record.source_raw_path, record.id);
        auto terminal =
            record_terminal_failure(journal, record, State::prepared, prepared_revision,
                                    std::nullopt, failure, cleaned.has_value() && copy_cleaned);
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(cleaned ? failure : cleaned.error());
    }
    auto backup_source = core::observe_local_source_revision(record.source_raw_path);
    auto backup = core::observe_local_source_revision(record.backup_raw_path);
    if (!backup_source || !backup || *backup_source != record.expected_revision ||
        *backup != backup_identity(record)) {
        const auto failure =
            !backup_source
                ? backup_source.error()
                : (!backup
                       ? backup.error()
                       : operation_error(core::ErrorCode::conflict,
                                         "metadata backup does not preserve the expected source",
                                         record.source_raw_path, record.id));
        const auto backup_cleaned = unlink_if_matches(
            record.backup_raw_path, backup_identity(record), record.source_raw_path, record.id);
        const auto prepared_cleaned = unlink_if_matches(record.prepared_raw_path, prepared_revision,
                                                        record.source_raw_path, record.id);
        const bool restored = backup_cleaned.has_value() && prepared_cleaned.has_value();
        auto terminal = record_terminal_failure(journal, record, State::prepared, prepared_revision,
                                                std::nullopt, failure, restored);
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(
            restored ? failure
                     : (!backup_cleaned ? backup_cleaned.error() : prepared_cleaned.error()));
    }
    auto directory_synced = fsync_parent(record.source_raw_path, record.source_raw_path, record.id);
    if (!directory_synced) {
        const auto& failure = directory_synced.error();
        const auto backup_cleaned = unlink_if_matches(
            record.backup_raw_path, backup_identity(record), record.source_raw_path, record.id);
        const auto prepared_cleaned = unlink_if_matches(record.prepared_raw_path, prepared_revision,
                                                        record.source_raw_path, record.id);
        const bool restored = backup_cleaned.has_value() && prepared_cleaned.has_value();
        auto terminal = record_terminal_failure(journal, record, State::prepared, prepared_revision,
                                                std::nullopt, failure, restored);
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(failure);
    }
    if (cancellation.is_cancellation_requested()) {
        const auto failure = cancelled(record.source_raw_path);
        const auto backup_cleaned = unlink_if_matches(
            record.backup_raw_path, backup_identity(record), record.source_raw_path, record.id);
        const auto prepared_cleaned = unlink_if_matches(record.prepared_raw_path, prepared_revision,
                                                        record.source_raw_path, record.id);
        const bool restored = backup_cleaned.has_value() && prepared_cleaned.has_value();
        auto terminal = record_terminal_failure(journal, record, State::prepared, prepared_revision,
                                                std::nullopt, failure, restored);
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(failure);
    }

    if (::rename(record.prepared_raw_path.c_str(), record.source_raw_path.c_str()) != 0) {
        const auto failure = system_error("publishing prepared metadata failed", errno,
                                          record.source_raw_path, record.id);
        const auto backup_cleaned = unlink_if_matches(
            record.backup_raw_path, backup_identity(record), record.source_raw_path, record.id);
        const auto prepared_cleaned = unlink_if_matches(record.prepared_raw_path, prepared_revision,
                                                        record.source_raw_path, record.id);
        const bool restored = backup_cleaned.has_value() && prepared_cleaned.has_value();
        auto terminal = record_terminal_failure(journal, record, State::prepared, prepared_revision,
                                                std::nullopt, failure, restored);
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(failure);
    }
    auto published_sync = fsync_parent(record.source_raw_path, record.source_raw_path, record.id);
    auto published = core::observe_local_source_revision(record.source_raw_path);
    // The prepared file, renamed into place: on some filesystems with a new
    // inode number (ADR-0249). What is observed now is what is recorded.
    const bool published_identity =
        published &&
        core::same_file_after_rename(record.source_raw_path, prepared_revision, *published);
    if (!published_sync || !published_identity) {
        const auto failure =
            !published_sync
                ? published_sync.error()
                : (!published ? published.error()
                              : operation_error(core::ErrorCode::conflict,
                                                "published metadata has an unexpected revision (" +
                                                    core::describe_revision_change(
                                                        prepared_revision, *published) +
                                                    ")",
                                                record.source_raw_path, record.id));
        const auto rolled_back = rollback_published(record, prepared_revision);
        auto terminal =
            record_terminal_failure(journal, record, State::prepared, prepared_revision,
                                    prepared_revision, failure, rolled_back.has_value());
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(rolled_back ? failure
                                           : not_rolled_back(failure, rolled_back.error()));
    }

    auto published_transition = transition(journal, record, State::prepared, State::published,
                                           prepared_revision, *published);
    if (!published_transition) {
        const auto& failure = published_transition.error();
        const auto rolled_back = rollback_published(record, *published);
        static_cast<void>(record_terminal_failure(journal, record, State::prepared,
                                                  prepared_revision, *published, failure,
                                                  rolled_back.has_value()));
        return std::unexpected(rolled_back ? failure
                                           : not_rolled_back(failure, rolled_back.error()));
    }

    auto reread = verify_published_content(record, *published, cancellation);
    if (!reread || *reread != prepared_document) {
        const auto failure =
            !reread ? reread.error()
                    : operation_error(core::ErrorCode::conflict,
                                      "published metadata document failed reread verification",
                                      record.source_raw_path, record.id);
        const auto rolled_back = rollback_published(record, *published);
        auto terminal =
            record_terminal_failure(journal, record, State::published, prepared_revision,
                                    *published, failure, rolled_back.has_value());
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(rolled_back ? failure
                                           : not_rolled_back(failure, rolled_back.error()));
    }
    auto result = verified_commit_result(record, *published, std::move(*reread));
    auto dependent = dependent_state_committer(*result);
    if (!dependent) {
        const auto& failure = dependent.error();
        const auto rolled_back = rollback_published(record, *published);
        auto terminal =
            record_terminal_failure(journal, record, State::published, prepared_revision,
                                    *published, failure, rolled_back.has_value());
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(rolled_back ? failure
                                           : not_rolled_back(failure, rolled_back.error()));
    }
    auto final_revision = core::observe_local_source_revision(record.source_raw_path);
    if (!final_revision || *final_revision != *published) {
        const auto failure =
            !final_revision
                ? final_revision.error()
                : operation_error(core::ErrorCode::conflict,
                                  "metadata source changed during dependent-state commit",
                                  record.source_raw_path, record.id);
        auto terminal = record_terminal_failure(journal, record, State::published,
                                                prepared_revision, *published, failure, false);
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(failure);
    }
    auto completed = transition(journal, record, State::published, State::complete,
                                prepared_revision, *published);
    if (!completed) {
        return std::unexpected(std::move(completed.error()));
    }
    return result;
}

// ADR-0145: writes a rewritten carrier to the journal's prepared sibling;
// failures terminate the planned record with cleaned debris.
[[nodiscard]] core::Result<core::LocalSourceRevision>
write_prepared_carrier_bytes(const MetadataOperationJournalRecord& record, const std::string& bytes,
                             MetadataOperationJournal& journal) {
    const auto fail = [&record,
                       &journal](core::Error failure,
                                 const bool restored) -> core::Result<core::LocalSourceRevision> {
        auto terminal = record_terminal_failure(journal, record, State::planned, std::nullopt,
                                                std::nullopt, failure, restored);
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(std::move(failure));
    };
    Descriptor prepared{
        ::open(record.prepared_raw_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)};
    if (!prepared.valid()) {
        return fail(system_error("creating the prepared carrier failed", errno,
                                 record.source_raw_path, record.id),
                    true);
    }
    std::size_t written = 0U;
    while (written < bytes.size()) {
        const auto count = ::write(prepared.get(), bytes.data() + written, bytes.size() - written);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            auto failure = system_error("writing the prepared carrier failed", errno,
                                        record.source_raw_path, record.id);
            const bool restored = ::unlink(record.prepared_raw_path.c_str()) == 0;
            return fail(std::move(failure), restored);
        }
        written += static_cast<std::size_t>(count);
    }
    if (::fsync(prepared.get()) != 0) {
        auto failure = system_error("syncing the prepared carrier failed", errno,
                                    record.source_raw_path, record.id);
        const bool restored = ::unlink(record.prepared_raw_path.c_str()) == 0;
        return fail(std::move(failure), restored);
    }
    auto revision = core::observe_local_source_revision(record.prepared_raw_path);
    if (!revision) {
        const bool restored = ::unlink(record.prepared_raw_path.c_str()) == 0;
        return fail(std::move(revision.error()), restored);
    }
    return *revision;
}

// ADR-0145: sidecar creation stays outside the journal — nothing
// pre-existing is at risk and the publish is one atomic rename.
[[nodiscard]] core::Result<void> publish_created_carrier_bytes(const std::string& raw_path,
                                                               const std::string& bytes) {
    const auto prepared_path = raw_path + ".tk-prepared-" + core::StableId::random().to_string();
    Descriptor prepared{
        ::open(prepared_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644)};
    if (!prepared.valid()) {
        return std::unexpected(
            system_error("creating the prepared carrier failed", errno, raw_path));
    }
    std::size_t written = 0U;
    while (written < bytes.size()) {
        const auto count = ::write(prepared.get(), bytes.data() + written, bytes.size() - written);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            const auto failure =
                system_error("writing the prepared carrier failed", errno, raw_path);
            ::unlink(prepared_path.c_str());
            return std::unexpected(failure);
        }
        written += static_cast<std::size_t>(count);
    }
    if (::fsync(prepared.get()) != 0) {
        const auto failure = system_error("syncing the prepared carrier failed", errno, raw_path);
        ::unlink(prepared_path.c_str());
        return std::unexpected(failure);
    }
    if (std::rename(prepared_path.c_str(), raw_path.c_str()) != 0) {
        const auto failure = system_error("publishing the created carrier failed", errno, raw_path);
        ::unlink(prepared_path.c_str());
        return std::unexpected(failure);
    }
    static_cast<void>(fsync_parent(raw_path, raw_path, core::StableId{}));
    return {};
}

} // namespace

core::Result<MetadataCommitResult>
commit_metadata_source(const metadata::MetadataWritePlanSource& source_plan,
                            MetadataOperationJournal& journal,
                            const MetadataDependentStateCommitter& dependent_state_committer,
                            const core::CancellationToken& cancellation) {
    if (source_plan.artwork && source_plan.artwork->folder_image) {
        if (!dependent_state_committer || !source_plan.ready() || !source_plan.observed_revision ||
            source_plan.expected_revision != source_plan.observed_revision ||
            source_plan.raw_path != source_plan.artwork->raw_media_path ||
            source_plan.expected_revision != source_plan.artwork->expected_media_revision ||
            std::filesystem::path{source_plan.raw_path}.parent_path() !=
                std::filesystem::path{source_plan.artwork->folder_image->raw_path}.parent_path())
            return std::unexpected(operation_error(core::ErrorCode::invalid_argument,
                                                   "Incomplete cover plan", source_plan.raw_path));
        auto fresh = metadata::read_local_metadata(source_plan.raw_path, cancellation);
        if (!fresh || fresh->source_revision != *source_plan.observed_revision)
            return std::unexpected(operation_error(core::ErrorCode::conflict,
                                                   "Cover source changed after review",
                                                   source_plan.raw_path));
        auto folder =
            commit_folder_image(*source_plan.artwork->folder_image, journal, cancellation);
        if (!folder)
            return std::unexpected(folder.error());
        auto media = source_plan;
        if (source_plan.artwork->embed) {
            auto embedded =
                std::make_shared<metadata::ArtworkWritePlanSource>(*source_plan.artwork);
            embedded->folder_image.reset();
            media.artwork = std::move(embedded);
        } else {
            media.artwork.reset();
        }
        if (media.changes.empty() && !media.artwork) {
            return MetadataCommitResult{.journal_id = {},
                                        .source_raw_path = media.raw_path,
                                        .backup_raw_path = {},
                                        .previous_revision = fresh->source_revision,
                                        .published_revision = fresh->source_revision,
                                        .document = std::move(fresh->document),
                                        .occurrence_indexes = media.occurrence_indexes,
                                        .content_kind = MetadataOperationContentKind::text_fields};
        }
        auto committed =
            commit_metadata_source(media, journal, dependent_state_committer, cancellation);
        if (!committed)
            committed.error().message =
                "Folder image saved; media save failed: " + committed.error().message;
        return committed;
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_plan.raw_path));
    }
    if (!dependent_state_committer || source_plan.raw_path.empty() ||
        source_plan.raw_path.find('\0') != std::string::npos || !source_plan.ready() ||
        !metadata::is_qualified_text_adapter(source_plan.adapter_name) ||
        !source_plan.expected_revision || !source_plan.observed_revision ||
        *source_plan.expected_revision != *source_plan.observed_revision ||
        (source_plan.changes.empty() && !source_plan.artwork)) {
        return std::unexpected(
            operation_error(core::ErrorCode::invalid_argument,
                            "metadata commit requires a ready native-FLAC plan and state committer",
                            source_plan.raw_path));
    }

    auto process_lock =
        acquire_process_lock(*source_plan.observed_revision, cancellation, source_plan.raw_path);
    if (!process_lock) {
        return std::unexpected(std::move(process_lock.error()));
    }
    const auto incomplete = journal.load_incomplete_for_source(source_plan.raw_path);
    if (!incomplete) {
        return std::unexpected(incomplete.error());
    }
    for (const auto& operation : *incomplete) {
        if (operation.source_raw_path == source_plan.raw_path) {
            return std::unexpected(operation_error(
                core::ErrorCode::conflict,
                "An unfinished operation for this file requires recovery before another save",
                source_plan.raw_path, operation.id));
        }
    }
    auto source_descriptor =
        open_and_lock_file(source_plan.raw_path, cancellation, source_plan.raw_path);
    if (!source_descriptor) {
        return std::unexpected(std::move(source_descriptor.error()));
    }
    auto source_status = require_direct_single_link_source(*source_descriptor, source_plan.raw_path,
                                                           *source_plan.observed_revision);
    if (!source_status) {
        return std::unexpected(std::move(source_status.error()));
    }
    auto source_attributes = read_extended_attributes(*source_descriptor, source_plan.raw_path);
    if (!source_attributes) {
        return std::unexpected(std::move(source_attributes.error()));
    }
    auto fresh = metadata::read_local_metadata(source_plan.raw_path, cancellation);
    if (!fresh) {
        return std::unexpected(std::move(fresh.error()));
    }
    if (fresh->source_revision != *source_plan.observed_revision ||
        fresh->adapter_name != source_plan.adapter_name) {
        return std::unexpected(operation_error(
            core::ErrorCode::conflict,
            "metadata source changed before its commit journal was created", source_plan.raw_path));
    }
    auto originals = verify_plan_originals(fresh->document, source_plan);
    if (!originals) {
        return std::unexpected(std::move(originals.error()));
    }

    const auto journal_id = core::StableId::random();
    auto record = make_journal_record(source_plan, journal_id);
    if (source_plan.artwork) {
        auto inventory = read_embedded_artwork_inventory(source_plan.raw_path, cancellation);
        if (!inventory) {
            return std::unexpected(inventory.error());
        }
        if (inventory->media_revision != *source_plan.observed_revision) {
            return std::unexpected(operation_error(
                core::ErrorCode::conflict, "artwork revision changed", source_plan.raw_path));
        }
        if (!record) {
            return std::unexpected(record.error());
        }
        auto combined = make_artwork_journal_record(*source_plan.artwork, *inventory, journal_id);
        if (!combined) {
            return std::unexpected(combined.error());
        }
        combined->changes = std::move(record->changes);
        combined->occurrence_indexes = source_plan.occurrence_indexes;
        if (!combined->changes.empty()) {
            combined->artwork->kind = metadata::ArtworkWritePlanIntentKind::batch;
            combined->artwork->target_ordinal = 0;
            combined->artwork->original_target_fingerprint.reset();
            combined->artwork->replacement_fingerprint.reset();
        }
        record = std::move(combined);
    }
    if (!record) {
        return std::unexpected(std::move(record.error()));
    }
    auto created = journal.create(*record);
    if (!created) {
        return std::unexpected(std::move(created.error()));
    }

    auto prepared = metadata::prepare_qualified_metadata_write_copy(
        source_plan, record->prepared_raw_path, cancellation);
    if (!prepared) {
        const auto& failure = prepared.error();
        auto terminal = record_terminal_failure(journal, *record, State::planned, std::nullopt,
                                                std::nullopt, failure, true);
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(failure);
    }
    return publish_prepared_metadata_copy(*record, *source_descriptor, *source_status,
                                          *source_attributes, prepared->prepared_revision,
                                          prepared->document, journal, dependent_state_committer,
                                          cancellation);
}

core::Result<MetadataCommitResult>
commit_artwork_source(const metadata::ArtworkWritePlanSource& source_plan,
                      MetadataOperationJournal& journal,
                      const MetadataDependentStateCommitter& dependent_state_committer,
                      const core::CancellationToken& cancellation) {
    if (source_plan.folder_image) {
        auto combined = metadata::merge_artwork_write_plan(
            {}, metadata::ArtworkWritePlan{.sources = {source_plan},
                                           .logical_intent_count =
                                               source_plan.occurrence_indexes.size()});
        if (!combined)
            return std::unexpected(combined.error());
        return commit_metadata_source(combined->sources.front(), journal,
                                           dependent_state_committer, cancellation);
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_plan.raw_media_path));
    }
    if (!dependent_state_committer || source_plan.raw_media_path.empty() ||
        source_plan.raw_media_path.find('\0') != std::string::npos || !source_plan.ready() ||
        !metadata::is_qualified_artwork_adapter(source_plan.adapter_name) ||
        !source_plan.expected_media_revision || !source_plan.observed_media_revision ||
        *source_plan.expected_media_revision != *source_plan.observed_media_revision ||
        (source_plan.change.kind != metadata::ArtworkWritePlanIntentKind::add &&
         !source_plan.change.original) ||
        ((source_plan.change.kind == metadata::ArtworkWritePlanIntentKind::replace ||
          source_plan.change.kind == metadata::ArtworkWritePlanIntentKind::add) &&
         !source_plan.change.replacement)) {
        return std::unexpected(
            operation_error(core::ErrorCode::invalid_argument,
                            "artwork commit requires a ready qualified plan and state committer",
                            source_plan.raw_media_path));
    }

    auto process_lock = acquire_process_lock(*source_plan.observed_media_revision, cancellation,
                                             source_plan.raw_media_path);
    if (!process_lock) {
        return std::unexpected(std::move(process_lock.error()));
    }
    auto source_descriptor =
        open_and_lock_file(source_plan.raw_media_path, cancellation, source_plan.raw_media_path);
    if (!source_descriptor) {
        return std::unexpected(std::move(source_descriptor.error()));
    }
    auto source_status = require_direct_single_link_source(
        *source_descriptor, source_plan.raw_media_path, *source_plan.observed_media_revision);
    if (!source_status) {
        return std::unexpected(std::move(source_status.error()));
    }
    auto source_attributes =
        read_extended_attributes(*source_descriptor, source_plan.raw_media_path);
    if (!source_attributes) {
        return std::unexpected(std::move(source_attributes.error()));
    }
    auto fresh_document = metadata::read_local_metadata(source_plan.raw_media_path, cancellation);
    auto fresh_inventory =
        read_embedded_artwork_inventory(source_plan.raw_media_path, cancellation);
    if (!fresh_document || !fresh_inventory) {
        return std::unexpected(!fresh_document ? std::move(fresh_document.error())
                                               : std::move(fresh_inventory.error()));
    }
    // Each artwork adapter pairs with exactly one text adapter of the same
    // container (ADR-0137).
    const std::string_view expected_text_adapter =
        source_plan.adapter_name == "taglib-id3v2-apic-v1" ? "taglib-mpeg-v1"
        : source_plan.adapter_name == "taglib-mp4-covr-v1" ? "taglib-mp4-v1"
                                                           : "taglib-flac-v1";
    if (fresh_document->source_revision != *source_plan.observed_media_revision ||
        fresh_document->adapter_name != expected_text_adapter ||
        fresh_inventory->media_revision != *source_plan.observed_media_revision ||
        fresh_inventory->embedded_adapter_name != source_plan.adapter_name) {
        return std::unexpected(
            operation_error(core::ErrorCode::conflict,
                            "artwork source changed before its commit journal was created",
                            source_plan.raw_media_path));
    }

    const auto journal_id = core::StableId::random();
    auto record = make_artwork_journal_record(source_plan, *fresh_inventory, journal_id);
    if (!record) {
        return std::unexpected(std::move(record.error()));
    }
    auto created = journal.create(*record);
    if (!created) {
        return std::unexpected(std::move(created.error()));
    }

    auto prepared = metadata::prepare_qualified_artwork_write_copy(
        source_plan, record->prepared_raw_path, cancellation);
    if (!prepared) {
        const auto& failure = prepared.error();
        auto terminal = record_terminal_failure(journal, *record, State::planned, std::nullopt,
                                                std::nullopt, failure, true);
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(failure);
    }
    auto prepared_inventory_fingerprint =
        metadata::fingerprint_embedded_artwork_inventory(prepared->inventory.items);
    if (!prepared_inventory_fingerprint ||
        prepared->inventory.items.size() != record->artwork->planned_item_count ||
        *prepared_inventory_fingerprint != record->artwork->planned_inventory_fingerprint) {
        const auto failure =
            !prepared_inventory_fingerprint
                ? prepared_inventory_fingerprint.error()
                : operation_error(core::ErrorCode::conflict,
                                  "prepared embedded artwork differs from journal evidence",
                                  source_plan.raw_media_path, record->id);
        const auto cleaned =
            unlink_if_matches(record->prepared_raw_path, prepared->prepared_revision,
                              record->source_raw_path, record->id);
        auto terminal =
            record_terminal_failure(journal, *record, State::planned, prepared->prepared_revision,
                                    std::nullopt, failure, cleaned.has_value());
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(cleaned ? failure : cleaned.error());
    }
    return publish_prepared_metadata_copy(*record, *source_descriptor, *source_status,
                                          *source_attributes, prepared->prepared_revision,
                                          prepared->document, journal, dependent_state_committer,
                                          cancellation);
}

core::Result<CueReplayGainCommitResult>
commit_cue_replay_gain_sheet(const metadata::MetadataWritePlanCueSheet& sheet_plan,
                             MetadataOperationJournal& journal,
                             const MetadataDependentStateCommitter& dependent_state_committer,
                             const core::CancellationToken& cancellation) {
    const auto& raw_cue_path = sheet_plan.raw_cue_path;
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(raw_cue_path));
    }
    if (!dependent_state_committer || raw_cue_path.empty() ||
        raw_cue_path.find('\0') != std::string::npos || !sheet_plan.ready() ||
        !sheet_plan.expected_revision || !sheet_plan.observed_revision ||
        *sheet_plan.expected_revision != *sheet_plan.observed_revision ||
        (sheet_plan.tracks.empty() && sheet_plan.album_fields.empty())) {
        return std::unexpected(operation_error(
            core::ErrorCode::invalid_argument,
            "CUE ReplayGain commit requires a ready revision-bound plan and state committer",
            raw_cue_path));
    }
    auto process_lock =
        acquire_process_lock(*sheet_plan.observed_revision, cancellation, raw_cue_path);
    if (!process_lock) {
        return std::unexpected(std::move(process_lock.error()));
    }
    auto source_descriptor = open_and_lock_file(raw_cue_path, cancellation, raw_cue_path);
    if (!source_descriptor) {
        return std::unexpected(std::move(source_descriptor.error()));
    }
    auto source_status = require_direct_single_link_source(*source_descriptor, raw_cue_path,
                                                           *sheet_plan.observed_revision);
    if (!source_status) {
        return std::unexpected(std::move(source_status.error()));
    }
    auto source_attributes = read_extended_attributes(*source_descriptor, raw_cue_path);
    if (!source_attributes) {
        return std::unexpected(std::move(source_attributes.error()));
    }
    const auto journal_id = core::StableId::random();
    auto bytes =
        read_carrier_bytes(raw_cue_path, formats::CueParseLimits{}.source_bytes, journal_id);
    if (!bytes) {
        return std::unexpected(std::move(bytes.error()));
    }
    auto sheet = formats::parse_cue_sheet(*bytes);
    if (!sheet) {
        return std::unexpected(std::move(sheet.error()));
    }

    formats::CueReplayGainUpdate update;
    CueReplayGainCommitResult result{
        .raw_cue_path = raw_cue_path,
        .previous_revision = *sheet_plan.expected_revision,
        .published_revision = {},
        .album_fields = {},
        .tracks = {},
    };
    MetadataOperationJournalRecord record;
    record.id = journal_id;
    record.source_raw_path = raw_cue_path;
    const auto [prepared_path, backup_path] = sibling_paths(raw_cue_path, journal_id);
    record.prepared_raw_path = prepared_path;
    record.backup_raw_path = backup_path;
    record.expected_revision = *sheet_plan.expected_revision;
    record.content_kind = MetadataOperationContentKind::cue_replay_gain;

    const auto convert_field = [&](const metadata::MetadataWritePlanLoudnessField& field,
                                   const std::optional<std::pair<std::size_t, std::size_t>>& track)
        -> core::Result<std::pair<formats::CueReplayGainField, CueReplayGainAppliedField>> {
        CueReplayGainAppliedField applied{
            .canonical_name = field.canonical_name,
            .display_name = std::string(carrier_display_name(field.canonical_name)),
            .value = std::nullopt,
        };
        formats::CueReplayGainField carrier{.update = true, .value = std::nullopt};
        std::vector<std::string> planned;
        if (field.kind != metadata::StagedMetadataPatchKind::remove_field) {
            if (field.values.size() != 1U) {
                return std::unexpected(operation_error(
                    core::ErrorCode::invariant,
                    "a planned CUE ReplayGain replacement must carry exactly one value",
                    raw_cue_path, journal_id));
            }
            auto value =
                canonical_loudness_value(field.canonical_name, field.values.front(), raw_cue_path);
            if (!value) {
                return std::unexpected(std::move(value.error()));
            }
            carrier.value = value->first;
            applied.value = value->second;
            planned = {value->second};
        }
        const auto slug =
            track ? cue_track_slug(track->first, track->second) : std::string{cue_album_slug};
        record.changes.push_back(carrier_change(
            field, slug,
            cue_scope_values(*sheet, track, carrier_display_name(field.canonical_name)),
            std::move(planned)));
        return std::pair{carrier, std::move(applied)};
    };

    for (const auto& field : sheet_plan.album_fields) {
        auto converted = convert_field(field, std::nullopt);
        if (!converted) {
            return std::unexpected(std::move(converted.error()));
        }
        (field.canonical_name == "replaygainalbumgain" ? update.album_gain_db : update.album_peak) =
            converted->first;
        result.album_fields.push_back(std::move(converted->second));
        record.occurrence_indexes.insert(record.occurrence_indexes.end(),
                                         field.item_indexes.begin(), field.item_indexes.end());
    }
    for (const auto& track : sheet_plan.tracks) {
        formats::CueTrackReplayGainUpdate track_update{
            .file_index = track.file_index,
            .track_index = track.track_index,
            .track_gain_db = {},
            .track_peak = {},
        };
        CueReplayGainAppliedTrack applied{
            .file_index = track.file_index,
            .track_index = track.track_index,
            .occurrence_indexes = track.occurrence_indexes,
            .fields = {},
        };
        for (const auto& field : track.fields) {
            auto converted = convert_field(field, std::pair{track.file_index, track.track_index});
            if (!converted) {
                return std::unexpected(std::move(converted.error()));
            }
            (field.canonical_name == "replaygaintrackgain" ? track_update.track_gain_db
                                                           : track_update.track_peak) =
                converted->first;
            applied.fields.push_back(std::move(converted->second));
        }
        update.tracks.push_back(track_update);
        result.tracks.push_back(std::move(applied));
        record.occurrence_indexes.insert(record.occurrence_indexes.end(),
                                         track.occurrence_indexes.begin(),
                                         track.occurrence_indexes.end());
    }
    std::ranges::sort(record.occurrence_indexes);
    record.occurrence_indexes.erase(
        std::unique(record.occurrence_indexes.begin(), record.occurrence_indexes.end()),
        record.occurrence_indexes.end());

    auto created = journal.create(record);
    if (!created) {
        return std::unexpected(std::move(created.error()));
    }
    auto rewritten = formats::rewrite_cue_replay_gain(*bytes, update);
    if (!rewritten) {
        const auto& failure = rewritten.error();
        auto terminal = record_terminal_failure(journal, record, State::planned, std::nullopt,
                                                std::nullopt, failure, true);
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(failure);
    }
    auto prepared_revision = write_prepared_carrier_bytes(record, rewritten->bytes, journal);
    if (!prepared_revision) {
        return std::unexpected(std::move(prepared_revision.error()));
    }
    auto published = publish_prepared_metadata_copy(
        record, *source_descriptor, *source_status, *source_attributes, *prepared_revision, {},
        journal, dependent_state_committer, cancellation);
    if (!published) {
        return std::unexpected(std::move(published.error()));
    }
    result.published_revision = published->published_revision;
    return result;
}

core::Result<LoudnessSidecarCommitResult>
commit_loudness_sidecar(const metadata::MetadataWritePlanSidecar& sidecar_plan,
                        MetadataOperationJournal& journal,
                        const MetadataDependentStateCommitter& dependent_state_committer,
                        const core::CancellationToken& cancellation) {
    const auto& raw_audio_path = sidecar_plan.raw_audio_path;
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(raw_audio_path));
    }
    if (!dependent_state_committer || raw_audio_path.empty() ||
        raw_audio_path.find('\0') != std::string::npos || !sidecar_plan.ready() ||
        !sidecar_plan.expected_revision || !sidecar_plan.observed_revision ||
        *sidecar_plan.expected_revision != *sidecar_plan.observed_revision ||
        sidecar_plan.entries.empty()) {
        return std::unexpected(operation_error(
            core::ErrorCode::invalid_argument,
            "loudness sidecar commit requires a ready revision-bound plan and state committer",
            raw_audio_path));
    }
    auto fresh_audio = core::observe_local_source_revision(raw_audio_path);
    if (!fresh_audio) {
        return std::unexpected(std::move(fresh_audio.error()));
    }
    if (*fresh_audio != *sidecar_plan.expected_revision) {
        return std::unexpected(operation_error(
            core::ErrorCode::conflict, "the source changed after its loudness draft was captured",
            raw_audio_path));
    }

    LoudnessSidecarCommitResult result{
        .raw_audio_path = raw_audio_path,
        .sidecar_raw_path = metadata::loudness_sidecar_path(raw_audio_path),
        .audio_revision = *sidecar_plan.expected_revision,
        .sidecar_removed = false,
        .entries = {},
    };
    auto sidecar_revision = optional_revision(result.sidecar_raw_path);
    if (!sidecar_revision) {
        return std::unexpected(std::move(sidecar_revision.error()));
    }

    std::optional<ProcessSourceLock> process_lock;
    std::optional<Descriptor> sidecar_descriptor;
    std::optional<struct stat> sidecar_status;
    std::optional<ExtendedAttributeListing> sidecar_attributes;
    std::optional<metadata::LoudnessSidecar> pre_image;
    if (*sidecar_revision) {
        auto lock = acquire_process_lock(**sidecar_revision, cancellation, result.sidecar_raw_path);
        if (!lock) {
            return std::unexpected(std::move(lock.error()));
        }
        process_lock.emplace(std::move(*lock));
        auto descriptor =
            open_and_lock_file(result.sidecar_raw_path, cancellation, result.sidecar_raw_path);
        if (!descriptor) {
            return std::unexpected(std::move(descriptor.error()));
        }
        auto status = require_direct_single_link_source(*descriptor, result.sidecar_raw_path,
                                                        **sidecar_revision);
        if (!status) {
            return std::unexpected(std::move(status.error()));
        }
        auto attributes = read_extended_attributes(*descriptor, result.sidecar_raw_path);
        if (!attributes) {
            return std::unexpected(std::move(attributes.error()));
        }
        auto existing = metadata::read_loudness_sidecar(raw_audio_path);
        if (!existing) {
            return std::unexpected(std::move(existing.error()));
        }
        if (!*existing) {
            return std::unexpected(operation_error(core::ErrorCode::conflict,
                                                   "the loudness sidecar vanished while locked",
                                                   result.sidecar_raw_path));
        }
        sidecar_descriptor.emplace(std::move(*descriptor));
        sidecar_status = *status;
        sidecar_attributes = std::move(*attributes);
        pre_image = std::move(**existing);
    }

    // The merge starts from the pre-image only while it still describes
    // the current audio bytes; stale entries drop wholesale (ADR-0141).
    metadata::LoudnessSidecar merged;
    if (pre_image && pre_image->matches(*sidecar_plan.expected_revision)) {
        merged = *pre_image;
    }
    merged.source_size = sidecar_plan.expected_revision->size;
    merged.source_modified_seconds = sidecar_plan.expected_revision->modification_time_seconds;
    merged.source_modified_nanoseconds =
        sidecar_plan.expected_revision->modification_time_nanoseconds;

    const auto journal_id = core::StableId::random();
    MetadataOperationJournalRecord record;
    record.id = journal_id;
    record.source_raw_path = result.sidecar_raw_path;
    const auto [prepared_path, backup_path] = sibling_paths(result.sidecar_raw_path, journal_id);
    record.prepared_raw_path = prepared_path;
    record.backup_raw_path = backup_path;
    record.content_kind = MetadataOperationContentKind::loudness_sidecar;
    if (*sidecar_revision) {
        record.expected_revision = **sidecar_revision;
    }

    for (const auto& planned : sidecar_plan.entries) {
        auto target = metadata::LoudnessSidecarEntry{
            .stream_index = planned.identity.stream_index,
            .subsong_index = planned.identity.subsong_index,
            .start_sample = planned.identity.start_sample,
            .end_sample = planned.identity.end_sample,
            .track_gain_db = std::nullopt,
            .track_peak = std::nullopt,
            .album_gain_db = std::nullopt,
            .album_peak = std::nullopt,
        };
        auto found = std::ranges::find_if(merged.entries,
                                          [&target](const metadata::LoudnessSidecarEntry& entry) {
                                              return entry.same_identity(target);
                                          });
        if (found == merged.entries.end()) {
            merged.entries.push_back(target);
            found = std::prev(merged.entries.end());
        }
        LoudnessSidecarAppliedEntry applied{
            .identity = planned.identity,
            .occurrence_indexes = planned.occurrence_indexes,
            .fields = {},
        };
        bool wrote_peak = false;
        for (const auto& field : planned.fields) {
            const auto is_peak = field.canonical_name == "replaygaintrackpeak" ||
                                 field.canonical_name == "replaygainalbumpeak";
            auto* member = field.canonical_name == "replaygaintrackgain"   ? &found->track_gain_db
                           : field.canonical_name == "replaygaintrackpeak" ? &found->track_peak
                           : field.canonical_name == "replaygainalbumgain" ? &found->album_gain_db
                                                                           : &found->album_peak;
            CueReplayGainAppliedField applied_field{
                .canonical_name = field.canonical_name,
                .display_name = std::string(carrier_display_name(field.canonical_name)),
                .value = std::nullopt,
            };
            std::vector<std::string> planned_values;
            if (field.kind == metadata::StagedMetadataPatchKind::remove_field) {
                member->reset();
            } else {
                if (field.values.size() != 1U) {
                    return std::unexpected(operation_error(
                        core::ErrorCode::invariant,
                        "a planned sidecar replacement must carry exactly one value",
                        raw_audio_path, journal_id));
                }
                auto value = canonical_loudness_value(field.canonical_name, field.values.front(),
                                                      raw_audio_path);
                if (!value) {
                    return std::unexpected(std::move(value.error()));
                }
                *member = value->first;
                applied_field.value = value->second;
                planned_values = {value->second};
                wrote_peak = wrote_peak || is_peak;
            }
            record.changes.push_back(carrier_change(
                field, sidecar_entry_slug(planned.identity),
                pre_image
                    ? sidecar_member_values(*pre_image, planned.identity, field.canonical_name)
                    : std::vector<std::string>{},
                std::move(planned_values)));
            applied.fields.push_back(std::move(applied_field));
        }
        // ADR-0148: freshly written peaks carry the plan's peak kind; an
        // entry whose peaks are all gone has no kind to describe.
        if (wrote_peak) {
            found->true_peak = planned.true_peak;
        }
        if (!found->track_peak && !found->album_peak) {
            found->true_peak = false;
        }
        if (found->empty()) {
            merged.entries.erase(found);
        }
        record.occurrence_indexes.insert(record.occurrence_indexes.end(),
                                         planned.occurrence_indexes.begin(),
                                         planned.occurrence_indexes.end());
        result.entries.push_back(std::move(applied));
    }
    std::ranges::sort(record.occurrence_indexes);
    record.occurrence_indexes.erase(
        std::unique(record.occurrence_indexes.begin(), record.occurrence_indexes.end()),
        record.occurrence_indexes.end());

    auto serialized = metadata::serialize_loudness_sidecar(merged);
    if (!serialized) {
        return std::unexpected(std::move(serialized.error()));
    }

    if (!*sidecar_revision) {
        // ADR-0145: creation stays a direct atomic publish — there is no
        // pre-image to protect and nothing for recovery or undo to do.
        if (merged.entries.empty()) {
            result.sidecar_removed = true;
            return result;
        }
        auto created = publish_created_carrier_bytes(result.sidecar_raw_path, *serialized);
        if (!created) {
            return std::unexpected(std::move(created.error()));
        }
        return result;
    }

    auto created = journal.create(record);
    if (!created) {
        return std::unexpected(std::move(created.error()));
    }
    auto prepared_revision = write_prepared_carrier_bytes(record, *serialized, journal);
    if (!prepared_revision) {
        return std::unexpected(std::move(prepared_revision.error()));
    }
    auto published = publish_prepared_metadata_copy(
        record, *sidecar_descriptor, *sidecar_status, *sidecar_attributes, *prepared_revision, {},
        journal, dependent_state_committer, cancellation);
    if (!published) {
        return std::unexpected(std::move(published.error()));
    }
    return result;
}

core::Result<std::vector<MetadataRecoveryResult>>
recover_metadata_operations(MetadataOperationJournal& journal,
                            const MetadataDependentStateCommitter& dependent_state_committer,
                            const core::CancellationToken& cancellation) {
    if (!dependent_state_committer) {
        return std::unexpected(operation_error(core::ErrorCode::invalid_argument,
                                               "metadata recovery requires a state committer", {}));
    }
    auto records = journal.load_incomplete();
    if (!records) {
        return std::unexpected(std::move(records.error()));
    }
    std::vector<MetadataRecoveryResult> results;
    results.reserve(records->size());
    for (auto& record : *records) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled(record.source_raw_path));
        }
        if (record.state == State::needs_reconciliation || !expected_sibling_paths(record)) {
            results.push_back(MetadataRecoveryResult{
                .journal_id = record.id,
                .outcome = MetadataRecoveryOutcome::needs_reconciliation,
                .issue = record.failure,
            });
            continue;
        }
        if (record.content_kind == MetadataOperationContentKind::folder_image) {
            auto recovered = recover_folder_image(record, journal, cancellation);
            if (!recovered)
                return std::unexpected(recovered.error());
            results.push_back(std::move(*recovered));
            continue;
        }
        auto process_lock =
            acquire_process_lock(record.expected_revision, cancellation, record.source_raw_path);
        if (!process_lock) {
            return std::unexpected(std::move(process_lock.error()));
        }
        auto source_revision = optional_revision(record.source_raw_path);
        auto prepared_revision = optional_revision(record.prepared_raw_path);
        auto backup_revision = optional_revision(record.backup_raw_path);
        if (!source_revision || !prepared_revision || !backup_revision) {
            auto issue = !source_revision     ? std::move(source_revision.error())
                         : !prepared_revision ? std::move(prepared_revision.error())
                                              : std::move(backup_revision.error());
            static_cast<void>(record_terminal_failure(journal, record, record.state,
                                                      record.prepared_revision,
                                                      record.published_revision, issue, false));
            results.push_back({.journal_id = record.id,
                               .outcome = MetadataRecoveryOutcome::needs_reconciliation,
                               .issue = std::move(issue)});
            continue;
        }

        std::optional<Descriptor> recovery_source_lock;
        if (*source_revision) {
            auto locked_source = open_and_lock_file(record.source_raw_path, cancellation,
                                                    record.source_raw_path, record.id);
            if (!locked_source) {
                if (locked_source.error().code == core::ErrorCode::cancelled) {
                    return std::unexpected(std::move(locked_source.error()));
                }
                const auto& issue = locked_source.error();
                auto terminal =
                    record_terminal_failure(journal, record, record.state, record.prepared_revision,
                                            record.published_revision, issue, false);
                if (!terminal) {
                    return std::unexpected(std::move(terminal.error()));
                }
                results.push_back({.journal_id = record.id,
                                   .outcome = MetadataRecoveryOutcome::needs_reconciliation,
                                   .issue = issue});
                continue;
            }
            auto status = locked_status(*locked_source, record.source_raw_path, record.id);
            if (!status || revision_from_stat(*status) != **source_revision) {
                const auto issue =
                    !status ? status.error()
                            : operation_error(core::ErrorCode::conflict,
                                              "metadata source changed while recovery locked it",
                                              record.source_raw_path, record.id);
                auto terminal =
                    record_terminal_failure(journal, record, record.state, record.prepared_revision,
                                            record.published_revision, issue, false);
                if (!terminal) {
                    return std::unexpected(std::move(terminal.error()));
                }
                results.push_back({.journal_id = record.id,
                                   .outcome = MetadataRecoveryOutcome::needs_reconciliation,
                                   .issue = issue});
                continue;
            }
            recovery_source_lock.emplace(std::move(*locked_source));
        }

        // ADR-0266: a rollback a crash interrupted while it put the original
        // back -- the written file set aside: the original put in place if it
        // is not yet, and what is left beside it goes. The names are this
        // operation's own.
        if (auto aside = optional_revision(aside_sibling(record)); !aside || *aside) {
            std::optional<core::Error> issue;
            if (!aside) {
                issue = std::move(aside.error());
            } else if (!*source_revision) {
                if (auto restored = rename_original_in(record); !restored) {
                    issue = std::move(restored.error());
                }
            }
            if (!issue) {
                for (const auto& debris :
                     {aside_sibling(record), restore_sibling(record), record.prepared_raw_path}) {
                    if (auto removed = unlink_if_matches(debris, std::nullopt,
                                                         record.source_raw_path, record.id, true);
                        !removed) {
                        issue = std::move(removed.error());
                        break;
                    }
                }
            }
            if (!issue) {
                if (auto removed = unlink_if_matches(record.backup_raw_path, backup_identity(record),
                                                     record.source_raw_path, record.id);
                    !removed) {
                    issue = std::move(removed.error());
                }
            }
            const bool restored = !issue;
            const auto failure =
                restored ? operation_error(core::ErrorCode::cancelled,
                                           "interrupted metadata rollback was finished",
                                           record.source_raw_path, record.id)
                         : *issue;
            auto terminal =
                record_terminal_failure(journal, record, record.state, record.prepared_revision,
                                        record.published_revision, failure, restored);
            results.push_back({.journal_id = record.id,
                               .outcome = restored ? MetadataRecoveryOutcome::rolled_back
                                                   : MetadataRecoveryOutcome::needs_reconciliation,
                               .issue = restored ? std::nullopt : std::optional{failure}});
            if (!terminal) {
                return std::unexpected(std::move(terminal.error()));
            }
            continue;
        }

        // An original made ready for a rollback a crash stopped before it
        // set anything aside: kept again, and the rollback judged afresh.
        if (auto unstaged = unstage_original(record); !unstaged) {
            const auto& issue = unstaged.error();
            auto terminal =
                record_terminal_failure(journal, record, record.state, record.prepared_revision,
                                        record.published_revision, issue, false);
            results.push_back({.journal_id = record.id,
                               .outcome = MetadataRecoveryOutcome::needs_reconciliation,
                               .issue = issue});
            if (!terminal) {
                return std::unexpected(std::move(terminal.error()));
            }
            continue;
        }
        if (auto kept = optional_revision(record.backup_raw_path); kept) {
            backup_revision = std::move(kept);
        }

        // The original, or -- after a rollback that renamed a copied backup
        // back -- that copy (ADR-0248): nothing was published, or it was
        // undone, so the debris goes. A copy made but not yet journaled is
        // this operation's own and unpublished, and goes by its path.
        if (*source_revision &&
            (**source_revision == record.expected_revision ||
             core::same_file_after_rename(record.source_raw_path, backup_identity(record),
                                          **source_revision))) {
            auto prepared_cleaned = unlink_if_matches(
                record.prepared_raw_path, record.prepared_revision, record.source_raw_path,
                record.id, record.state == State::planned && !record.prepared_revision);
            auto backup_cleaned = unlink_if_matches(
                record.backup_raw_path, backup_identity(record), record.source_raw_path, record.id,
                record.state == State::prepared && !record.backup_revision);
            const bool restored = prepared_cleaned.has_value() && backup_cleaned.has_value();
            const auto issue =
                restored ? operation_error(core::ErrorCode::cancelled,
                                           "interrupted metadata commit was rolled back",
                                           record.source_raw_path, record.id)
                         : (!prepared_cleaned ? prepared_cleaned.error() : backup_cleaned.error());
            auto terminal =
                record_terminal_failure(journal, record, record.state, record.prepared_revision,
                                        record.published_revision, issue, restored);
            results.push_back({.journal_id = record.id,
                               .outcome = restored ? MetadataRecoveryOutcome::rolled_back
                                                   : MetadataRecoveryOutcome::needs_reconciliation,
                               .issue = restored ? std::nullopt : std::optional{issue}});
            if (!terminal) {
                return std::unexpected(std::move(terminal.error()));
            }
            continue;
        }

        // The prepared file renamed into place, perhaps renumbered by that
        // rename (ADR-0249); from here on, as it is observed now.
        const auto recorded_candidate =
            record.published_revision ? record.published_revision : record.prepared_revision;
        const bool published_candidate =
            *source_revision && recorded_candidate &&
            core::same_file_after_rename(record.source_raw_path, *recorded_candidate,
                                         **source_revision) &&
            *backup_revision && **backup_revision == backup_identity(record);
        const auto candidate_revision = published_candidate ? *source_revision : recorded_candidate;
        if (!published_candidate || record.state == State::planned) {
            const auto issue = operation_error(
                core::ErrorCode::conflict,
                "interrupted metadata operation has ambiguous filesystem identities",
                record.source_raw_path, record.id);
            auto terminal =
                record_terminal_failure(journal, record, record.state, record.prepared_revision,
                                        record.published_revision, issue, false);
            if (!terminal) {
                return std::unexpected(std::move(terminal.error()));
            }
            results.push_back({.journal_id = record.id,
                               .outcome = MetadataRecoveryOutcome::needs_reconciliation,
                               .issue = issue});
            continue;
        }

        auto reread = verify_published_content(record, *candidate_revision, cancellation);
        if (!reread) {
            const auto& issue = reread.error();
            const auto rolled_back = rollback_published(record, *candidate_revision);
            auto terminal =
                record_terminal_failure(journal, record, record.state, record.prepared_revision,
                                        *candidate_revision, issue, rolled_back.has_value());
            if (!terminal) {
                return std::unexpected(std::move(terminal.error()));
            }
            results.push_back({
                .journal_id = record.id,
                .outcome = rolled_back ? MetadataRecoveryOutcome::rolled_back
                                       : MetadataRecoveryOutcome::needs_reconciliation,
                .issue =
                    rolled_back ? std::nullopt : std::optional<core::Error>{rolled_back.error()},
            });
            continue;
        }

        auto commit_result =
            verified_commit_result(record, *candidate_revision, std::move(*reread));
        auto dependent = dependent_state_committer(*commit_result);
        if (!dependent) {
            const auto& issue = dependent.error();
            const auto rolled_back = rollback_published(record, *candidate_revision);
            auto terminal =
                record_terminal_failure(journal, record, record.state, record.prepared_revision,
                                        *candidate_revision, issue, rolled_back.has_value());
            if (!terminal) {
                return std::unexpected(std::move(terminal.error()));
            }
            results.push_back({
                .journal_id = record.id,
                .outcome = rolled_back ? MetadataRecoveryOutcome::rolled_back
                                       : MetadataRecoveryOutcome::needs_reconciliation,
                .issue =
                    rolled_back ? std::nullopt : std::optional<core::Error>{rolled_back.error()},
            });
            continue;
        }
        auto final_revision = core::observe_local_source_revision(record.source_raw_path);
        if (!final_revision || *final_revision != *candidate_revision) {
            const auto issue =
                !final_revision
                    ? final_revision.error()
                    : operation_error(core::ErrorCode::conflict,
                                      "metadata source changed during recovery state commit",
                                      record.source_raw_path, record.id);
            auto terminal =
                record_terminal_failure(journal, record, record.state, record.prepared_revision,
                                        *candidate_revision, issue, false);
            if (!terminal) {
                return std::unexpected(std::move(terminal.error()));
            }
            results.push_back({.journal_id = record.id,
                               .outcome = MetadataRecoveryOutcome::needs_reconciliation,
                               .issue = issue});
            continue;
        }
        if (record.state == State::prepared) {
            auto published_transition =
                transition(journal, record, State::prepared, State::published,
                           record.prepared_revision, *candidate_revision);
            if (!published_transition) {
                return std::unexpected(std::move(published_transition.error()));
            }
            record.state = State::published;
            record.published_revision = candidate_revision;
        }
        auto completed = transition(journal, record, State::published, State::complete,
                                    record.prepared_revision, *candidate_revision);
        if (!completed) {
            return std::unexpected(std::move(completed.error()));
        }
        results.push_back({.journal_id = record.id,
                           .outcome = MetadataRecoveryOutcome::completed,
                           .issue = std::nullopt});
    }

    auto backups = journal.load_backups();
    if (!backups) {
        return std::unexpected(std::move(backups.error()));
    }
    for (auto& backup : *backups) {
        if (backup.state == BackupState::needs_reconciliation) {
            // A damaged record can be both incomplete and backed up; it is one
            // incident, reported once.
            if (std::ranges::contains(results, backup.operation.id,
                                      &MetadataRecoveryResult::journal_id))
                continue;
            results.push_back({.journal_id = backup.operation.id,
                               .outcome = MetadataRecoveryOutcome::needs_reconciliation,
                               .issue = backup.failure});
            continue;
        }
        if (backup.state != BackupState::undoing) {
            continue;
        }
        auto undone =
            finish_metadata_undo(backup, journal, dependent_state_committer, cancellation);
        if (undone) {
            results.push_back({.journal_id = backup.operation.id,
                               .outcome = MetadataRecoveryOutcome::undone,
                               .issue = std::nullopt});
            continue;
        }
        auto current = journal.load_backup(backup.operation.id);
        if (!current || !*current) {
            return std::unexpected(
                current ? operation_error(core::ErrorCode::database,
                                          "metadata undo recovery lost its journal record",
                                          backup.operation.source_raw_path, backup.operation.id)
                        : std::move(current.error()));
        }
        const auto outcome = (**current).state == BackupState::retained
                                 ? MetadataRecoveryOutcome::rolled_back
                                 : MetadataRecoveryOutcome::needs_reconciliation;
        results.push_back(
            {.journal_id = backup.operation.id, .outcome = outcome, .issue = undone.error()});
    }
    return results;
}

namespace detail {

// A filesystem without hard links says so in one of these: SMB on macOS and
// Linux CIFS without unix extensions (ENOTSUP), FAT and exFAT (EPERM), and
// FUSE mounts that do not implement link (ENOSYS). A source already at its
// link limit (EMLINK) is copied the same way, as is one whose undo copies
// are kept on another filesystem (EXDEV, ADR-0266).
bool hard_link_refused(const int number) noexcept {
    return number == ENOTSUP || number == EOPNOTSUPP || number == EPERM || number == ENOSYS ||
           number == EMLINK || number == EXDEV;
}

bool filesystem_without_links_simulated() noexcept {
    return copied_backups_for_testing.load(std::memory_order_relaxed);
}

} // namespace detail

void use_copied_metadata_backups_for_testing(const bool enabled) noexcept {
    copied_backups_for_testing.store(enabled, std::memory_order_relaxed);
}

core::Result<MetadataCommitResult>
undo_metadata_operation(const core::StableId& journal_id, MetadataOperationJournal& journal,
                             const MetadataDependentStateCommitter& dependent_state_committer,
                             const core::CancellationToken& cancellation) {
    if (journal_id.is_nil() || !dependent_state_committer) {
        return std::unexpected(operation_error(core::ErrorCode::invalid_argument,
                                               "metadata undo requires an operation and state "
                                               "committer",
                                               {}));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled({}));
    }
    auto loaded = journal.load_backup(journal_id);
    if (!loaded || !*loaded) {
        return std::unexpected(loaded ? operation_error(core::ErrorCode::not_found,
                                                        "metadata operation has no retained backup",
                                                        {}, journal_id)
                                      : std::move(loaded.error()));
    }
    auto backup = std::move(**loaded);
    const auto& record = backup.operation;
    if (record.content_kind == MetadataOperationContentKind::folder_image &&
        !record.changes.front().original_present)
        return std::unexpected(operation_error(
            core::ErrorCode::unsupported,
            "A newly created folder image has no prior image to restore", record.source_raw_path));
    if (backup.state != BackupState::retained || !record.published_revision) {
        return std::unexpected(operation_error(core::ErrorCode::conflict,
                                               "metadata backup is not available for undo",
                                               record.source_raw_path, journal_id));
    }
    auto source_revision = optional_revision(record.source_raw_path);
    auto backup_revision = optional_revision(record.backup_raw_path);
    if (!source_revision || !backup_revision || !*source_revision || !*backup_revision ||
        **source_revision != *record.published_revision ||
        **backup_revision != backup_identity(record)) {
        auto issue =
            !source_revision ? std::move(source_revision.error())
            : !backup_revision
                ? std::move(backup_revision.error())
                : operation_error(core::ErrorCode::conflict,
                                  "metadata source or retained backup changed since the completed "
                                  "operation",
                                  record.source_raw_path, journal_id);
        auto marked = transition_backup(journal, journal_id, BackupState::retained,
                                        BackupState::needs_reconciliation, std::nullopt, issue);
        return std::unexpected(marked ? issue : std::move(marked.error()));
    }
    const auto undo_id = core::StableId::random();
    auto begun = transition_backup(journal, journal_id, BackupState::retained, BackupState::undoing,
                                   undo_id);
    if (!begun) {
        return std::unexpected(std::move(begun.error()));
    }
    backup.state = BackupState::undoing;
    backup.undo_id = undo_id;
    return finish_metadata_undo(std::move(backup), journal, dependent_state_committer,
                                cancellation);
}

core::Result<void> release_metadata_backup(const core::StableId& journal_id,
                                           MetadataOperationJournal& journal,
                                           const core::CancellationToken& cancellation) {
    if (journal_id.is_nil()) {
        return std::unexpected(operation_error(core::ErrorCode::invalid_argument,
                                               "metadata backup release requires an operation",
                                               {}));
    }
    auto loaded = journal.load_backup(journal_id);
    if (!loaded || !*loaded) {
        return std::unexpected(loaded ? operation_error(core::ErrorCode::not_found,
                                                        "metadata operation has no backup record",
                                                        {}, journal_id)
                                      : std::move(loaded.error()));
    }
    const auto& backup = **loaded;
    const auto& record = backup.operation;
    if (backup.state != BackupState::retained) {
        return std::unexpected(operation_error(core::ErrorCode::conflict,
                                               "metadata backup is not retained",
                                               record.source_raw_path, journal_id));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(record.source_raw_path));
    }
    auto observed = optional_revision(record.backup_raw_path);
    if (!observed) {
        auto issue = std::move(observed.error());
        auto marked = transition_backup(journal, journal_id, BackupState::retained,
                                        BackupState::needs_reconciliation, std::nullopt, issue);
        return std::unexpected(marked ? issue : std::move(marked.error()));
    }
    if (*observed && **observed != backup_identity(record)) {
        auto issue = operation_error(core::ErrorCode::conflict,
                                     "retained metadata backup has an unexpected identity",
                                     record.source_raw_path, journal_id);
        auto marked = transition_backup(journal, journal_id, BackupState::retained,
                                        BackupState::needs_reconciliation, std::nullopt, issue);
        return std::unexpected(marked ? issue : std::move(marked.error()));
    }
    std::optional<Descriptor> locked_backup;
    if (*observed) {
        auto locked = open_and_lock_file(record.backup_raw_path, cancellation,
                                         record.source_raw_path, journal_id);
        if (!locked) {
            auto issue = std::move(locked.error());
            if (issue.code == core::ErrorCode::cancelled) {
                return std::unexpected(std::move(issue));
            }
            auto marked = transition_backup(journal, journal_id, BackupState::retained,
                                            BackupState::needs_reconciliation, std::nullopt, issue);
            if (!marked) {
                return std::unexpected(marked.error());
            }
            return std::unexpected(issue);
        }
        if (auto verified =
                verify_direct_single_link(record.backup_raw_path, *locked, backup_identity(record),
                                          record, "retained metadata backup");
            !verified) {
            const auto& issue = verified.error();
            auto marked = transition_backup(journal, journal_id, BackupState::retained,
                                            BackupState::needs_reconciliation, std::nullopt, issue);
            if (!marked) {
                return std::unexpected(marked.error());
            }
            return std::unexpected(issue);
        }
        locked_backup.emplace(std::move(*locked));
        auto removed = unlink_if_matches(record.backup_raw_path, backup_identity(record),
                                         record.source_raw_path, journal_id);
        if (!removed) {
            const auto& issue = removed.error();
            auto marked = transition_backup(journal, journal_id, BackupState::retained,
                                            BackupState::needs_reconciliation, std::nullopt, issue);
            if (!marked) {
                return std::unexpected(marked.error());
            }
            return std::unexpected(issue);
        }
    }
    return transition_backup(journal, journal_id, BackupState::retained, BackupState::released);
}

core::Result<std::vector<MetadataBackupMaintenanceResult>> maintain_metadata_backups(
    MetadataOperationJournal& journal, const MetadataBackupRetentionPolicy& policy,
    const std::int64_t now_unix_seconds, const core::CancellationToken& cancellation) {
    if (policy.maximum_age_seconds < 0 || now_unix_seconds <= 0) {
        return std::unexpected(operation_error(core::ErrorCode::invalid_argument,
                                               "metadata backup retention policy is invalid", {}));
    }
    auto backups = journal.load_backups();
    if (!backups) {
        return std::unexpected(std::move(backups.error()));
    }
    // load_backups() is newest-first. Validate the current source against the
    // newest completed operation for each exact raw path once; only then is it
    // safe to release older backups for that path.
    std::unordered_map<std::string, bool> source_is_unambiguous;
    source_is_unambiguous.reserve(backups->size());
    std::size_t retained_count = 0U;
    std::uint64_t retained_bytes = 0U;
    std::vector<MetadataBackupMaintenanceResult> results;
    results.reserve(backups->size());
    for (const auto& backup : *backups) {
        const auto [source_state, is_newest] =
            source_is_unambiguous.emplace(backup.operation.source_raw_path, false);
        if (is_newest) {
            std::optional<core::LocalSourceRevision> expected_source;
            if (backup.state == BackupState::retained || backup.state == BackupState::released) {
                expected_source = backup.operation.published_revision;
            } else if (backup.state == BackupState::undone) {
                expected_source = backup_identity(backup.operation);
            }
            if (expected_source) {
                auto locked =
                    open_and_lock_file(backup.operation.source_raw_path, cancellation,
                                       backup.operation.source_raw_path, backup.operation.id);
                if (locked && verify_direct_single_link(backup.operation.source_raw_path, *locked,
                                                        *expected_source, backup.operation,
                                                        "published metadata source")) {
                    source_state->second = true;
                }
            }
        }
        if (backup.state == BackupState::needs_reconciliation) {
            results.push_back({.journal_id = backup.operation.id,
                               .outcome = MetadataBackupMaintenanceOutcome::needs_reconciliation,
                               .issue = backup.failure});
            continue;
        }
        if (backup.state != BackupState::retained) {
            continue;
        }
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled(backup.operation.source_raw_path));
        }
        if (!source_state->second) {
            auto issue = operation_error(
                core::ErrorCode::conflict,
                "published metadata source changed while an undo backup is retained",
                backup.operation.source_raw_path, backup.operation.id);
            auto marked = transition_backup(journal, backup.operation.id, BackupState::retained,
                                            BackupState::needs_reconciliation, std::nullopt, issue);
            results.push_back({
                .journal_id = backup.operation.id,
                .outcome = MetadataBackupMaintenanceOutcome::needs_reconciliation,
                .issue = marked ? std::optional<core::Error>{std::move(issue)}
                                : std::optional<core::Error>{std::move(marked.error())},
            });
            continue;
        }
        auto retained_revision = optional_revision(backup.operation.backup_raw_path);
        if (!retained_revision) {
            auto issue = std::move(retained_revision.error());
            auto marked = transition_backup(journal, backup.operation.id, BackupState::retained,
                                            BackupState::needs_reconciliation, std::nullopt, issue);
            results.push_back({
                .journal_id = backup.operation.id,
                .outcome = MetadataBackupMaintenanceOutcome::needs_reconciliation,
                .issue = marked ? std::optional<core::Error>{std::move(issue)}
                                : std::optional<core::Error>{std::move(marked.error())},
            });
            continue;
        }
        if (!*retained_revision || **retained_revision != backup_identity(backup.operation)) {
            auto released = release_metadata_backup(backup.operation.id, journal, cancellation);
            results.push_back({
                .journal_id = backup.operation.id,
                .outcome = released ? MetadataBackupMaintenanceOutcome::released
                                    : MetadataBackupMaintenanceOutcome::needs_reconciliation,
                .issue = released ? std::nullopt
                                  : std::optional<core::Error>{std::move(released.error())},
            });
            continue;
        }
        const bool expired =
            now_unix_seconds > backup.completed_at_unix_seconds &&
            now_unix_seconds - backup.completed_at_unix_seconds > policy.maximum_age_seconds;
        const auto bytes = backup.operation.expected_revision.size;
        const bool count_available = retained_count < policy.maximum_entries;
        const bool bytes_available =
            bytes <=
            policy.maximum_total_bytes - std::min(retained_bytes, policy.maximum_total_bytes);
        const bool keep = is_newest && expired == false && count_available && bytes_available;
        if (keep) {
            auto locked = open_and_lock_file(backup.operation.backup_raw_path, cancellation,
                                             backup.operation.source_raw_path, backup.operation.id);
            if (!locked) {
                auto issue = std::move(locked.error());
                if (issue.code == core::ErrorCode::cancelled) {
                    return std::unexpected(std::move(issue));
                }
                auto marked =
                    transition_backup(journal, backup.operation.id, BackupState::retained,
                                      BackupState::needs_reconciliation, std::nullopt, issue);
                results.push_back({
                    .journal_id = backup.operation.id,
                    .outcome = MetadataBackupMaintenanceOutcome::needs_reconciliation,
                    .issue = marked ? std::optional<core::Error>{std::move(issue)}
                                    : std::optional<core::Error>{std::move(marked.error())},
                });
                continue;
            }
            if (auto verified = verify_direct_single_link(
                    backup.operation.backup_raw_path, *locked, backup_identity(backup.operation),
                    backup.operation, "retained metadata backup");
                !verified) {
                auto issue = std::move(verified.error());
                auto marked =
                    transition_backup(journal, backup.operation.id, BackupState::retained,
                                      BackupState::needs_reconciliation, std::nullopt, issue);
                results.push_back({
                    .journal_id = backup.operation.id,
                    .outcome = MetadataBackupMaintenanceOutcome::needs_reconciliation,
                    .issue = marked ? std::optional<core::Error>{std::move(issue)}
                                    : std::optional<core::Error>{std::move(marked.error())},
                });
                continue;
            }
            ++retained_count;
            retained_bytes += bytes;
            results.push_back({.journal_id = backup.operation.id,
                               .outcome = MetadataBackupMaintenanceOutcome::retained,
                               .issue = std::nullopt});
            continue;
        }
        auto released = release_metadata_backup(backup.operation.id, journal, cancellation);
        results.push_back({
            .journal_id = backup.operation.id,
            .outcome = released ? MetadataBackupMaintenanceOutcome::released
                                : MetadataBackupMaintenanceOutcome::needs_reconciliation,
            .issue =
                released ? std::nullopt : std::optional<core::Error>{std::move(released.error())},
        });
    }
    return results;
}

} // namespace trackknife::operations
