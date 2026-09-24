// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/engine/transcode_cache.hpp"
#include "trackknife/output/stream_query.hpp"
#include "trackknife/protocol/dispatch.hpp"

#include <array>
#include <chrono>
#include <functional>
#include <string>
#include <string_view>

namespace trackknife::engine {

// What the stream port serves, and to whom (ADR-0228). Two keys open it:
//
//  - the agents' token, for what the player holds -- an output agent
//    fetching the track it was told to play, and nothing else;
//  - a ticket, for one track in one form until a time: signed by this
//    engine for a client that asked over the control connection, where it
//    had to give the password. A phone downloading an album for offline
//    listening fetches with tickets.
//
// A request that names a format is answered with the track converted --
// from the transcode cache, made then if need be.
class MediaStreams final {
  public:
    using Holds = std::function<bool(const std::string& raw_path)>;

    MediaStreams(std::string agent_token, Holds holds, TranscodeCache* cache);

    // The file to send for a request's query string, or why not: an
    // unauthorized request, one for something not served, or a conversion
    // that failed.
    [[nodiscard]] core::Result<std::string> resolve(std::string_view query) const;

    // A ticket's query string: the request, when it expires, and the
    // signature over both.
    [[nodiscard]] std::string ticket(const output::StreamRequest& request,
                                     std::chrono::seconds lifetime) const;

  private:
    [[nodiscard]] std::string signature(const std::string& signed_text) const;

    std::string agent_token_;
    Holds holds_;
    TranscodeCache* cache_;
    // Made at start: tickets live no longer than the engine that signed
    // them, which is as long as a download needs.
    std::array<unsigned char, 32> key_{};
};

class Catalogue;

// `streams.ticket`: a client that has given the password asks for a URL to
// one library track -- as it is, or as Opus at a bit rate -- and gets the
// port and the query to fetch it with, good for an hour. Only tracks in the
// library: the ticket opens music, not the machine's disk.
void register_stream_methods(protocol::Dispatcher& dispatcher, const MediaStreams& streams,
                             Catalogue& catalogue, std::uint16_t port);

} // namespace trackknife::engine
