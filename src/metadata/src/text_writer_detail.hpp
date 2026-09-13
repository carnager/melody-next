// SPDX-License-Identifier: GPL-3.0-only

// Format-agnostic core of the prepared-copy text writers (ADR-0043 FLAC,
// ADR-0095 WavPack). Everything here operates on plans, documents, and
// TagLib's generic PropertyMap surface; per-format container knowledge —
// layout parsing, binary preservation, file construction — stays with the
// owning writer. The ADR-0087 paired-totals rules live here exactly once so
// the writers can never diverge on them.

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/metadata/flac_mapping.hpp"
#include "trackknife/metadata/write_plan.hpp"

#include <tfile.h>
#include <tpropertymap.h>
#include <tstring.h>
#include <tstringlist.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <map>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace trackknife::metadata::text_writer_detail {

inline constexpr std::size_t copy_buffer_size = 1U * 1024U * 1024U;

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
        const auto descriptor = std::exchange(value_, -1);
        return ::close(descriptor) == 0;
    }

  private:
    int value_{-1};
};

class PreparedPathGuard final {
  public:
    explicit PreparedPathGuard(std::string raw_path) : raw_path_{std::move(raw_path)} {}
    PreparedPathGuard(const PreparedPathGuard&) = delete;
    PreparedPathGuard& operator=(const PreparedPathGuard&) = delete;
    ~PreparedPathGuard() {
        if (owned_) {
            static_cast<void>(::unlink(raw_path_.c_str()));
        }
    }

    void take_ownership() noexcept { owned_ = true; }
    void release() noexcept { owned_ = false; }

  private:
    std::string raw_path_;
    bool owned_{false};
};

[[nodiscard]] inline core::Error writer_error(const core::ErrorCode code, std::string message,
                                              const std::string& source_raw_path,
                                              const std::string& prepared_raw_path = {}) {
    core::Error result{
        .code = code,
        .message = std::move(message),
        .context = {{.key = "source", .value = core::escape_raw_path(source_raw_path)}},
    };
    if (!prepared_raw_path.empty()) {
        result.context.push_back(
            {.key = "prepared", .value = core::escape_raw_path(prepared_raw_path)});
    }
    return result;
}

[[nodiscard]] inline core::Error system_error(const std::string_view operation, const int number,
                                              const std::string& source_raw_path,
                                              const std::string& prepared_raw_path = {}) {
    return writer_error(core::ErrorCode::io,
                        std::string{operation} + ": " +
                            std::error_code{number, std::generic_category()}.message(),
                        source_raw_path, prepared_raw_path);
}

[[nodiscard]] inline core::Error cancelled(const std::string_view format_label,
                                           const std::string& source_raw_path,
                                           const std::string& prepared_raw_path) {
    return writer_error(core::ErrorCode::cancelled,
                        "prepared " + std::string{format_label} + " write was cancelled",
                        source_raw_path, prepared_raw_path);
}

[[nodiscard]] inline bool same_revision(const struct stat& observed,
                                        const core::LocalSourceRevision& expected) {
    return observed.st_size >= 0 &&
           static_cast<std::uint64_t>(observed.st_dev) == expected.device &&
           static_cast<std::uint64_t>(observed.st_ino) == expected.inode &&
           static_cast<std::uint64_t>(observed.st_size) == expected.size &&
           static_cast<std::int64_t>(observed.st_mtim.tv_sec) ==
               expected.modification_time_seconds &&
           static_cast<std::int64_t>(observed.st_mtim.tv_nsec) ==
               expected.modification_time_nanoseconds;
}

