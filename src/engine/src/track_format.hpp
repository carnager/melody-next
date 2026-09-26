// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// A track formatted by the engine, for a client: what playback.format and
// catalogue.find share.

#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/persistence/tkq_row.hpp"
#include "trackknife/titleformat/compiler.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>

namespace trackknife::engine {

// A client's format, compiled; one that does not compile is the client's
// mistake, and says why.
[[nodiscard]] inline core::Result<titleformat::Program>
compile_client_format(std::string source, const titleformat::FormatContextKind context) {
    auto compiled = titleformat::compile(std::move(source),
                                         {.context = context, .dialect = {}, .parse_options = {}});
    if (!compiled.isValid()) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::invalid_argument,
                        .message = !compiled.parse_diagnostics.empty()
                                       ? compiled.parse_diagnostics.front().message
                                       : compiled.diagnostics.front().message,
                        .context = {{.key = "param", .value = "format"}}});
    }
    return std::move(*compiled.program);
}

// "4:05", or "1:02:03" past the hour.
[[nodiscard]] inline std::string clock(const std::int64_t milliseconds) {
    const auto seconds = std::max<std::int64_t>(0, milliseconds) / 1'000;
    const auto hours = seconds / 3'600;
    char text[32];
    if (hours > 0) {
        std::snprintf(text, sizeof(text), "%lld:%02lld:%02lld", static_cast<long long>(hours),
                      static_cast<long long>(seconds / 60 % 60),
                      static_cast<long long>(seconds % 60));
    } else {
        std::snprintf(text, sizeof(text), "%lld:%02lld", static_cast<long long>(seconds / 60),
                      static_cast<long long>(seconds % 60));
    }
    return text;
}

// Untitled, a track is named by its file, as everywhere else.
inline void name_by_file(persistence::TkqRowFacts& facts, const std::string& raw_path) {
    if (facts.title.empty()) {
        facts.title = std::filesystem::path{raw_path}.filename().string();
    }
}

// The fields a track has besides its tags, by canonical name: its path,
// length and rating (1-10, half stars; absent when unrated).
[[nodiscard]] inline std::map<std::string, std::string>
track_fields(const persistence::TkqRowFacts& facts, const std::string& raw_path) {
    std::map<std::string, std::string> fields{
        {"path", core::display_raw_path(raw_path)},
        {"rating", facts.rating > 0 ? std::to_string(facts.rating) : std::string{}},
    };
    if (facts.duration_ms >= 0) {
        fields.emplace("length", clock(facts.duration_ms));
    }
    return fields;
}

} // namespace trackknife::engine
