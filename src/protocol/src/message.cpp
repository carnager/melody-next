// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/protocol/message.hpp"

#include <array>
#include <string>

namespace trackknife::protocol {
namespace {

[[nodiscard]] core::Error malformed(std::string message, std::string detail = {}) {
    core::Error error{
        .code = core::ErrorCode::invalid_argument, .message = std::move(message), .context = {}};
    if (!detail.empty()) {
        error.context.push_back({.key = "detail", .value = std::move(detail)});
    }
    return error;
}

// An object payload or nothing; a scalar where an object belongs is a
// malformed message rather than something to coerce.
[[nodiscard]] core::Result<Json> object_member(const Json& parent, const char* key) {
    const auto found = parent.find(key);
    if (found == parent.end()) {
        return Json::object();
    }
    if (!found->is_object()) {
        return std::unexpected(malformed("protocol member must be an object", key));
    }
    return *found;
}

constexpr std::string_view base64_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

} // namespace

core::Result<Message> parse_message(const std::string_view line) {
    // Framing is one object per line, so an embedded newline means the caller
    // has already lost the boundary.
    if (line.find('\n') != std::string_view::npos) {
        return std::unexpected(malformed("a framed message must not contain a newline"));
    }
    const auto document = Json::parse(line, nullptr, false);
    if (document.is_discarded()) {
        return std::unexpected(malformed("message is not valid JSON"));
    }
    if (!document.is_object()) {
        return std::unexpected(malformed("message must be a JSON object"));
    }

    const auto has = [&document](const char* key) { return document.contains(key); };

    if (has("event")) {
        if (has("id") || has("method") || has("result") || has("error")) {
            return std::unexpected(malformed("an event carries no id, method, result or error"));
        }
        if (!document["event"].is_string()) {
            return std::unexpected(malformed("event name must be a string"));
        }
        auto data = object_member(document, "data");
        if (!data) {
            return std::unexpected(std::move(data.error()));
        }
        return Message{
            Event{.name = document["event"].get<std::string>(), .data = std::move(*data)}};
    }

    if (has("result") || has("error")) {
        if (has("method")) {
            return std::unexpected(malformed("a response carries no method"));
        }
        if (has("result") && has("error")) {
            return std::unexpected(malformed("a response carries result or error, never both"));
        }
        if (!has("id") || !document["id"].is_number_integer()) {
            return std::unexpected(malformed("a response must carry the integer id it answers"));
        }
        Response response{.id = document["id"].get<std::int64_t>(), .result = {}, .error = {}};
        if (has("result")) {
            response.result = document["result"];
            return Message{std::move(response)};
        }
        const auto& failure = document["error"];
        if (!failure.is_object() || !failure.contains("code") || !failure["code"].is_string()) {
            return std::unexpected(malformed("an error must be an object with a string code"));
        }
        auto context = object_member(failure, "context");
        if (!context) {
            return std::unexpected(std::move(context.error()));
        }
        response.error = Error{
            .code = failure["code"].get<std::string>(),
            .message = failure.contains("message") && failure["message"].is_string()
                           ? failure["message"].get<std::string>()
                           : std::string{},
            .context = std::move(*context),
        };
        return Message{std::move(response)};
    }

    if (!has("method")) {
        return std::unexpected(malformed("message has no method, result, error or event"));
    }
    if (!document["method"].is_string()) {
        return std::unexpected(malformed("method must be a string"));
    }
    auto params = object_member(document, "params");
    if (!params) {
        return std::unexpected(std::move(params.error()));
    }
    // An id makes it a request; without one it is fire and forget.
    if (!has("id")) {
        return Message{Notification{.method = document["method"].get<std::string>(),
                                    .params = std::move(*params)}};
    }
    if (!document["id"].is_number_integer() || document["id"].get<std::int64_t>() <= 0) {
        return std::unexpected(malformed("a request id must be a positive integer"));
    }
    return Message{Request{.id = document["id"].get<std::int64_t>(),
                           .method = document["method"].get<std::string>(),
                           .params = std::move(*params)}};
}

std::string encode_message(const Message& message) {
    Json document = Json::object();
    std::visit(
        [&document](const auto& value) {
            using Kind = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Kind, Request>) {
                document["id"] = value.id;
                document["method"] = value.method;
                document["params"] = value.params;
            } else if constexpr (std::is_same_v<Kind, Notification>) {
                document["method"] = value.method;
                document["params"] = value.params;
            } else if constexpr (std::is_same_v<Kind, Response>) {
                document["id"] = value.id;
                if (value.result) {
                    document["result"] = *value.result;
                } else if (value.error) {
                    Json failure = Json::object();
                    failure["code"] = value.error->code;
                    failure["message"] = value.error->message;
                    failure["context"] = value.error->context;
                    document["error"] = std::move(failure);
                }
            } else {
                document["event"] = value.name;
                document["data"] = value.data;
            }
        },
        message);
    // dump() without indent emits no newline; ensure_ascii keeps the line
    // byte-safe for transports that are not UTF-8 clean.
    return document.dump(-1, ' ', true);
}

