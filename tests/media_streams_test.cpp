// SPDX-License-Identifier: GPL-3.0-only
//
// The stream port as a phone meets it: tracks converted to Opus when asked,
// fetched with the agents' token or with a ticket the engine signed for a
// client that gave the password -- and nothing else.

#include "trackknife/core/stable_id.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/media_streams.hpp"
#include "trackknife/engine/stream_server.hpp"
#include "trackknife/engine/transcode_cache.hpp"
#include "trackknife/protocol/message.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

namespace engine = trackknife::engine;
namespace output = trackknife::output;
namespace protocol = trackknife::protocol;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

// One GET, the whole response: status line and body.
[[nodiscard]] std::pair<std::string, std::string> get(const std::uint16_t port, const std::string& target) {
    const int connection = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    require(::connect(connection, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
            "the stream port answers");
    const auto request = "GET " + target + " HTTP/1.1\r\nHost: test\r\n\r\n";
    require(::send(connection, request.data(), request.size(), MSG_NOSIGNAL) ==
                static_cast<ssize_t>(request.size()),
            "the request is sent");
    std::string response;
    std::array<char, 65536> buffer{};
    while (true) {
        const auto received = ::recv(connection, buffer.data(), buffer.size(), 0);
        if (received <= 0) {
            break;
        }
        response.append(buffer.data(), static_cast<std::size_t>(received));
    }
    ::close(connection);
    const auto head_end = response.find("\r\n\r\n");
    return {response.substr(0, response.find("\r\n")),
            head_end == std::string::npos ? std::string{} : response.substr(head_end + 4U)};
}

} // namespace

int main(int argc, char** argv) {
    require(argc == 2, "usage: media_streams_test <fixture-dir>");
    const auto directory = std::filesystem::temp_directory_path() /
                           ("trackknife-streams-" + trackknife::core::StableId::random().to_string());
    const auto music = directory / "music";
    std::filesystem::create_directories(music);
    {
        std::ifstream input{std::filesystem::path{argv[1]} / "rich-metadata-long-flac.b64"};
        std::string base64((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        std::erase(base64, '\n');
        const auto decoded = protocol::decode_raw_path(base64);
        require(decoded.has_value(), "the fixture decodes");
        std::ofstream file{music / "track.flac", std::ios::binary};
        file.write(decoded->data(), static_cast<std::streamsize>(decoded->size()));
    }
    const auto track = (music / "track.flac").string();
    std::ofstream{directory / "elsewhere.flac"} << "not in the library";

    engine::LocalCatalogue catalogue{directory / "library.sqlite3"};
    require(catalogue.prepare().has_value() && catalogue.add_root(music.string()).has_value(),
            "a library");
    trackknife::persistence::LibraryScanProgress progress;
    require(catalogue.scan({}, progress).has_value(), "scanned");

    engine::TranscodeCache cache{directory / "transcodes", 64U * 1024U * 1024U};
    std::string playing;
    engine::MediaStreams media{"agent-token", [&playing](const std::string& path) { return path == playing; },
                               &cache};
    auto server = engine::StreamServer::listen(
        "127.0.0.1", 0, [&media](const std::string_view query) { return media.resolve(query); });
    require(server.has_value(), "the stream port opens");
    (*server)->start();
    const auto port = (*server)->port();

    // A client asks, over the control connection, for a track as Opus.
    protocol::Dispatcher dispatcher;
    engine::register_stream_methods(dispatcher, media, catalogue, port);
    const auto ask = [&dispatcher](const protocol::Json& params) {
        return dispatcher.dispatch(protocol::Request{.id = 1, .method = "streams.ticket", .params = params});
    };
    const auto answer = ask(protocol::Json{{"path", protocol::encode_raw_path(track)}, {"format", "opus"}, {"bitrate", 96}});
    require(answer.result.has_value(), "a ticket for a library track");
    require(answer.result->at("port") == port, "with the port to fetch it from");
    const auto query = answer.result->at("query").get<std::string>();

    const auto started = std::chrono::steady_clock::now();
    const auto [status, body] = get(port, "/stream?" + query);
    const auto first = std::chrono::steady_clock::now() - started;
    require(status == "HTTP/1.1 200 OK", "the ticket opens it");
    require(body.starts_with("OggS") && body.find("OpusHead") != std::string::npos, "as Opus");
    const auto again_started = std::chrono::steady_clock::now();
    const auto again = get(port, "/stream?" + query);
    require(again.second == body, "the same again");
    require(std::chrono::steady_clock::now() - again_started < first, "from the cache, not converted twice");

    // As it is: the original bytes.
    const auto original = ask(protocol::Json{{"path", protocol::encode_raw_path(track)}});
    const auto whole = get(port, "/stream?" + original.result->at("query").get<std::string>());
    require(whole.first == "HTTP/1.1 200 OK" && whole.second.starts_with("fLaC"), "or as it is");

    // A ticket is for what it names, and until it says.
    auto forged = query;
    forged.replace(forged.find("bitrate=96"), 10, "bitrate=64");
    require(get(port, "/stream?" + forged).first == "HTTP/1.1 403 Forbidden", "a changed ticket opens nothing");
    const auto expired = media.ticket(output::StreamRequest{.raw_path = track, .format = {}, .selection = {}, .segment = {}},
                                      std::chrono::seconds{-5});
    require(get(port, "/stream?" + expired).first == "HTTP/1.1 403 Forbidden", "nor an expired one");
    require(get(port, "/stream?path=" + output::percent_encoded(protocol::encode_raw_path(track))).first ==
                "HTTP/1.1 403 Forbidden",
            "nor no key at all");

    // Only library tracks get tickets.
    const auto outside = ask(protocol::Json{{"path", protocol::encode_raw_path((directory / "elsewhere.flac").string())}});
    require(outside.error.has_value() && outside.error->code == "not_found", "a file outside the library gets none");

    // The agents' token opens what plays, converted when asked.
    const auto agent = output::stream_query(output::StreamRequest{
                           .raw_path = track, .format = output::StreamFormat{.bitrate_kbps = 64}, .selection = {}, .segment = {}}) +
                       "&token=agent-token";
    require(get(port, "/stream?" + agent).first == "HTTP/1.1 404 Not Found", "not while it is not played");
    playing = track;
    const auto streamed = get(port, "/stream?" + agent);
    require(streamed.first == "HTTP/1.1 200 OK" && streamed.second.starts_with("OggS"), "and then, as Opus");
    // A part of a file goes only converted.
    require(get(port, "/stream?path=" + output::percent_encoded(protocol::encode_raw_path(track)) +
                          "&start=0&end=1000&token=agent-token")
                    .first == "HTTP/1.1 400 Bad Request",
            "a part of the original is not something to send");

    (*server)->stop();
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::cout << "media streams: ok\n";
    return EXIT_SUCCESS;
}
