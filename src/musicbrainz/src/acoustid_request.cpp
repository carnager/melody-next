// SPDX-License-Identifier: GPL-3.0-only

// Building an AcoustID request needs nothing but text, and is kept apart from
// the Qt parts of this library so an engine without Qt can build one: ADR-0237
// keeps the AcoustID key in the engine, which makes the request itself.

#include "trackknife/musicbrainz/acoustid.hpp"

#include <array>
#include <string>
#include <utility>

namespace trackknife::musicbrainz {
namespace {

// RFC 3986 percent-encoding: every byte but the unreserved ASCII letters,
// digits and "-._~" -- what QUrl::toPercentEncoding produced here before.
[[nodiscard]] std::string form_encoded(const std::string_view value) {
    constexpr std::array<char, 16> hex{'0', '1', '2', '3', '4', '5', '6', '7',
                                       '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    std::string encoded;
    encoded.reserve(value.size());
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        const bool unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                                (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' ||
                                byte == '_' || byte == '~';
        if (unreserved) {
            encoded.push_back(character);
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[byte >> 4U]);
            encoded.push_back(hex[byte & 0x0FU]);
        }
    }
    return encoded;
}

} // namespace

core::Result<std::string> build_acoustid_lookup_body(const std::string_view client_key,
                                                     const std::size_t duration_seconds,
                                                     const std::string_view fingerprint) {
    if (client_key.empty() || duration_seconds == 0U || fingerprint.empty()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "an AcoustID lookup needs a client key, a duration, and a fingerprint",
            .context = {}});
    }
    return "client=" + form_encoded(client_key) + "&format=json&meta=recordings+releases" +
           "&duration=" + std::to_string(duration_seconds) +
           "&fingerprint=" + form_encoded(fingerprint);
}

} // namespace trackknife::musicbrainz
