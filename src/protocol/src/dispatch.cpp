// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/protocol/dispatch.hpp"

#include <array>
#include <utility>

namespace trackknife::protocol {
namespace {

// The wire names. Changing one of these strings is a protocol change; changing
// an enumerator name is not.
constexpr std::array<std::pair<core::ErrorCode, std::string_view>, 10> error_code_names{{
    {core::ErrorCode::cancelled, "cancelled"},
    {core::ErrorCode::invalid_argument, "invalid_argument"},
    {core::ErrorCode::not_found, "not_found"},
    {core::ErrorCode::conflict, "conflict"},
    {core::ErrorCode::unsupported, "unsupported"},
    {core::ErrorCode::limit_exceeded, "limit_exceeded"},
    {core::ErrorCode::io, "io"},
    {core::ErrorCode::backend, "backend"},
    {core::ErrorCode::database, "database"},
    {core::ErrorCode::invariant, "invariant"},
}};

} // namespace

std::string_view error_code_name(const core::ErrorCode code) noexcept {
    for (const auto& [value, name] : error_code_names) {
        if (value == code) {
            return name;
        }
    }
    return "invariant";
}

core::ErrorCode error_code_from_name(const std::string_view name) noexcept {
    for (const auto& [value, candidate] : error_code_names) {
        if (candidate == name) {
            return value;
        }
    }
    return core::ErrorCode::invariant;
}

Error to_protocol_error(const core::Error& error) {
    Error rendered{.code = std::string{error_code_name(error.code)},
                   .message = error.message,
                   .context = Json::object()};
    for (const auto& pair : error.context) {
        rendered.context[pair.key] = pair.value;
    }
    return rendered;
}

void Dispatcher::on(std::string method, Handler handler) {
    handlers_.insert_or_assign(std::move(method), std::move(handler));
}

bool Dispatcher::knows(const std::string_view method) const {
    return handlers_.find(method) != handlers_.end();
}

std::vector<std::string> Dispatcher::methods() const {
    std::vector<std::string> names;
    names.reserve(handlers_.size());
    for (const auto& [name, handler] : handlers_) {
        names.push_back(name);
    }
    return names;
}

Response Dispatcher::dispatch(const Request& request) const {
    const auto found = handlers_.find(request.method);
    if (found == handlers_.end()) {
        return {.id = request.id,
                .result = {},
                .error = to_protocol_error(
                    core::Error{.code = core::ErrorCode::unsupported,
                                .message = "unknown method",
                                .context = {{.key = "method", .value = request.method}}})};
    }
    auto outcome = found->second(request.params);
    if (!outcome) {
        return {.id = request.id, .result = {}, .error = to_protocol_error(outcome.error())};
    }
    return {.id = request.id, .result = std::move(*outcome), .error = {}};
}

} // namespace trackknife::protocol
