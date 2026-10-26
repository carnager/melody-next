// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trackknife::metadata::ogg_stream_detail {

// The complete logical packets of one single-stream Ogg file, in order.
// Rewriting a Vorbis comment legitimately relayouts pages — lacing, page
// sequence numbers, and CRCs all change — so preservation is proven at the
// packet layer: every packet except the comment packet must be
// byte-identical between the source and the prepared copy (ADR-0114).
struct OggPacketStream {
    std::uint32_t serial_number{0U};
    std::vector<std::vector<unsigned char>> packets;
};

// Parses the packet stream, failing typed on anything the qualification
// cannot vouch for: a missing capture pattern, an unknown page version, a
// second bitstream serial (chained or multiplexed Ogg), or a truncated
// final packet.
[[nodiscard]] inline core::Result<OggPacketStream>
parse_packet_stream(const std::span<const unsigned char> bytes, const core::Error& malformed) {
    constexpr std::size_t page_header_size = 27U;
    OggPacketStream stream;
    std::vector<unsigned char> current;
    bool current_open = false;
    bool first_page = true;
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < page_header_size || bytes[offset] != 'O' ||
            bytes[offset + 1U] != 'g' || bytes[offset + 2U] != 'g' || bytes[offset + 3U] != 'S' ||
            bytes[offset + 4U] != 0U) {
            return std::unexpected(malformed);
        }
        const auto header_type = bytes[offset + 5U];
        const auto serial = static_cast<std::uint32_t>(bytes[offset + 14U]) |
                            (static_cast<std::uint32_t>(bytes[offset + 15U]) << 8U) |
                            (static_cast<std::uint32_t>(bytes[offset + 16U]) << 16U) |
                            (static_cast<std::uint32_t>(bytes[offset + 17U]) << 24U);
        if (first_page) {
            stream.serial_number = serial;
            first_page = false;
        } else if (serial != stream.serial_number) {
            return std::unexpected(malformed);
        }
        const auto continued = (header_type & 0x01U) != 0U;
        if (continued != current_open) {
            return std::unexpected(malformed);
        }
        const std::size_t segment_count = bytes[offset + 26U];
        auto lacing = offset + page_header_size;
        if (bytes.size() - lacing < segment_count) {
            return std::unexpected(malformed);
        }
        auto payload = lacing + segment_count;
        for (std::size_t segment = 0U; segment < segment_count; ++segment) {
            const std::size_t length = bytes[lacing + segment];
            if (bytes.size() - payload < length) {
                return std::unexpected(malformed);
            }
            current.insert(current.end(), bytes.begin() + static_cast<std::ptrdiff_t>(payload),
                           bytes.begin() + static_cast<std::ptrdiff_t>(payload + length));
            payload += length;
            current_open = true;
            if (length < 255U) {
                stream.packets.push_back(std::move(current));
                current.clear();
                current_open = false;
            }
        }
        offset = payload;
    }
    if (current_open) {
        return std::unexpected(malformed);
    }
    return stream;
}

// The entries of a Vorbis or Opus comment packet -- "KEY=value", as stored --
// after its `magic_size` bytes of codec magic; none when malformed.
[[nodiscard]] inline std::optional<std::vector<std::string>>
comment_entries(const std::vector<unsigned char>& packet, const std::size_t magic_size) {
    std::size_t at = magic_size;
    const auto read_length = [&packet, &at]() -> std::optional<std::size_t> {
        if (packet.size() - std::min(at, packet.size()) < 4U) {
            return std::nullopt;
        }
        const auto length = static_cast<std::size_t>(packet[at]) |
                            (static_cast<std::size_t>(packet[at + 1U]) << 8U) |
                            (static_cast<std::size_t>(packet[at + 2U]) << 16U) |
                            (static_cast<std::size_t>(packet[at + 3U]) << 24U);
        at += 4U;
        return length;
    };
    const auto vendor = read_length();
    if (!vendor || packet.size() - at < *vendor) {
        return std::nullopt;
    }
    at += *vendor;
    const auto count = read_length();
    if (!count) {
        return std::nullopt;
    }
    std::vector<std::string> entries;
    for (std::size_t index = 0U; index < *count; ++index) {
        const auto length = read_length();
        if (!length || packet.size() - at < *length) {
            return std::nullopt;
        }
        entries.emplace_back(packet.begin() + static_cast<std::ptrdiff_t>(at),
                             packet.begin() + static_cast<std::ptrdiff_t>(at + *length));
        at += *length;
    }
    return entries;
}

// ADR-0334: the comment entries that carry covers -- METADATA_BLOCK_PICTURE,
// and the older COVERART with its COVERARTMIME -- as "KEY=value" with the
// key upper-cased, in order.
[[nodiscard]] inline std::vector<std::string>
cover_entries(const std::vector<std::string>& entries) {
    std::vector<std::string> covers;
    for (const auto& entry : entries) {
        const auto equals = entry.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        std::string key = entry.substr(0U, equals);
        std::ranges::transform(key, key.begin(), [](const unsigned char letter) {
            return static_cast<char>(std::toupper(letter));
        });
        if (key == "METADATA_BLOCK_PICTURE" || key == "COVERART" || key == "COVERARTMIME") {
            covers.push_back(key + entry.substr(equals));
        }
    }
    return covers;
}

// Base64 as comments carry it, whitespace and padding aside; the bytes, or
// the text itself where it is not base64.
[[nodiscard]] inline std::string base64_or_text(const std::string_view text) {
    std::string bytes;
    std::uint32_t buffer = 0U;
    int bits = 0;
    for (const auto letter : text) {
        int value = -1;
        if (letter >= 'A' && letter <= 'Z') {
            value = letter - 'A';
        } else if (letter >= 'a' && letter <= 'z') {
            value = letter - 'a' + 26;
        } else if (letter >= '0' && letter <= '9') {
            value = letter - '0' + 52;
        } else if (letter == '+') {
            value = 62;
        } else if (letter == '/') {
            value = 63;
        } else if (letter == '=' || letter == '\n' || letter == '\r' || letter == ' ') {
            continue;
        } else {
            return std::string{text};
        }
        buffer = (buffer << 6U) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<char>((buffer >> static_cast<unsigned>(bits)) & 0xFFU));
        }
    }
    return bytes;
}

// Whether two files carry the same covers: the same entries, their base64
// compared as the bytes it carries -- a library re-encoding a picture
// unchanged keeps it -- in any order.
[[nodiscard]] inline bool same_covers(const std::vector<std::string>& source,
                                      const std::vector<std::string>& prepared) {
    const auto normal = [](const std::vector<std::string>& covers) {
        std::vector<std::pair<std::string, std::string>> pairs;
        for (const auto& cover : covers) {
            const auto equals = cover.find('=');
            const auto key = cover.substr(0U, equals);
            const auto value = std::string_view{cover}.substr(equals + 1U);
            pairs.emplace_back(key,
                               key == "COVERARTMIME" ? std::string{value} : base64_or_text(value));
        }
        std::ranges::sort(pairs);
        return pairs;
    };
    return normal(source) == normal(prepared);
}

} // namespace trackknife::metadata::ogg_stream_detail
