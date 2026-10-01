// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/local_sources.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace trackknife::core {
namespace {

[[nodiscard]] bool raw_less(const std::string& left, const std::string& right) {
    return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
                                        [](const char first, const char second) {
                                            return static_cast<unsigned char>(first) <
                                                   static_cast<unsigned char>(second);
                                        });
}

[[nodiscard]] LocalSourceIssue issue(std::string raw_path, const ErrorCode code,
                                     std::string message) {
    return LocalSourceIssue{
        .raw_path = std::move(raw_path),
        .error = Error{.code = code, .message = std::move(message), .context = {}},
    };
}

[[nodiscard]] std::string native_bytes(const std::filesystem::path& path) { return path.native(); }

[[nodiscard]] bool matches_extension(const std::string& path_bytes,
                                     const std::span<const std::string_view> extensions) {
    const auto dot = path_bytes.find_last_of('.');
    const auto slash = path_bytes.find_last_of('/');
    if (dot == std::string::npos || dot + 1U >= path_bytes.size() ||
        (slash != std::string::npos && dot < slash)) {
        return false;
    }
    const std::string_view tail{path_bytes.data() + dot + 1U, path_bytes.size() - dot - 1U};
    return std::ranges::any_of(extensions, [tail](const std::string_view extension) {
        return std::ranges::equal(tail, extension, [](const char left, const char right) {
            const auto lowered =
                left >= 'A' && left <= 'Z' ? static_cast<char>(left - 'A' + 'a') : left;
            return lowered == right;
        });
    });
}

} // namespace

LocalSourceDiscovery
discover_local_sources(const std::span<const std::string> raw_paths,
                       const CancellationToken& cancellation, const std::size_t file_limit,
                       const std::span<const std::string_view> directory_file_extensions) {
    LocalSourceDiscovery result;
    if (file_limit == 0U) {
        result.truncated = true;
        result.issues.push_back(
            issue({}, ErrorCode::limit_exceeded, "local source discovery file limit is zero"));
        return result;
    }

    for (const auto& raw_path : raw_paths) {
        if (cancellation.is_cancellation_requested()) {
            result.cancelled = true;
            break;
        }
        if (raw_path.empty()) {
            result.issues.push_back(
                issue(raw_path, ErrorCode::invalid_argument, "local source path is empty"));
            continue;
        }

        struct PendingEntry {
            std::filesystem::path path;
            bool from_directory{false};
        };
        std::vector<PendingEntry> pending{{std::filesystem::path{raw_path}, false}};
        while (!pending.empty()) {
            if (cancellation.is_cancellation_requested()) {
                result.cancelled = true;
                break;
            }
            auto [path, from_directory] = std::move(pending.back());
            pending.pop_back();
            const auto path_bytes = native_bytes(path);
            std::error_code error;
            const auto lexical_status = std::filesystem::symlink_status(path, error);
            ++result.visited_entries;
            if (error) {
                result.issues.push_back(issue(path_bytes, ErrorCode::io, error.message()));
                continue;
            }

            auto status = lexical_status;
            if (std::filesystem::is_symlink(lexical_status)) {
                status = std::filesystem::status(path, error);
                if (error) {
                    result.issues.push_back(issue(path_bytes, ErrorCode::io, error.message()));
                    continue;
                }
                if (std::filesystem::is_directory(status)) {
                    result.issues.push_back(issue(path_bytes, ErrorCode::unsupported,
                                                  "directory symlink is not followed"));
                    continue;
                }
            }

            if (std::filesystem::is_regular_file(status)) {
                // Explicitly listed files always pass; expansion respects the
                // caller's extension allowlist so folder opens skip artwork,
                // cue sheets, and other non-audio residents silently.
                if (from_directory && !directory_file_extensions.empty() &&
                    !matches_extension(path_bytes, directory_file_extensions)) {
                    continue;
                }
                if (result.raw_files.size() == file_limit) {
                    result.truncated = true;
                    result.issues.push_back(issue(raw_path, ErrorCode::limit_exceeded,
                                                  "local source discovery reached its file limit"));
                    break;
                }
                result.raw_files.push_back(path_bytes);
                continue;
            }
            if (!std::filesystem::is_directory(status)) {
                result.issues.push_back(issue(path_bytes, ErrorCode::unsupported,
                                              "path is not a regular file or directory"));
                continue;
            }

            std::vector<std::filesystem::path> children;
            std::filesystem::directory_iterator iterator{
                path, std::filesystem::directory_options::skip_permission_denied, error};
            const std::filesystem::directory_iterator end;
            if (error) {
                result.issues.push_back(issue(path_bytes, ErrorCode::io, error.message()));
                continue;
            }
            while (iterator != end) {
                children.push_back(iterator->path());
                iterator.increment(error);
                if (error) {
                    result.issues.push_back(issue(path_bytes, ErrorCode::io, error.message()));
                    error.clear();
                }
            }
            std::ranges::sort(children, [](const auto& left, const auto& right) {
                return raw_less(native_bytes(left), native_bytes(right));
            });
            pending.reserve(pending.size() + children.size());
            for (auto it = children.rbegin(); it != children.rend(); ++it) {
                pending.push_back({std::move(*it), true});
            }
        }
        if (result.cancelled) {
            break;
        }
        if (result.truncated) {
            break;
        }
    }
    return result;
}

