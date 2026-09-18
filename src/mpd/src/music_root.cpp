// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/mpd/music_root.hpp"

#include "trackknife/core/unicode.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace trackknife::mpd {
namespace {

[[nodiscard]] core::Error invalid_uri(std::string message) {
    return core::Error{
        .code = core::ErrorCode::invalid_argument, .message = std::move(message), .context = {}};
}

[[nodiscard]] bool has_uri_scheme(std::string_view uri) noexcept {
    const auto colon = uri.find(':');
    const auto slash = uri.find('/');
    if (colon == std::string_view::npos || (slash != std::string_view::npos && colon > slash) ||
        colon == 0U) {
        return false;
    }
    const auto first = static_cast<unsigned char>(uri.front());
    if (std::isalpha(first) == 0) {
        return false;
    }
    return std::ranges::all_of(uri.substr(1, colon - 1U), [](const char value) {
        const auto byte = static_cast<unsigned char>(value);
        return std::isalnum(byte) != 0 || value == '+' || value == '-' || value == '.';
    });
}

[[nodiscard]] bool path_has_prefix(const std::filesystem::path& candidate,
                                   const std::filesystem::path& root) {
    auto candidate_part = candidate.begin();
    for (auto root_part = root.begin(); root_part != root.end(); ++root_part, ++candidate_part) {
        if (candidate_part == candidate.end() || *candidate_part != *root_part) {
            return false;
        }
    }
    return true;
}

} // namespace

core::Result<std::filesystem::path>
resolve_below_music_root(const std::filesystem::path& music_root, std::string_view mpd_uri) {
    if (!music_root.is_absolute()) {
        return std::unexpected(invalid_uri("the configured music root must be absolute"));
    }
    if (mpd_uri.empty()) {
        return std::unexpected(invalid_uri("an empty MPD URI cannot resolve to a local file"));
    }
    if (mpd_uri.find('\0') != std::string_view::npos) {
        return std::unexpected(invalid_uri("the MPD URI contains a NUL byte"));
    }
    if (mpd_uri.front() == '/' || has_uri_scheme(mpd_uri)) {
        return std::unexpected(
            invalid_uri("only relative MPD file URIs can use a local music root"));
    }
    if (!core::unicodeCodePointCount(mpd_uri)) {
        return std::unexpected(invalid_uri("the MPD URI is not valid UTF-8"));
    }

    std::filesystem::path relative;
    std::size_t offset = 0U;
    while (offset <= mpd_uri.size()) {
        const auto separator = mpd_uri.find('/', offset);
        const auto end = separator == std::string_view::npos ? mpd_uri.size() : separator;
        const auto component = mpd_uri.substr(offset, end - offset);
        if (component.empty() || component == "." || component == "..") {
            return std::unexpected(
                invalid_uri("the MPD URI contains an empty or traversal path component"));
        }
        relative /= std::filesystem::path{std::string{component}};
        if (separator == std::string_view::npos) {
            break;
        }
        offset = separator + 1U;
    }

    // A trailing separator normalizes to an empty final component, which
    // would fail the component-wise prefix check against real filenames —
    // "/mnt/music/" and "/mnt/music" name the same root.
    auto normalized_root = music_root.lexically_normal();
    if (!normalized_root.has_filename() && normalized_root.has_relative_path()) {
        normalized_root = normalized_root.parent_path();
    }
    const auto candidate = (normalized_root / relative).lexically_normal();
    if (!path_has_prefix(candidate, normalized_root) || candidate == normalized_root) {
        return std::unexpected(invalid_uri("the resolved path escapes the configured music root"));
    }
    return candidate;
}

} // namespace trackknife::mpd
