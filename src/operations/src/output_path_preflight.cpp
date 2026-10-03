// SPDX-License-Identifier: GPL-3.0-only
#include "trackknife/core/posix.hpp"

#include "trackknife/operations/output_path_preflight.hpp"

#include "trackknife/core/local_sources.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace trackknife::operations {
namespace {

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
        const auto value = std::exchange(value_, -1);
        return ::close(value) == 0;
    }

    int value_{-1};
};

struct DirectoryWalk {
    Descriptor nearest_descriptor;
    std::vector<std::string> missing_paths;
    std::optional<OutputPathPreflightIssueKind> issue;
    std::string message;
};

[[nodiscard]] core::Error preflight_error(const core::ErrorCode code, std::string message) {
    return {.code = code, .message = std::move(message), .context = {}};
}

[[nodiscard]] std::string system_message(const int number) {
    return std::error_code{number, std::generic_category()}.message();
}

[[nodiscard]] bool contains_nul(const std::string_view value) {
    return value.find('\0') != std::string_view::npos;
}

[[nodiscard]] bool is_normal_absolute_path(const std::string_view raw_path) {
    if (raw_path.empty() || contains_nul(raw_path)) {
        return false;
    }
    const std::filesystem::path path{raw_path};
    return path.is_absolute() && path == path.lexically_normal();
}

[[nodiscard]] std::filesystem::path operation_root_path(std::filesystem::path path) {
    if (path != path.root_path() && path.filename().empty()) {
        path = path.parent_path();
    }
    return path;
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

[[nodiscard]] bool contained_by(const std::filesystem::path& root,
                                const std::filesystem::path& target) {
    auto root_component = root.begin();
    auto target_component = target.begin();
    for (; root_component != root.end(); ++root_component, ++target_component) {
        if (target_component == target.end() || *root_component != *target_component) {
            return false;
        }
    }
    return target_component != target.end();
}

[[nodiscard]] DirectoryWalk
walk_absolute_directory(const std::string& raw_path, const bool allow_missing,
                        const OutputPathPreflightIssueKind symlink_kind,
                        const OutputPathPreflightIssueKind missing_kind,
                        const OutputPathPreflightIssueKind not_directory_kind) {
    DirectoryWalk result;
    if (!is_normal_absolute_path(raw_path)) {
        result.issue = not_directory_kind;
        result.message = "Path is not normalized and absolute";
        return result;
    }
    result.nearest_descriptor =
        Descriptor{::open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)};
    if (!result.nearest_descriptor.valid()) {
        result.issue = OutputPathPreflightIssueKind::filesystem_observation_failed;
        result.message = "Opening the filesystem root failed: " + system_message(errno);
        return result;
    }

    const std::filesystem::path path{raw_path};
    auto current_path = path.root_path();
    std::vector<std::string> components;
    for (const auto& component : path.relative_path()) {
        components.push_back(component.native());
    }
    for (std::size_t index = 0U; index < components.size(); ++index) {
        const auto& component = components[index];
        Descriptor next{::openat(result.nearest_descriptor.get(), component.c_str(),
                                 O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)};
        if (next.valid()) {
            result.nearest_descriptor = std::move(next);
            current_path /= component;
            continue;
        }
        const auto number = errno;
        struct stat lexical{};
        const auto lexical_result = ::fstatat(result.nearest_descriptor.get(), component.c_str(),
                                              &lexical, AT_SYMLINK_NOFOLLOW);
        if (lexical_result == 0 && S_ISLNK(lexical.st_mode)) {
            result.issue = symlink_kind;
            result.message = "A path component is a symbolic link";
            return result;
        }
        if (number == ENOENT && allow_missing) {
            for (; index < components.size(); ++index) {
                current_path /= components[index];
                result.missing_paths.push_back(current_path.native());
            }
            return result;
        }
        if (number == ENOENT) {
            result.issue = missing_kind;
            result.message = "Required directory does not exist";
        } else if (number == ENOTDIR || lexical_result == 0) {
            result.issue = not_directory_kind;
            result.message = "A path component is not a directory";
        } else {
            result.issue = OutputPathPreflightIssueKind::filesystem_observation_failed;
            result.message = "Opening a directory component failed: " + system_message(number);
        }
        return result;
    }
    return result;
}

