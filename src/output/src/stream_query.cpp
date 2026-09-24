// SPDX-License-Identifier: GPL-3.0-only
#include "trackknife/output/stream_query.hpp"

#include "trackknife/protocol/message.hpp"

#include <charconv>

namespace trackknife::output {
namespace {

[[nodiscard]] int hex_value(const char digit) {
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f') {
        return digit - 'a' + 10;
    }
    if (digit >= 'A' && digit <= 'F') {
        return digit - 'A' + 10;
    }
    return -1;
}

template <typename T> [[nodiscard]] std::optional<T> number(const std::optional<std::string>& text) {
    if (!text) {
        return std::nullopt;
    }
    T value{};
    const auto* end = text->data() + text->size();
    const auto [stopped, error] = std::from_chars(text->data(), end, value);
    if (error != std::errc{} || stopped != end) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] core::Error malformed(std::string what) {
    return core::Error{.code = core::ErrorCode::invalid_argument,
                       .message = "a stream request with " + std::move(what),
                       .context = {}};
}

} // namespace

std::string percent_encoded(const std::string_view text) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string encoded;
    for (const auto character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' ||
            byte == '~') {
            encoded.push_back(character);
        } else {
            encoded.push_back('%');
            encoded.push_back(digits[byte >> 4U]);
            encoded.push_back(digits[byte & 0x0FU]);
        }
    }
    return encoded;
}

std::optional<std::string> percent_decoded(const std::string_view text) {
    std::string decoded;
    decoded.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        // '+' stays '+': this is a URL, not a form, and base64 is full of them.
        if (text[index] == '%') {
            if (index + 2U >= text.size()) {
                return std::nullopt;
            }
            const auto high = hex_value(text[index + 1U]);
            const auto low = hex_value(text[index + 2U]);
            if (high < 0 || low < 0) {
                return std::nullopt;
            }
            decoded.push_back(static_cast<char>((high << 4) | low));
            index += 2U;
        } else {
            decoded.push_back(text[index]);
        }
    }
    return decoded;
}

std::optional<std::string> query_value(std::string_view query, const std::string_view name) {
    while (!query.empty()) {
        const auto end = query.find('&');
        const auto pair = query.substr(0, end);
        if (const auto equals = pair.find('='); equals != std::string_view::npos) {
            if (pair.substr(0, equals) == name) {
                return percent_decoded(pair.substr(equals + 1U));
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        query.remove_prefix(end + 1U);
    }
    return std::nullopt;
}

std::string stream_query(const StreamRequest& request) {
    std::string query = "path=" + percent_encoded(protocol::encode_raw_path(request.raw_path));
    if (request.format) {
        query += "&format=opus&bitrate=" + std::to_string(request.format->bitrate_kbps);
    }
    if (request.selection.stream_index) {
        query += "&stream=" + std::to_string(*request.selection.stream_index);
    }
    if (request.selection.subsong_index) {
        query += "&subsong=" + std::to_string(*request.selection.subsong_index);
    }
    if (request.segment) {
        query += "&start=" + std::to_string(request.segment->start_sample);
        if (request.segment->end_sample) {
            query += "&end=" + std::to_string(*request.segment->end_sample);
        }
    }
    return query;
}

core::Result<StreamRequest> parse_stream_query(const std::string_view query) {
    StreamRequest request;
    const auto encoded = query_value(query, "path");
    if (!encoded) {
        return std::unexpected(malformed("no path"));
    }
    auto raw_path = protocol::decode_raw_path(*encoded);
    if (!raw_path) {
        return std::unexpected(malformed("a path that is not encoded"));
    }
    request.raw_path = std::move(*raw_path);
    if (const auto format = query_value(query, "format")) {
        if (*format != "opus") {
            return std::unexpected(malformed("a format other than opus"));
        }
        const auto bitrate = number<int>(query_value(query, "bitrate"));
        if (!bitrate || *bitrate < 16 || *bitrate > 512) {
            return std::unexpected(malformed("no bit rate between 16 and 512 kbps"));
        }
        request.format = StreamFormat{.bitrate_kbps = *bitrate};
    }
    request.selection.stream_index = number<int>(query_value(query, "stream"));
    request.selection.subsong_index = number<int>(query_value(query, "subsong"));
    if (const auto start = number<std::int64_t>(query_value(query, "start"))) {
        request.segment = formats::SampleRange{
            .start_sample = *start, .end_sample = number<std::int64_t>(query_value(query, "end"))};
    }
    if (!request.format && (request.segment || request.selection.stream_index ||
                            request.selection.subsong_index)) {
        // The original file is sent whole; its parts only as tracks of
        // their own, which is to say converted.
        return std::unexpected(malformed("a part of a file but no format to send it in"));
    }
    return request;
}

} // namespace trackknife::output
