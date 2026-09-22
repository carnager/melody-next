// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace trackknife::protocol {

using Json = nlohmann::json;

// ADR-0222: four shapes, distinguished by which keys are present.

// Has an id and a method. Expects exactly one response.
struct Request final {
    std::int64_t id{0};
    std::string method;
    Json params = Json::object();

    friend bool operator==(const Request&, const Request&) = default;
};

// A method call with no id: fire and forget, never answered.
struct Notification final {
    std::string method;
    Json params = Json::object();

    friend bool operator==(const Notification&, const Notification&) = default;
};

// `code` is a stable machine-readable string; `message` is human text that may
// change freely and must not be parsed. `context` carries what the code alone
// cannot say, such as which path failed.
struct Error final {
    std::string code;
    std::string message;
    Json context = Json::object();

    friend bool operator==(const Error&, const Error&) = default;
};

// Carries the id it answers and exactly one of result or error.
struct Response final {
    std::int64_t id{0};
    std::optional<Json> result;
    std::optional<Error> error;

    friend bool operator==(const Response&, const Response&) = default;
};

// Unsolicited, server to client, never carrying an id. Not ordered against
// responses: an event may arrive before the response to the request that
// caused it, so clients reconcile on state rather than sequence.
struct Event final {
    std::string name;
    Json data = Json::object();

    friend bool operator==(const Event&, const Event&) = default;
};

using Message = std::variant<Request, Notification, Response, Event>;

// Parses one framed line. The line must not include its terminator.
[[nodiscard]] core::Result<Message> parse_message(std::string_view line);

// Encodes one message as a single line, without the terminator. Never
// pretty-prints: a raw newline would break framing.
[[nodiscard]] std::string encode_message(const Message& message);

// Raw OS paths are bytes, not text, and JSON strings must be valid UTF-8, so
// paths travel base64 encoded. This is the same convention the workspace
// already uses for persisted paths in JSON.
[[nodiscard]] std::string encode_raw_path(std::string_view raw_path);
[[nodiscard]] core::Result<std::string> decode_raw_path(std::string_view encoded);

} // namespace trackknife::protocol
