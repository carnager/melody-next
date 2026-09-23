// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace trackknife::core {

enum class ErrorCode {
    cancelled,
    invalid_argument,
    not_found,
    conflict,
    unsupported,
    limit_exceeded,
    io,
    backend,
    database,
    invariant,
    // ADR-0223: the caller has not proven who it is. Distinct from
    // invalid_argument so a client can tell "ask again with a token" from
    // "that request was wrong".
    unauthorized,
};

struct ErrorContext {
    std::string key;
    std::string value;

    friend bool operator==(const ErrorContext&, const ErrorContext&) = default;
};

struct Error {
    ErrorCode code{ErrorCode::invariant};
    std::string message;
    std::vector<ErrorContext> context;

    [[nodiscard]] Error with_context(std::string key, std::string value) && {
        context.push_back({std::move(key), std::move(value)});
        return std::move(*this);
    }

    friend bool operator==(const Error&, const Error&) = default;
};

} // namespace trackknife::core