std::string escape_raw_path(const std::string_view raw_path) {
    constexpr std::string_view digits{"0123456789ABCDEF"};
    std::string escaped;
    escaped.reserve(raw_path.size());
    for (const auto byte : raw_path) {
        const auto value = static_cast<unsigned char>(byte);
        if (value >= 0x20U && value <= 0x7EU && value != static_cast<unsigned char>('\\')) {
            escaped.push_back(static_cast<char>(value));
        } else if (value == static_cast<unsigned char>('\\')) {
            escaped += "\\\\";
        } else {
            escaped += "\\x";
            escaped.push_back(digits.at(value >> 4U));
            escaped.push_back(digits.at(value & 0x0FU));
        }
    }
    return escaped;
}

namespace {

// The length of the valid UTF-8 sequence starting at `offset`, and its code
// point; zero when the bytes there are not one.
[[nodiscard]] std::pair<std::size_t, char32_t> utf8_sequence(const std::string_view text,
                                                             const std::size_t offset) {
    const auto lead = static_cast<unsigned char>(text[offset]);
    std::size_t length = 0U;
    char32_t code = 0U;
    if (lead >= 0xC2U && lead <= 0xDFU) {
        length = 2U;
        code = lead & 0x1FU;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
        length = 3U;
        code = lead & 0x0FU;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
        length = 4U;
        code = lead & 0x07U;
    } else {
        return {0U, 0U};
    }
    if (offset + length > text.size()) {
        return {0U, 0U};
    }
    for (std::size_t index = 1U; index < length; ++index) {
        const auto next = static_cast<unsigned char>(text[offset + index]);
        if ((next & 0xC0U) != 0x80U) {
            return {0U, 0U};
        }
        code = (code << 6U) | (next & 0x3FU);
    }
    // Overlong forms, surrogates and values past Unicode are not UTF-8.
    const bool overlong = (length == 3U && code < 0x800U) || (length == 4U && code < 0x10000U);
    if (overlong || (code >= 0xD800U && code <= 0xDFFFU) || code > 0x10FFFFU) {
        return {0U, 0U};
    }
    return {length, code};
}

[[nodiscard]] bool hides_itself(const char32_t code) {
    return (code >= 0x80U && code <= 0x9FU) || code == 0xADU ||
           (code >= 0x200BU && code <= 0x200FU) || code == 0x2028U || code == 0x2029U ||
           (code >= 0x202AU && code <= 0x202EU) || (code >= 0x2060U && code <= 0x2064U) ||
           (code >= 0x2066U && code <= 0x206FU) || code == 0xFEFFU;
}

} // namespace

std::string display_raw_path(const std::string_view raw_path) {
    std::string shown;
    shown.reserve(raw_path.size());
    for (std::size_t offset = 0U; offset < raw_path.size();) {
        const auto [length, code] = utf8_sequence(raw_path, offset);
        if (length > 0U && !hides_itself(code)) {
            shown.append(raw_path.substr(offset, length));
            offset += length;
        } else {
            shown += escape_raw_path(raw_path.substr(offset, 1U));
            ++offset;
        }
    }
    return shown;
}

void settle_written_file_time(const std::string& raw_path) noexcept {
    struct timespec now{};
    ::clock_gettime(CLOCK_REALTIME, &now);
    const struct timespec settled{.tv_sec = now.tv_sec - (now.tv_sec % 2), .tv_nsec = 0};
    const std::array times{settled, settled};
    // Refused, the file keeps its own time: the mounts that refuse are not
    // the ones whose time flickers.
    static_cast<void>(::utimensat(AT_FDCWD, raw_path.c_str(), times.data(), AT_SYMLINK_NOFOLLOW));
}