[[nodiscard]] inline core::Result<mode_t> copy_source_exclusively(
    const std::string_view format_label, const std::string& source_raw_path,
    const core::LocalSourceRevision& expected_revision, const std::string& prepared_raw_path,
    const core::CancellationToken& cancellation, PreparedPathGuard& prepared_guard) {
    const std::string label{format_label};
    Descriptor source{::open(source_raw_path.c_str(), O_RDONLY | O_CLOEXEC)};
    if (!source.valid()) {
        return std::unexpected(system_error("opening " + label + " source for copy failed", errno,
                                            source_raw_path, prepared_raw_path));
    }
    struct stat source_status{};
    if (::fstat(source.get(), &source_status) != 0) {
        return std::unexpected(system_error("observing opened " + label + " source failed", errno,
                                            source_raw_path, prepared_raw_path));
    }
    if (!S_ISREG(source_status.st_mode) || !same_revision(source_status, expected_revision)) {
        return std::unexpected(writer_error(
            core::ErrorCode::conflict, label + " source changed after the write plan was previewed",
            source_raw_path, prepared_raw_path));
    }

    Descriptor destination{
        ::open(prepared_raw_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)};
    if (!destination.valid()) {
        const auto number = errno;
        const auto code = number == EEXIST ? core::ErrorCode::conflict : core::ErrorCode::io;
        return std::unexpected(writer_error(
            code,
            "creating exclusive prepared " + label +
                " copy failed: " + std::error_code{number, std::generic_category()}.message(),
            source_raw_path, prepared_raw_path));
    }
    prepared_guard.take_ownership();

    std::vector<char> buffer(copy_buffer_size);
    while (true) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled(format_label, source_raw_path, prepared_raw_path));
        }
        const auto read_count = ::read(source.get(), buffer.data(), buffer.size());
        if (read_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(system_error("reading " + label + " source copy failed", errno,
                                                source_raw_path, prepared_raw_path));
        }
        if (read_count == 0) {
            break;
        }
        std::size_t written = 0U;
        while (written < static_cast<std::size_t>(read_count)) {
            const auto write_count = ::write(destination.get(), buffer.data() + written,
                                             static_cast<std::size_t>(read_count) - written);
            if (write_count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return std::unexpected(system_error("writing prepared " + label + " copy failed",
                                                    errno, source_raw_path, prepared_raw_path));
            }
            if (write_count == 0) {
                return std::unexpected(writer_error(
                    core::ErrorCode::io, "prepared " + label + " copy made no write progress",
                    source_raw_path, prepared_raw_path));
            }
            written += static_cast<std::size_t>(write_count);
        }
    }
    if (!destination.close()) {
        return std::unexpected(system_error("closing prepared " + label + " copy failed", errno,
                                            source_raw_path, prepared_raw_path));
    }
    return source_status.st_mode & 07777;
}

using EffectiveText = std::map<std::string, std::vector<std::string>>;

struct EffectiveNativeTextField {
    std::string canonical_name;
    std::vector<std::string> values;

    friend bool operator==(const EffectiveNativeTextField&,
                           const EffectiveNativeTextField&) = default;
};

using EffectiveNativeText = std::map<std::string, EffectiveNativeTextField>;

[[nodiscard]] inline std::string_view
mapping_native_name(const MetadataWritePlanChange& change) noexcept {
    return change.exact_native_name && change.native_name.empty()
               ? std::string_view{change.display_name}
               : std::string_view{change.native_name};
}

[[nodiscard]] inline EffectiveText effective_text(const MetadataDocument& document) {
    EffectiveText result;
    for (const auto& field : document.effective_fields()) {
        result.emplace(field.canonical_name, field.values);
    }
    return result;
}

[[nodiscard]] inline EffectiveNativeText effective_native_text(const MetadataDocument& document) {
    EffectiveNativeText result;
    for (const auto& field : document.effective_native_fields()) {
        result.emplace(canonicalize_native_field_name(field.native_name),
                       EffectiveNativeTextField{.canonical_name = field.canonical_name,
                                                .values = field.values});
    }
    return result;
}

