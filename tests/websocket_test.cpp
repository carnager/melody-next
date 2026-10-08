// SPDX-License-Identifier: GPL-3.0-only
//
// ADR-0271: the protocol as a WebSocket on the stream port, as a phone behind
// an HTTP proxy meets it -- the handshake, the password asked as on TCP,
// one message a line, pings answered, a broken frame refused.

#include "trackknife/core/posix.hpp"
#include "trackknife/engine/server.hpp"
#include "trackknife/engine/stream_server.hpp"
#include "trackknife/engine/websocket.hpp"
#include "trackknife/protocol/dispatch.hpp"
#include "trackknife/protocol/message.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace engine = trackknife::engine;
namespace protocol = trackknife::protocol;
using Json = protocol::Json;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

[[nodiscard]] int connect_to(const std::uint16_t port) {
    const int connection = trackknife::core::socket_cloexec(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    require(::connect(connection, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
                0,
            "the stream port answers");
    return connection;
}

void send_bytes(const int descriptor, const std::string& bytes) {
    require(::send(descriptor, bytes.data(), bytes.size(), MSG_NOSIGNAL) ==
                static_cast<ssize_t>(bytes.size()),
            "sent");
}

// Up to `size` bytes more, within two seconds; fewer when the other end is
// done or slow.
[[nodiscard]] std::string receive(const int descriptor, std::string& kept, const std::size_t size) {
    std::array<char, 4096> buffer{};
    while (kept.size() < size) {
        pollfd watched{.fd = descriptor, .events = POLLIN, .revents = 0};
        if (::poll(&watched, 1, 2000) <= 0) {
            break;
        }
        const auto received = ::recv(descriptor, buffer.data(), buffer.size(), 0);
        if (received <= 0) {
            break;
        }
        kept.append(buffer.data(), static_cast<std::size_t>(received));
    }
    auto taken = kept.substr(0, size);
    kept.erase(0, taken.size());
    return taken;
}

[[nodiscard]] std::string handshake(const int descriptor, std::string& kept,
                                    const std::string& extra = {}) {
    send_bytes(descriptor, "GET /protocol HTTP/1.1\r\nHost: music.example\r\nUpgrade: websocket\r\n"
                           "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                           "Sec-WebSocket-Version: 13\r\n" +
                               extra + "\r\n");
    std::string head;
    while (head.find("\r\n\r\n") == std::string::npos) {
        const auto more = receive(descriptor, kept, 1);
        if (more.empty()) {
            break;
        }
        head += more;
    }
    return head;
}

// A client's frame: masked, as every client's is -- or not, to be refused.
[[nodiscard]] std::string client_frame(const std::uint8_t opcode, const std::string_view payload,
                                       const bool masked = true) {
    std::string out;
    out.push_back(static_cast<char>(0x80U | opcode));
    const std::array<std::uint8_t, 4> mask{0x12, 0x34, 0x56, 0x78};
    const auto marker = static_cast<std::uint8_t>(masked ? 0x80U : 0U);
    if (payload.size() < 126U) {
        out.push_back(static_cast<char>(marker | payload.size()));
    } else {
        out.push_back(static_cast<char>(marker | 126U));
        out.push_back(static_cast<char>((payload.size() >> 8U) & 0xFFU));
        out.push_back(static_cast<char>(payload.size() & 0xFFU));
    }
    if (masked) {
        for (const auto byte : mask) {
            out.push_back(static_cast<char>(byte));
        }
    }
    for (std::size_t index = 0; index < payload.size(); ++index) {
        out.push_back(
            masked ? static_cast<char>(static_cast<std::uint8_t>(payload[index]) ^ mask[index % 4U])
                   : payload[index]);
    }
    return out;
}

struct Frame {
    std::uint8_t opcode{0};
    std::string payload;
};

[[nodiscard]] std::optional<Frame> server_frame(const int descriptor, std::string& kept) {
    const auto start = receive(descriptor, kept, 2);
    if (start.size() < 2U) {
        return std::nullopt;
    }
    require((static_cast<std::uint8_t>(start[1]) & 0x80U) == 0U, "a server's frame is not masked");
    std::size_t size = static_cast<std::uint8_t>(start[1]) & 0x7FU;
    if (size == 126U) {
        const auto extended = receive(descriptor, kept, 2);
        size = (static_cast<std::size_t>(static_cast<std::uint8_t>(extended[0])) << 8U) |
               static_cast<std::uint8_t>(extended[1]);
    }
    return Frame{.opcode = static_cast<std::uint8_t>(static_cast<std::uint8_t>(start[0]) & 0x0FU),
                 .payload = receive(descriptor, kept, size)};
}

[[nodiscard]] Json ask(const int descriptor, std::string& kept, const Json& request) {
    send_bytes(descriptor, client_frame(0x1, request.dump()));
    const auto answer = server_frame(descriptor, kept);
    require(answer.has_value() && answer->opcode == 0x1, "an answer comes as a text message");
    return Json::parse(answer->payload);
}

} // namespace

int main() {
    // RFC 6455's own example.
    require(engine::websocket_accept("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
            "the handshake's answer is RFC 6455's");

    protocol::Dispatcher dispatcher;
    dispatcher.on("test.echo", [](const Json& params) -> trackknife::core::Result<Json> {
        return Json{{"echo", params.value("said", std::string{})}};
    });
    auto tcp = engine::Server::listen_tcp("127.0.0.1", 0, dispatcher, "secret");
    require(tcp.has_value(), "the TCP listener starts");
    (*tcp)->start();
    auto& server = **tcp;
    auto streams = engine::StreamServer::listen(
        "127.0.0.1", 0,
        [](std::string_view) -> trackknife::core::Result<std::string> {
            return std::unexpected(trackknife::core::Error{
                .code = trackknife::core::ErrorCode::not_found, .message = "none", .context = {}});
        },
        [&server]() -> int {
            std::array<int, 2> pair{-1, -1};
            if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair.data()) < 0) {
                return -1;
            }
            server.attach(pair[0], false);
            return pair[1];
        });
    require(streams.has_value(), "the stream port starts");
    (*streams)->start();
    const auto port = (*streams)->port();

    {
        const auto client = connect_to(port);
        std::string kept;
        const auto head = handshake(client, kept);
        require(head.starts_with("HTTP/1.1 101"), "the WebSocket is taken");
        require(head.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") !=
                    std::string::npos,
                "the handshake is answered");

        // As on TCP: nothing before the password.
        const auto refused =
            ask(client, kept,
                Json{{"id", 1}, {"method", "test.echo"}, {"params", {{"said", "early"}}}});
        require(refused.contains("error") &&
                    refused["error"].value("code", std::string{}) == "unauthorized",
                "nothing is answered before the password");
        const auto admitted = ask(client, kept,
                                  Json{{"id", 3},
                                       {"method", "session.authenticate"},
                                       {"params", {{"password", "secret"}}}});
        require(admitted.contains("result") && admitted["result"].value("authenticated", false),
                "the password admits it");
        const auto echoed = ask(client, kept,
                                Json{{"id", 4},
                                     {"method", "test.echo"},
                                     {"params", {{"said", std::string(300, 'x')}}}});
        require(echoed["result"].value("echo", std::string{}) == std::string(300, 'x'),
                "a request is answered, at any length");

        // A ping is answered with its payload.
        send_bytes(client, client_frame(0x9, "are you there"));
        const auto pong = server_frame(client, kept);
        require(pong && pong->opcode == 0xA && pong->payload == "are you there",
                "a ping is answered");

        // A message in two parts is one request.
        const std::string whole =
            Json{{"id", 5}, {"method", "test.echo"}, {"params", {{"said", "parts"}}}}.dump();
        auto first = client_frame(0x1, whole.substr(0, 10));
        first[0] = static_cast<char>(0x01);
        send_bytes(client, first + client_frame(0x0, whole.substr(10)));
        const auto joined = server_frame(client, kept);
        require(joined &&
                    Json::parse(joined->payload)["result"].value("echo", std::string{}) == "parts",
                "a message in parts is one request");
        ::close(client);
    }
    {
        // A wrong password: answered, then closed -- a guesser pays a
        // reconnect per guess, as on TCP.
        const auto client = connect_to(port);
        std::string kept;
        require(handshake(client, kept).starts_with("HTTP/1.1 101"), "taken");
        const auto wrong = ask(client, kept,
                               Json{{"id", 1},
                                    {"method", "session.authenticate"},
                                    {"params", {{"password", "guess"}}}});
        require(wrong.contains("error"), "a wrong password is refused");
        const auto closed = server_frame(client, kept);
        require(closed && closed->opcode == 0x8, "and the WebSocket closed");
        ::close(client);
    }
    {
        // A client frame must be masked: refused with a close.
        const auto client = connect_to(port);
        std::string kept;
        require(handshake(client, kept).starts_with("HTTP/1.1 101"), "taken");
        send_bytes(client, client_frame(0x1, "{}", false));
        const auto closed = server_frame(client, kept);
        require(closed && closed->opcode == 0x8 && closed->payload.size() == 2U &&
                    static_cast<std::uint8_t>(closed->payload[0]) == 0x03 &&
                    static_cast<std::uint8_t>(closed->payload[1]) == 0xEA,
                "an unmasked frame is refused with 1002");
        ::close(client);
    }
    {
        // Not a WebSocket: a request at /protocol without the upgrade.
        const auto client = connect_to(port);
        send_bytes(client, "GET /protocol HTTP/1.1\r\nHost: x\r\n\r\n");
        std::string kept;
        require(receive(client, kept, 12) == "HTTP/1.1 400", "a plain GET of /protocol is refused");
        ::close(client);
    }

    (*streams)->stop();
    (*tcp)->stop();
    std::cout << "websocket tests passed\n";
    return EXIT_SUCCESS;
}