[[nodiscard]] bool directory_is_writable(const Descriptor& descriptor) {
    return ::faccessat(descriptor.get(), ".", W_OK | X_OK, 0) == 0;
}

[[nodiscard]] std::optional<std::uint64_t> directory_device(const Descriptor& descriptor) {
    struct stat status{};
    if (::fstat(descriptor.get(), &status) != 0 || !S_ISDIR(status.st_mode)) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(status.st_dev);
}

[[nodiscard]] std::optional<long> path_limit(const Descriptor& descriptor, const int name) {
    errno = 0;
    const auto value = ::fpathconf(descriptor.get(), name);
    return value >= 0 ? std::optional{value} : std::nullopt;
}

[[nodiscard]] bool has_case_only_warning(const OutputPathPlan& plan,
                                         const PlannedOutputPathSource& source) {
    return std::ranges::any_of(plan.issues, [&](const auto& issue) {
        return issue.kind == OutputPathPlanIssueKind::case_only_change && !issue.blocking &&
               issue.source_raw_path == source.source_raw_path &&
               issue.target_raw_path == source.target_raw_path;
    });
}

} // namespace

std::string_view output_path_preflight_issue_kind_name(const OutputPathPreflightIssueKind kind) {
    switch (kind) {
    case OutputPathPreflightIssueKind::source_missing:
        return "source missing";
    case OutputPathPreflightIssueKind::source_symlink:
        return "source symlink";
    case OutputPathPreflightIssueKind::source_not_regular:
        return "source not regular";
    case OutputPathPreflightIssueKind::source_changed:
        return "source changed";
    case OutputPathPreflightIssueKind::source_hard_linked:
        return "source hard linked";
    case OutputPathPreflightIssueKind::source_parent_not_writable:
        return "source parent not writable";
    case OutputPathPreflightIssueKind::operation_root_missing:
        return "operation root missing";
    case OutputPathPreflightIssueKind::operation_root_symlink:
        return "operation root symlink";
    case OutputPathPreflightIssueKind::operation_root_not_directory:
        return "operation root not directory";
    case OutputPathPreflightIssueKind::target_parent_symlink:
        return "target parent symlink";
    case OutputPathPreflightIssueKind::target_parent_not_directory:
        return "target parent not directory";
    case OutputPathPreflightIssueKind::target_parent_not_writable:
        return "target parent not writable";
    case OutputPathPreflightIssueKind::target_exists:
        return "target exists";
    case OutputPathPreflightIssueKind::component_too_long:
        return "component too long";
    case OutputPathPreflightIssueKind::path_too_long:
        return "path too long";
    case OutputPathPreflightIssueKind::filesystem_observation_failed:
        return "filesystem observation failed";
    }
    return "output path preflight issue";
}

bool OutputPathPreflight::ready() const noexcept {
    return plan.ready() && !sources.empty() &&
           std::ranges::none_of(issues, &OutputPathPreflightIssue::blocking);
}