// The case-folded native names one change addresses. An exact-native target
// that is itself a paired totals spelling addresses the whole pair — the
// two spellings are one identity (writes refresh both, removal clears
// both), so the partner never survives to resurrect the value.
[[nodiscard]] inline std::vector<std::string>
exact_native_targets(const MetadataWritePlanChange& change, const bool paired_totals = true) {
    std::vector<std::string> targets{*change.exact_native_name};
    const auto paired = paired_totals ? paired_flac_property_names(change.canonical_name)
                                      : std::vector<std::string>{};
    const auto pair_member =
        std::ranges::any_of(paired, [&change](const std::string& property_name) {
            return canonicalize_native_field_name(property_name) == *change.exact_native_name;
        });
    if (pair_member) {
        for (const auto& property_name : paired) {
            auto folded = canonicalize_native_field_name(property_name);
            if (!std::ranges::contains(targets, folded)) {
                targets.push_back(std::move(folded));
            }
        }
    }
    return targets;
}

[[nodiscard]] inline core::Result<void>
verify_text_result(const std::string_view format_label, const MetadataDocument& before,
                   const MetadataDocument& after, const MetadataWritePlanSource& source_plan,
                   const std::string& prepared_raw_path, const bool paired_totals = true) {
    auto expected = effective_native_text(before);
    for (const auto& change : source_plan.changes) {
        const auto& intent = change.intents.front();
        if (change.exact_native_name) {
            const auto targets = exact_native_targets(change, paired_totals);
            for (const auto& target : targets) {
                expected.erase(target);
            }
            if (intent.kind == StagedMetadataPatchKind::replace_values) {
                auto mapping =
                    map_flac_text_field(change.canonical_name, change.display_name,
                                        mapping_native_name(change), intent.kind, intent.values);
                if (!mapping) {
                    return std::unexpected(std::move(mapping.error()));
                }
                // A pair-member target writes both spellings and the reread
                // load rule surfaces only the pair primary.
                const auto paired = paired_totals
                                        ? paired_flac_property_names(change.canonical_name)
                                        : std::vector<std::string>{};
                const auto& property_name =
                    targets.size() > 1U ? paired.front() : mapping->property_name;
                const auto identity = resolve_text_property_identity(property_name);
                expected[canonicalize_native_field_name(property_name)] = EffectiveNativeTextField{
                    .canonical_name = identity.canonical_name, .values = intent.values};
            }
            continue;
        }
        std::erase_if(expected, [&change](const auto& entry) {
            const auto identity = resolve_text_property_identity(entry.first);
            return identity.conventional && identity.canonical_name == change.canonical_name;
        });
        if (intent.kind == StagedMetadataPatchKind::replace_values) {
            auto mapping =
                map_flac_text_field(change.canonical_name, change.display_name,
                                    mapping_native_name(change), intent.kind, intent.values);
            if (!mapping) {
                return std::unexpected(std::move(mapping.error()));
            }
            // Paired identities are written under every paired spelling, but
            // the reread applies the Picard load rule and surfaces only the
            // primary; expect exactly that primary.
            const auto paired = paired_totals ? paired_flac_property_names(change.canonical_name)
                                              : std::vector<std::string>{};
            const auto& property_name = paired.empty() ? mapping->property_name : paired.front();
            const auto identity = resolve_text_property_identity(property_name);
            expected[canonicalize_native_field_name(property_name)] = EffectiveNativeTextField{
                .canonical_name = identity.canonical_name, .values = intent.values};
        }
    }
    if (before.unsupported_native_objects != after.unsupported_native_objects) {
        return std::unexpected(writer_error(core::ErrorCode::conflict,
                                            "prepared " + std::string{format_label} +
                                                " changed unsupported native metadata identities",
                                            source_plan.raw_path, prepared_raw_path));
    }
    if (expected != effective_native_text(after)) {
        return std::unexpected(writer_error(core::ErrorCode::conflict,
                                            "prepared " + std::string{format_label} +
                                                " metadata reread differs from the plan",
                                            source_plan.raw_path, prepared_raw_path));
    }
    return {};
}