namespace {

std::atomic<bool> renumbering_simulated{false};

// Renames a fresh temporary file in `directory` and says whether it kept its
// inode number; empty when that could not be found out.
std::optional<bool> probe_rename_keeps_inode(const std::filesystem::path& directory) {
    std::random_device random;
    const auto token = std::to_string(random()) + std::to_string(random());
    const auto first = directory / (".trackknife-" + token + ".rename-probe");
    const auto second = directory / (".trackknife-" + token + ".rename-probe-renamed");
    const int descriptor =
        ::open(first.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0) {
        return std::nullopt;
    }
    const bool written = ::write(descriptor, "t", 1) == 1;
    static_cast<void>(::close(descriptor));
    struct stat before{};
    struct stat after{};
    std::optional<bool> kept;
    if (written && ::lstat(first.c_str(), &before) == 0 &&
        ::rename(first.c_str(), second.c_str()) == 0) {
        if (::lstat(second.c_str(), &after) == 0) {
            kept = before.st_ino == after.st_ino;
        }
        static_cast<void>(::unlink(second.c_str()));
    } else {
        static_cast<void>(::unlink(first.c_str()));
    }
    return kept;
}

bool renames_keep_inodes(const std::string& raw_path, const std::uint64_t device) {
    if (renumbering_simulated.load(std::memory_order_relaxed)) {
        return false;
    }
    static std::mutex mutex;
    static std::map<std::uint64_t, bool> known;
    {
        const std::scoped_lock lock{mutex};
        if (const auto found = known.find(device); found != known.end()) {
            return found->second;
        }
    }
    auto parent = std::filesystem::path{raw_path}.parent_path();
    if (parent.empty()) {
        parent = ".";
    }
    const auto kept = probe_rename_keeps_inode(parent);
    if (!kept) {
        return true;
    }
    const std::scoped_lock lock{mutex};
    known.insert_or_assign(device, *kept);
    return *kept;
}

} // namespace

bool same_file_after_rename(const std::string& raw_path, const LocalSourceRevision& before,
                            const LocalSourceRevision& after) {
    if (before == after) {
        return true;
    }
    auto renumbered = before;
    renumbered.inode = after.inode;
    return renumbered == after && !renames_keep_inodes(raw_path, after.device);
}

void simulate_renumbering_renames_for_testing(const bool enabled) noexcept {
    renumbering_simulated.store(enabled, std::memory_order_relaxed);
}

std::string describe_revision_change(const LocalSourceRevision& before,
                                     const LocalSourceRevision& after) {
    std::string described;
    const auto add = [&described](const std::string_view field, const std::string& was,
                                  const std::string& now) {
        if (!described.empty()) {
            described += "; ";
        }
        described += std::string{field} + ' ' + was + " -> " + now;
    };
    const auto time = [](const std::int64_t seconds, const std::int64_t nanoseconds) {
        auto fraction = std::to_string(nanoseconds);
        fraction.insert(0U, fraction.size() < 9U ? 9U - fraction.size() : 0U, '0');
        return std::to_string(seconds) + '.' + fraction;
    };
    if (before.device != after.device) {
        add("device", std::to_string(before.device), std::to_string(after.device));
    }
    if (before.inode != after.inode) {
        add("inode", std::to_string(before.inode), std::to_string(after.inode));
    }
    if (before.size != after.size) {
        add("size", std::to_string(before.size), std::to_string(after.size));
    }
    if (before.modification_time_seconds != after.modification_time_seconds ||
        before.modification_time_nanoseconds != after.modification_time_nanoseconds) {
        add("modification time",
            time(before.modification_time_seconds, before.modification_time_nanoseconds),
            time(after.modification_time_seconds, after.modification_time_nanoseconds));
    }
    return described;
}