core::Result<OutputPathPreflight>
preflight_output_paths(const OutputPathPlan& plan, const core::CancellationToken& cancellation) {
    if (!plan.ready()) {
        return std::unexpected(preflight_error(core::ErrorCode::invalid_argument,
                                               "filesystem preflight requires a ready path plan"));
    }
    OutputPathPreflight result{.plan = plan, .sources = {}, .issues = {}};
    result.sources.reserve(plan.sources.size());
    const auto add_issue = [&](const PlannedOutputPathSource& source,
                               const OutputPathPreflightIssueKind kind, std::string message) {
        result.issues.push_back(OutputPathPreflightIssue{
            .kind = kind,
            .blocking = true,
            .message = std::move(message),
            .item_indexes = source.item_indexes,
            .source_raw_path = source.source_raw_path,
            .target_raw_path = source.target_raw_path,
        });
    };

    for (const auto& planned : plan.sources) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(
                preflight_error(core::ErrorCode::cancelled, "filesystem preflight was cancelled"));
        }
        OutputPathPreflightSource source{.planned = planned,
                                         .observed_revision = {},
                                         .publication = OutputPathPublicationKind::no_change,
                                         .target_filesystem_device = 0U,
                                         .missing_directory_raw_paths = {}};
        if (!is_normal_absolute_path(planned.source_raw_path) ||
            !is_normal_absolute_path(planned.target_raw_path)) {
            add_issue(planned, OutputPathPreflightIssueKind::filesystem_observation_failed,
                      "Planned source or target path is no longer normalized and absolute");
            result.sources.push_back(std::move(source));
            continue;
        }

        const std::filesystem::path source_path{planned.source_raw_path};
        auto source_parent = walk_absolute_directory(
            source_path.parent_path().native(), false, OutputPathPreflightIssueKind::source_symlink,
            OutputPathPreflightIssueKind::source_missing,
            OutputPathPreflightIssueKind::source_not_regular);
        if (source_parent.issue) {
            add_issue(planned, *source_parent.issue, std::move(source_parent.message));
            result.sources.push_back(std::move(source));
            continue;
        }
        Descriptor source_descriptor{::openat(source_parent.nearest_descriptor.get(),
                                              source_path.filename().c_str(),
                                              O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
        if (!source_descriptor.valid()) {
            const auto number = errno;
            add_issue(planned,
                      number == ELOOP ? OutputPathPreflightIssueKind::source_symlink
                      : number == ENOENT
                          ? OutputPathPreflightIssueKind::source_missing
                          : OutputPathPreflightIssueKind::filesystem_observation_failed,
                      "Opening the source failed: " + system_message(number));
            result.sources.push_back(std::move(source));
            continue;
        }
        struct stat source_status{};
        if (::fstat(source_descriptor.get(), &source_status) != 0) {
            add_issue(planned, OutputPathPreflightIssueKind::filesystem_observation_failed,
                      "Observing the source failed: " + system_message(errno));
            result.sources.push_back(std::move(source));
            continue;
        }
        if (!S_ISREG(source_status.st_mode) || source_status.st_size < 0) {
            add_issue(planned, OutputPathPreflightIssueKind::source_not_regular,
                      "Source is not a regular file");
            result.sources.push_back(std::move(source));
            continue;
        }
        source.observed_revision = revision_from_stat(source_status);
        if (source.observed_revision != planned.source_revision) {
            add_issue(planned, OutputPathPreflightIssueKind::source_changed,
                      "Source revision changed after path planning");
        }
        if (source_status.st_nlink != 1) {
            add_issue(planned, OutputPathPreflightIssueKind::source_hard_linked,
                      "Source does not have exactly one hard link");
        }

        const auto operation_root =
            plan.operations.move_files
                ? operation_root_path(std::filesystem::path{plan.destination->root_raw_path})
                : source_path.parent_path();
        if (!contained_by(operation_root, std::filesystem::path{planned.target_raw_path})) {
            add_issue(planned, OutputPathPreflightIssueKind::filesystem_observation_failed,
                      "Target is no longer lexically beneath its operation root");
            result.sources.push_back(std::move(source));
            continue;
        }
        auto root = walk_absolute_directory(
            operation_root.native(), false, OutputPathPreflightIssueKind::operation_root_symlink,
            OutputPathPreflightIssueKind::operation_root_missing,
            OutputPathPreflightIssueKind::operation_root_not_directory);
        if (root.issue) {
            add_issue(planned, *root.issue, std::move(root.message));
            result.sources.push_back(std::move(source));
            continue;
        }

        const std::filesystem::path target_path{planned.target_raw_path};
        auto target_parent =
            walk_absolute_directory(target_path.parent_path().native(), true,
                                    OutputPathPreflightIssueKind::target_parent_symlink,
                                    OutputPathPreflightIssueKind::operation_root_missing,
                                    OutputPathPreflightIssueKind::target_parent_not_directory);
        if (target_parent.issue) {
            add_issue(planned, *target_parent.issue, std::move(target_parent.message));
            result.sources.push_back(std::move(source));
            continue;
        }
        source.missing_directory_raw_paths = target_parent.missing_paths;
        const auto target_device = directory_device(target_parent.nearest_descriptor);
        if (!target_device) {
            add_issue(planned, OutputPathPreflightIssueKind::filesystem_observation_failed,
                      "Could not observe the target filesystem");
            result.sources.push_back(std::move(source));
            continue;
        }
        source.target_filesystem_device = *target_device;

        bool target_is_source = false;
        if (target_parent.missing_paths.empty()) {
            struct stat target_status{};
            if (::fstatat(target_parent.nearest_descriptor.get(), target_path.filename().c_str(),
                          &target_status, AT_SYMLINK_NOFOLLOW) == 0) {
                target_is_source = S_ISREG(target_status.st_mode) &&
                                   static_cast<std::uint64_t>(target_status.st_dev) ==
                                       source.observed_revision.device &&
                                   static_cast<std::uint64_t>(target_status.st_ino) ==
                                       source.observed_revision.inode &&
                                   (planned.no_change || has_case_only_warning(plan, planned));
                if (!target_is_source) {
                    add_issue(planned, OutputPathPreflightIssueKind::target_exists,
                              "Target appeared after path planning");
                }
            } else if (errno != ENOENT) {
                add_issue(planned, OutputPathPreflightIssueKind::filesystem_observation_failed,
                          "Observing the target failed: " + system_message(errno));
            }
        }

        if (!planned.no_change && !directory_is_writable(source_parent.nearest_descriptor)) {
            add_issue(planned, OutputPathPreflightIssueKind::source_parent_not_writable,
                      "Source parent is not writable and searchable");
        }
        if (!planned.no_change && !directory_is_writable(target_parent.nearest_descriptor)) {
            add_issue(planned, OutputPathPreflightIssueKind::target_parent_not_writable,
                      "Nearest existing target parent is not writable and searchable");
        }

        if (const auto name_max = path_limit(target_parent.nearest_descriptor, _PC_NAME_MAX)) {
            if (target_path.filename().native().size() > static_cast<std::size_t>(*name_max) ||
                std::ranges::any_of(target_parent.missing_paths, [&](const auto& missing) {
                    return std::filesystem::path{missing}.filename().native().size() >
                           static_cast<std::size_t>(*name_max);
                })) {
                add_issue(planned, OutputPathPreflightIssueKind::component_too_long,
                          "Target exceeds the observed filesystem component limit");
            }
        }
        if (const auto path_max = path_limit(target_parent.nearest_descriptor, _PC_PATH_MAX);
            path_max && planned.target_raw_path.size() > static_cast<std::size_t>(*path_max)) {
            add_issue(planned, OutputPathPreflightIssueKind::path_too_long,
                      "Target exceeds the observed filesystem path limit");
        }

        if (planned.no_change) {
            source.publication = OutputPathPublicationKind::no_change;
        } else if (source.observed_revision.device == *target_device) {
            source.publication = OutputPathPublicationKind::same_filesystem_rename;
        } else {
            source.publication = OutputPathPublicationKind::cross_filesystem_copy;
        }
        static_cast<void>(target_is_source);
        result.sources.push_back(std::move(source));
    }
    return result;
}

} // namespace trackknife::operations
