// SPDX-License-Identifier: GPL-3.0-only
#include "trackknife/engine/media_streams.hpp"

#include "trackknife/engine/catalogue.hpp"
#include "trackknife/protocol/message.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <charconv>
#include <utility>

namespace trackknife::engine {
namespace {

[[nodiscard]] core::Error refused(core::ErrorCode code, std::string message) {
    return core::Error{.code = code, .message = std::move(message), .context = {}};
}

[[nodiscard]] bool same(const std::string_view left, const std::string_view right) {
    return left.size() == right.size() &&
           CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

[[nodiscard]] std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

MediaStreams::MediaStreams(std::string agent_token, Holds holds, TranscodeCache* cache)
    : agent_token_(std::move(agent_token)), holds_(std::move(holds)), cache_(cache) {
    if (RAND_bytes(key_.data(), static_cast<int>(key_.size())) != 1) {
        // No randomness is no tickets: a key nobody knows, not a known one.
        key_.fill(0);
        agent_token_.clear();
    }
}

std::string MediaStreams::signature(const std::string& signed_text) const {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    HMAC(EVP_sha256(), key_.data(), static_cast<int>(key_.size()),
         reinterpret_cast<const unsigned char*>(signed_text.data()), signed_text.size(),
         digest.data(), &length);
    static constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    for (unsigned int index = 0; index < length; ++index) {
        hex.push_back(digits[digest[index] >> 4U]);
        hex.push_back(digits[digest[index] & 0x0FU]);
    }
    return hex;
}

std::string MediaStreams::ticket(const output::StreamRequest& request,
                                 const std::chrono::seconds lifetime) const {
    const auto signed_text = output::stream_query(request) + "&expires=" +
                             std::to_string(now_seconds() + lifetime.count());
    return signed_text + "&ticket=" + signature(signed_text);
}

core::Result<std::string> MediaStreams::resolve(const std::string_view query) const {
    auto request = output::parse_stream_query(query);
    if (!request) {
        return std::unexpected(std::move(request.error()));
    }
    const auto token = output::query_value(query, "token");
    const auto ticket = output::query_value(query, "ticket");
    if (token && !agent_token_.empty() && same(*token, agent_token_)) {
        // Not being played is the same answer as not existing: the token
        // opens what the engine plays, and says nothing about anything else.
        if (!holds_(request->raw_path)) {
            return std::unexpected(refused(core::ErrorCode::not_found, "not being played"));
        }
    } else if (ticket) {
        const auto expires = output::query_value(query, "expires").value_or("");
        std::int64_t until = 0;
        const auto parsed = std::from_chars(expires.data(), expires.data() + expires.size(), until);
        const auto signed_text = output::stream_query(*request) + "&expires=" + expires;
        if (parsed.ec != std::errc{} || !same(*ticket, signature(signed_text))) {
            return std::unexpected(refused(core::ErrorCode::unauthorized, "not a ticket this engine signed"));
        }
        if (until < now_seconds()) {
            return std::unexpected(refused(core::ErrorCode::unauthorized, "the ticket has expired"));
        }
    } else {
        return std::unexpected(refused(core::ErrorCode::unauthorized, "no token or ticket"));
    }
    if (!request->format) {
        return request->raw_path;
    }
    if (cache_ == nullptr) {
        return std::unexpected(refused(core::ErrorCode::unsupported, "this engine converts nothing"));
    }
    auto converted = cache_->ensure(TranscodeSource{.raw_path = request->raw_path,
                                                    .selection = request->selection,
                                                    .segment = request->segment},
                                    *request->format);
    if (!converted) {
        return std::unexpected(std::move(converted.error()));
    }
    return converted->native();
}

void register_stream_methods(protocol::Dispatcher& dispatcher, const MediaStreams& streams,
                             Catalogue& catalogue, const std::uint16_t port) {
    dispatcher.on("streams.ticket", [&streams, &catalogue, port](const protocol::Json& params)
                                        -> core::Result<protocol::Json> {
        const auto encoded = params.find("path");
        if (encoded == params.end() || !encoded->is_string()) {
            return std::unexpected(refused(core::ErrorCode::invalid_argument, "a path is required"));
        }
        auto raw_path = protocol::decode_raw_path(encoded->get<std::string>());
        if (!raw_path) {
            return std::unexpected(refused(core::ErrorCode::invalid_argument, "path is not an encoded path"));
        }
        persistence::LibraryQuery lookup;
        lookup.kind = persistence::LibraryEntryKind::track;
        lookup.raw_path = *raw_path;
        lookup.limit = 1;
        auto found = catalogue.query(lookup);
        if (!found) {
            return std::unexpected(std::move(found.error()));
        }
        if (found->entries.empty()) {
            return std::unexpected(refused(core::ErrorCode::not_found, "not a track in the library"));
        }
        output::StreamRequest request{.raw_path = std::move(*raw_path), .format = {}, .selection = {}, .segment = {}};
        if (params.value("format", std::string{"original"}) == "opus") {
            request.format = output::StreamFormat{.bitrate_kbps = std::clamp(params.value("bitrate", 128), 16, 512)};
        }
        return protocol::Json{{"port", port},
                              {"query", streams.ticket(request, std::chrono::hours{1})}};
    });
}

} // namespace trackknife::engine