Result<LocalSourceRevision> observe_local_source_revision(const std::string& raw_path) {
    const auto failure = [&raw_path](const ErrorCode code, std::string message) {
        return std::unexpected(Error{
            .code = code,
            .message = std::move(message),
            .context = {{.key = "path", .value = escape_raw_path(raw_path)}},
        });
    };
    if (raw_path.empty()) {
        return failure(ErrorCode::invalid_argument, "local source path is empty");
    }
    if (raw_path.find('\0') != std::string::npos) {
        return failure(ErrorCode::invalid_argument, "local source path contains a NUL byte");
    }

    struct stat observed{};
    if (::stat(raw_path.c_str(), &observed) != 0) {
        const auto observed_error = errno;
        const auto code = observed_error == ENOENT || observed_error == ENOTDIR
                              ? ErrorCode::not_found
                              : ErrorCode::io;
        return failure(code,
                       "local source could not be observed: " +
                           std::error_code{observed_error, std::generic_category()}.message());
    }
    if (!S_ISREG(observed.st_mode)) {
        return failure(ErrorCode::unsupported, "local source is not a regular file");
    }
    if (observed.st_size < 0) {
        return failure(ErrorCode::io, "local source reported a negative size");
    }
    return LocalSourceRevision{
        .device = static_cast<std::uint64_t>(observed.st_dev),
        .inode = static_cast<std::uint64_t>(observed.st_ino),
        .size = static_cast<std::uint64_t>(observed.st_size),
        .modification_time_seconds = static_cast<std::int64_t>(observed.st_mtim.tv_sec),
        .modification_time_nanoseconds = static_cast<std::int64_t>(observed.st_mtim.tv_nsec),
    };
}

Result<ContainedLocalSource> revalidate_contained_source(const std::string& raw_root,
                                                         const std::string& raw_path) {
    const auto reject = [&raw_root, &raw_path](const ErrorCode code, std::string message,
                                               std::string resolved = {}) {
        Error error{.code = code, .message = std::move(message), .context = {}};
        error.context.push_back({.key = "root", .value = escape_raw_path(raw_root)});
        error.context.push_back({.key = "path", .value = escape_raw_path(raw_path)});
        if (!resolved.empty()) {
            error.context.push_back({.key = "resolved", .value = escape_raw_path(resolved)});
        }
        return std::unexpected(std::move(error));
    };
    if (raw_root.empty()) {
        return reject(ErrorCode::invalid_argument, "local music root is empty");
    }
    if (raw_path.empty()) {
        return reject(ErrorCode::invalid_argument, "local source path is empty");
    }

    std::error_code error;
    const auto resolved_root = std::filesystem::canonical(std::filesystem::path{raw_root}, error);
    if (error) {
        return reject(error == std::errc::no_such_file_or_directory ? ErrorCode::not_found
                                                                    : ErrorCode::io,
                      "local music root could not be resolved: " + error.message());
    }
    if (!std::filesystem::is_directory(resolved_root, error) || error) {
        return reject(ErrorCode::invalid_argument, "local music root is not a directory",
                      resolved_root.native());
    }

    const auto resolved = std::filesystem::canonical(std::filesystem::path{raw_path}, error);
    if (error) {
        return reject(error == std::errc::no_such_file_or_directory ? ErrorCode::not_found
                                                                    : ErrorCode::io,
                      "local source could not be resolved: " + error.message());
    }
    if (!std::filesystem::is_regular_file(resolved, error) || error) {
        return reject(ErrorCode::unsupported, "local source is not a regular file",
                      resolved.native());
    }

    const auto& root_bytes = resolved_root.native();
    const auto& path_bytes = resolved.native();
    const bool root_has_separator = !root_bytes.empty() && root_bytes.back() == '/';
    const bool contained = path_bytes.size() > root_bytes.size() + (root_has_separator ? 0U : 1U) &&
                           path_bytes.starts_with(root_bytes) &&
                           (root_has_separator || path_bytes[root_bytes.size()] == '/');
    if (!contained) {
        return reject(ErrorCode::invalid_argument,
                      "local source resolves outside the configured music root", resolved.native());
    }

    return ContainedLocalSource{
        .raw_root = raw_root,
        .raw_path = raw_path,
        .resolved_path = path_bytes,
    };
}

bool is_audio_path(const std::string_view raw_path) {
    static constexpr std::array<std::string_view, 27> extensions{
        ".flac", ".mp3",  ".ogg", ".oga",  ".opus", ".m4a",  ".mp4", ".aac", ".wv",
        ".wav",  ".rf64", ".w64", ".aiff", ".aif",  ".aifc", ".ape", ".mpc", ".tta",
        ".spx",  ".mka",  ".wma", ".dsf",  ".dff",  ".mod",  ".xm",  ".s3m", ".it"};
    const auto slash = raw_path.rfind('/');
    const auto name = slash == std::string_view::npos ? raw_path : raw_path.substr(slash + 1U);
    const auto dot = name.rfind('.');
    // A leading dot names a hidden file, not an extension.
    if (dot == std::string_view::npos || dot == 0U || name.size() - dot > 5U) {
        return false;
    }
    std::string extension{name.substr(dot)};
    std::ranges::transform(extension, extension.begin(), [](const char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return std::ranges::find(extensions, extension) != extensions.end();
}

} // namespace trackknife::core
