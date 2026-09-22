// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/error.hpp"
#include "trackknife/protocol/message.hpp"

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::protocol {

// ADR-0222: error.code is a stable machine-readable string from a closed set.
// The strings are the wire contract, not the enumerator names -- renaming an
// enumerator must not change what goes over the socket, so the mapping is
// written out rather than derived.
[[nodiscard]] std::string_view error_code_name(core::ErrorCode code) noexcept;

// The inverse, for a client turning a response back into a typed error. An
// unknown code is not an error in itself: a newer engine may report something
// this client has never heard of, and treating that as a parse failure would
// make every added code a breaking change. Unknown maps to `invariant`, which
// is the "something is wrong and it is not your fault" case.
[[nodiscard]] core::ErrorCode error_code_from_name(std::string_view name) noexcept;

// Renders a core::Error onto the wire, flattening its context pairs into the
// object the envelope carries.
[[nodiscard]] Error to_protocol_error(const core::Error& error);

// Routes a request to a registered handler and turns its outcome into a
// response. Dispatch itself is transport-agnostic and synchronous: whoever
// owns the socket decides about threads, and a long operation is a job rather
// than a slow handler.
class Dispatcher final {
  public:
    // Handlers receive the request's params and return a result document or an
    // error. Params have already been checked to be an object.
    using Handler = std::function<core::Result<Json>(const Json& params)>;

    void on(std::string method, Handler handler);

    [[nodiscard]] bool knows(std::string_view method) const;

    // Every registered method name, sorted. For a caller that has to wrap or
    // mirror the whole surface: listing the names by hand means a method
    // added later is silently missing from the wrapper.
    [[nodiscard]] std::vector<std::string> methods() const;

    // Always produces a response, because a request always gets exactly one.
    // An unknown method answers `unsupported` rather than going unanswered,
    // which would leave the caller waiting forever.
    [[nodiscard]] Response dispatch(const Request& request) const;

  private:
    std::map<std::string, Handler, std::less<>> handlers_;
};

} // namespace trackknife::protocol