[[nodiscard]] inline core::Result<void>
verify_plan_originals(const std::string_view format_label, const MetadataDocument& before,
                      const MetadataWritePlanSource& source_plan,
                      const std::string& prepared_raw_path) {
    const auto current = effective_text(before);
    const auto current_native = effective_native_text(before);
    for (const auto& change : source_plan.changes) {
        bool present = false;
        std::vector<std::string> values;
        if (change.exact_native_name) {
            const auto found = current_native.find(*change.exact_native_name);
            present = found != current_native.end();
            if (present) {
                values = found->second.values;
            }
        } else {
            const auto found = current.find(change.canonical_name);
            present = found != current.end();
            if (present) {
                values = found->second;
            }
        }
        if (present != change.original_present || (present && values != change.original_values)) {
            return std::unexpected(
                writer_error(core::ErrorCode::conflict,
                             "fresh " + std::string{format_label} +
                                 " metadata differs from the previewed original values",
                             source_plan.raw_path, prepared_raw_path));
        }
    }
    return {};
}

// Applies every planned change through TagLib's generic property surface on
// an already-validated file. The caller owns construction and save().
template <typename File>
[[nodiscard]] inline core::Result<void> apply_text_changes_to_properties(
    const std::string_view format_label, const MetadataWritePlanSource& source_plan, File& file,
    const std::string& prepared_raw_path, const core::CancellationToken& cancellation,
    const bool paired_totals = true) {
    auto properties = file.properties();
    for (const auto& change : source_plan.changes) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(
                cancelled(format_label, source_plan.raw_path, prepared_raw_path));
        }
        const auto& intent = change.intents.front();
        auto mapping = map_flac_text_field(change.canonical_name, change.display_name,
                                           mapping_native_name(change), intent.kind, intent.values);
        if (!mapping) {
            auto mapping_error = std::move(mapping.error());
            mapping_error.context.push_back(
                {.key = "source", .value = core::escape_raw_path(source_plan.raw_path)});
            return std::unexpected(std::move(mapping_error));
        }

        const auto exact_targets = change.exact_native_name
                                       ? exact_native_targets(change, paired_totals)
                                       : std::vector<std::string>{};
        std::vector<TagLib::String> aliases;
        for (auto property = properties.cbegin(); property != properties.cend(); ++property) {
            const auto native_name = property->first.to8Bit(true);
            const auto matches =
                change.exact_native_name
                    ? std::ranges::contains(exact_targets,
                                            canonicalize_native_field_name(native_name))
                    : [&] {
                          const auto identity = resolve_text_property_identity(native_name);
                          return identity.conventional &&
                                 identity.canonical_name == change.canonical_name;
                      }();
            if (matches) {
                aliases.push_back(property->first);
            }
        }
        for (const auto& alias : aliases) {
            properties.erase(alias);
        }
        if (intent.kind == StagedMetadataPatchKind::replace_values) {
            TagLib::StringList values;
            for (const auto& value : intent.values) {
                values.append(TagLib::String{value, TagLib::String::UTF8});
            }
            // Picard-paired identities are written under every paired native
            // spelling so any consumer finds the one it reads; everything
            // else keeps its single mapped property name. A pair-member
            // exact target refreshes both spellings exactly like the
            // logical write; other exact targets stay single-name.
            auto property_names =
                (change.exact_native_name && exact_targets.size() <= 1U) || !paired_totals
                    ? std::vector<std::string>{}
                    : paired_flac_property_names(change.canonical_name);
            if (property_names.empty()) {
                property_names.push_back(mapping->property_name);
            }
            for (const auto& property_name : property_names) {
                if (!properties.replace(TagLib::String{property_name, TagLib::String::UTF8},
                                        values)) {
                    return std::unexpected(writer_error(
                        core::ErrorCode::unsupported,
                        "TagLib rejected the " + std::string{format_label} + " property mapping",
                        source_plan.raw_path, prepared_raw_path));
                }
            }
        }
    }
    const auto unsupported = file.setProperties(properties);
    if (!unsupported.isEmpty()) {
        return std::unexpected(writer_error(core::ErrorCode::unsupported,
                                            "TagLib could not map every planned " +
                                                std::string{format_label} + " property",
                                            source_plan.raw_path, prepared_raw_path));
    }
    if (!file.save()) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "TagLib failed to save the prepared " +
                                                std::string{format_label} + " copy",
                                            source_plan.raw_path, prepared_raw_path));
    }
    return {};
}

} // namespace trackknife::metadata::text_writer_detail