std::string encode_raw_path(const std::string_view raw_path) {
    std::string encoded;
    encoded.reserve((raw_path.size() + 2U) / 3U * 4U);
    std::size_t index = 0;
    while (index + 2U < raw_path.size()) {
        const auto triple =
            static_cast<std::uint32_t>((static_cast<unsigned char>(raw_path[index]) << 16) |
                                       (static_cast<unsigned char>(raw_path[index + 1U]) << 8) |
                                       static_cast<unsigned char>(raw_path[index + 2U]));
        encoded.push_back(base64_alphabet[(triple >> 18) & 0x3FU]);
        encoded.push_back(base64_alphabet[(triple >> 12) & 0x3FU]);
        encoded.push_back(base64_alphabet[(triple >> 6) & 0x3FU]);
        encoded.push_back(base64_alphabet[triple & 0x3FU]);
        index += 3U;
    }
    if (const auto remaining = raw_path.size() - index; remaining > 0U) {
        auto triple = static_cast<std::uint32_t>(static_cast<unsigned char>(raw_path[index]) << 16);
        if (remaining == 2U) {
            triple |=
                static_cast<std::uint32_t>(static_cast<unsigned char>(raw_path[index + 1U]) << 8);
        }
        encoded.push_back(base64_alphabet[(triple >> 18) & 0x3FU]);
        encoded.push_back(base64_alphabet[(triple >> 12) & 0x3FU]);
        encoded.push_back(remaining == 2U ? base64_alphabet[(triple >> 6) & 0x3FU] : '=');
        encoded.push_back('=');
    }
    return encoded;
}

std::string displayable_text(const std::string_view text) {
    std::string result;
    result.reserve(text.size());
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t length = 0;
        std::uint32_t minimum = 0;
        if (lead < 0x80U) {
            result.push_back(static_cast<char>(lead));
            ++index;
            continue;
        }
        if ((lead & 0xE0U) == 0xC0U) {
            length = 2;
            minimum = 0x80U;
        } else if ((lead & 0xF0U) == 0xE0U) {
            length = 3;
            minimum = 0x800U;
        } else if ((lead & 0xF8U) == 0xF0U) {
            length = 4;
            minimum = 0x10000U;
        }
        bool valid = length != 0 && index + length <= text.size();
        std::uint32_t code_point = valid ? (lead & (0xFFU >> (length + 1U))) : 0U;
        for (std::size_t offset = 1; valid && offset < length; ++offset) {
            const auto next = static_cast<unsigned char>(text[index + offset]);
            valid = (next & 0xC0U) == 0x80U;
            code_point = (code_point << 6U) | (next & 0x3FU);
        }
        // Overlong forms, surrogates and values past U+10FFFF are as invalid
        // as a stray byte.
        valid = valid && code_point >= minimum && code_point <= 0x10FFFFU &&
                (code_point < 0xD800U || code_point > 0xDFFFU);
        if (valid) {
            result.append(text.substr(index, length));
            index += length;
        } else {
            result.append("\xEF\xBF\xBD");
            ++index;
        }
    }
    return result;
}

core::Result<std::string> decode_raw_path(const std::string_view encoded) {
    if (encoded.size() % 4U != 0U) {
        return std::unexpected(malformed("encoded path length must be a multiple of four"));
    }
    std::array<signed char, 256> reverse{};
    reverse.fill(-1);
    for (std::size_t position = 0; position < base64_alphabet.size(); ++position) {
        reverse[static_cast<unsigned char>(base64_alphabet[position])] =
            static_cast<signed char>(position);
    }
    std::string raw_path;
    raw_path.reserve(encoded.size() / 4U * 3U);
    for (std::size_t index = 0; index < encoded.size(); index += 4U) {
        std::uint32_t quad = 0;
        // Padding is counted rather than positioned: two '=' mean one decoded
        // byte, one means two. Deriving the count from the last padding index
        // gets the two-padding case wrong.
        std::size_t padding = 0;
        for (std::size_t offset = 0; offset < 4U; ++offset) {
            const auto symbol = encoded[index + offset];
            if (symbol == '=') {
                if (index + 4U != encoded.size() || offset < 2U) {
                    return std::unexpected(malformed("misplaced padding in encoded path"));
                }
                ++padding;
                quad <<= 6;
                continue;
            }
            if (padding > 0U) {
                return std::unexpected(malformed("encoded path has data after padding"));
            }
            const auto value = reverse[static_cast<unsigned char>(symbol)];
            if (value < 0) {
                return std::unexpected(malformed("encoded path contains a non-base64 character"));
            }
            quad = (quad << 6) | static_cast<std::uint32_t>(value);
        }
        const auto bytes = 3U - padding;
        raw_path.push_back(static_cast<char>((quad >> 16) & 0xFFU));
        if (bytes > 1U) {
            raw_path.push_back(static_cast<char>((quad >> 8) & 0xFFU));
        }
        if (bytes > 2U) {
            raw_path.push_back(static_cast<char>(quad & 0xFFU));
        }
    }
    return raw_path;
}

} // namespace trackknife::protocol
