// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/formats/decoder.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace trackknife::output {

// A track sent as Opus rather than as the file: for a phone on mobile data,
// or one that cannot decode the original.
struct StreamFormat final {
    int bitrate_kbps{128};

    friend bool operator==(const StreamFormat&, const StreamFormat&) = default;
};

// What a stream URL asks the engine for (ADR-0228's stream port). A part of
// a file -- a CUE track, a subsong -- is only ever sent converted, as a
// track of its own: whoever plays it then needs to know nothing of parts.
struct StreamRequest final {
    std::string raw_path;
    std::optional<StreamFormat> format;
    formats::AudioSourceSelection selection;
    std::optional<formats::SampleRange> segment;

    friend bool operator==(const StreamRequest&, const StreamRequest&) = default;
};

[[nodiscard]] std::string percent_encoded(std::string_view text);
[[nodiscard]] std::optional<std::string> percent_decoded(std::string_view text);
// The value of `name` in a query string, decoded.
[[nodiscard]] std::optional<std::string> query_value(std::string_view query, std::string_view name);

// The request as query parameters, always in the same order and spelling:
// what the agent's URL carries, and exactly what a download ticket signs.
[[nodiscard]] std::string stream_query(const StreamRequest& request);
[[nodiscard]] core::Result<StreamRequest> parse_stream_query(std::string_view query);

} // namespace trackknife::output
