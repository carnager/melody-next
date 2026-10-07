// SPDX-License-Identifier: GPL-3.0-only
#include "trackknife/engine/websocket.hpp"

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <optional>

namespace trackknife::engine {
namespace {

// RFC 6455's constant, joined to the client's key.
constexpr std::string_view handshake_guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
// As a request line on the socket: the engine closes a connection whose line
// passes this, so a message larger is refused here alike.
constexpr std::size_t maximum_message = 1U << 20U;

enum Opcode : std::uint8_t {
    continuation = 0x0,
    text = 0x1,
    binary = 0x2,
    close = 0x8,
    ping = 0x9,
    pong = 0xA,
};

[[nodiscard]] bool send_all(const int descriptor, const std::string_view bytes) {
    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto sent =
            ::send(descriptor, bytes.data() + written, bytes.size() - written, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent <= 0) {
            return false;
        }
        written += static_cast<std::size_t>(sent);
    }
    return true;
}

// A frame from the server: never masked.
[[nodiscard]] std::string frame(const Opcode opcode, const std::string_view payload) {
    std::string out;
    out.push_back(static_cast<char>(0x80U | opcode));
    const auto size = payload.size();
    if (size < 126U) {
        out.push_back(static_cast<char>(size));
    } else if (size <= 0xFFFFU) {
        out.push_back(static_cast<char>(126));
        out.push_back(static_cast<char>((size >> 8U) & 0xFFU));
        out.push_back(static_cast<char>(size & 0xFFU));
    } else {
        out.push_back(static_cast<char>(127));
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(static_cast<char>((static_cast<std::uint64_t>(size) >> shift) & 0xFFU));
        }
    }
    out.append(payload);
    return out;
}

struct Frame {
    bool fin{false};
    Opcode opcode{text};
    std::string payload;
};

// The next whole frame in `buffer`, taken from it; none while it is not all
// there. A frame that breaks the rules -- unmasked, too large, a control
// frame in parts -- is a close code.
struct Parsed {
    std::optional<Frame> frame;
    std::uint16_t refused{0};
};

[[nodiscard]] Parsed take_frame(std::string& buffer) {
    if (buffer.size() < 2U) {
        return {};
    }
    const auto first = static_cast<std::uint8_t>(buffer[0]);
    const auto second = static_cast<std::uint8_t>(buffer[1]);
    const bool masked = (second & 0x80U) != 0U;
    std::uint64_t size = second & 0x7FU;
    std::size_t at = 2U;
    if (size == 126U) {
        if (buffer.size() < 4U) {
            return {};
        }
        size = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(buffer[2])) << 8U) |
               static_cast<std::uint8_t>(buffer[3]);
        at = 4U;
    } else if (size == 127U) {
        if (buffer.size() < 10U) {
            return {};
        }
        size = 0;
        for (std::size_t index = 2; index < 10; ++index) {
            size = (size << 8U) | static_cast<std::uint8_t>(buffer[index]);
        }
        at = 10U;
    }
    // A client masks every frame (RFC 6455, 5.1).
    if (!masked) {
        return {.frame = std::nullopt, .refused = 1002};
    }
    if (size > maximum_message) {
        return {.frame = std::nullopt, .refused = 1009};
    }
    const auto opcode = static_cast<Opcode>(first & 0x0FU);
    const bool fin = (first & 0x80U) != 0U;
    if ((opcode & 0x8U) != 0U && (!fin || size > 125U)) {
        return {.frame = std::nullopt, .refused = 1002};
    }
    if (buffer.size() < at + 4U + size) {
        return {};
    }
    std::array<std::uint8_t, 4> mask{};
    for (std::size_t index = 0; index < 4; ++index) {
        mask[index] = static_cast<std::uint8_t>(buffer[at + index]);
    }
    at += 4U;
    std::string payload = buffer.substr(at, static_cast<std::size_t>(size));
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] =
            static_cast<char>(static_cast<std::uint8_t>(payload[index]) ^ mask[index % 4U]);
    }
    buffer.erase(0, at + static_cast<std::size_t>(size));
    return {.frame = Frame{.fin = fin, .opcode = opcode, .payload = std::move(payload)},
            .refused = 0};
}

[[nodiscard]] std::string close_payload(const std::uint16_t code) {
    return std::string{static_cast<char>((code >> 8U) & 0xFFU), static_cast<char>(code & 0xFFU)};
}

} // namespace

std::string websocket_accept(const std::string_view key) {
    std::string joined{key};
    joined.append(handshake_guid);
    std::array<unsigned char, SHA_DIGEST_LENGTH> digest{};
    SHA1(reinterpret_cast<const unsigned char*>(joined.data()), joined.size(), digest.data());
    std::array<unsigned char, 4 * ((SHA_DIGEST_LENGTH + 2) / 3) + 1> encoded{};
    const auto length = EVP_EncodeBlock(encoded.data(), digest.data(), SHA_DIGEST_LENGTH);
    return std::string{reinterpret_cast<const char*>(encoded.data()),
                       static_cast<std::size_t>(length)};
}

void bridge_websocket(const int client, const int inner, std::string pending,
                      const std::atomic<bool>& running) {
    std::string from_client = std::move(pending);
    std::string from_inner;
    std::string message;
    bool in_message = false;
    std::uint16_t closing = 0;
    std::array<char, 16 * 1024> buffer{};
    const auto refuse = [&closing](const std::uint16_t code) { closing = code; };
    while (running.load() && closing == 0U) {
        // Whole frames first: some may have come with the handshake.
        while (closing == 0U) {
            auto parsed = take_frame(from_client);
            if (parsed.refused != 0U) {
                refuse(parsed.refused);
                break;
            }
            if (!parsed.frame) {
                break;
            }
            auto& frame = *parsed.frame;
            switch (frame.opcode) {
            case ping:
                if (!send_all(client, engine::frame(pong, frame.payload))) {
                    refuse(1001);
                }
                break;
            case pong:
                break;
            case close:
                refuse(1000);
                break;
            case text:
            case binary:
            case continuation: {
                if ((frame.opcode == continuation) != in_message) {
                    refuse(1002);
                    break;
                }
                message += frame.payload;
                in_message = !frame.fin;
                if (message.size() > maximum_message) {
                    refuse(1009);
                    break;
                }
                if (!frame.fin) {
                    break;
                }
                // One message, one line: a raw newline inside would be two.
                if (message.find('\n') != std::string::npos) {
                    refuse(1007);
                    break;
                }
                message.push_back('\n');
                if (!send_all(inner, message)) {
                    refuse(1011);
                }
                message.clear();
                break;
            }
            default:
                refuse(1002);
                break;
            }
        }
        if (closing != 0U) {
            break;
        }
        std::array<pollfd, 2> watched{pollfd{.fd = client, .events = POLLIN, .revents = 0},
                                      pollfd{.fd = inner, .events = POLLIN, .revents = 0}};
        // Woken now and then to see whether the engine is stopping.
        const auto ready = ::poll(watched.data(), watched.size(), 1000);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            refuse(1011);
            break;
        }
        if ((watched[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            const auto received = ::recv(client, buffer.data(), buffer.size(), 0);
            if (received <= 0) {
                break;
            }
            from_client.append(buffer.data(), static_cast<std::size_t>(received));
        }
        if ((watched[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            const auto received = ::recv(inner, buffer.data(), buffer.size(), 0);
            if (received <= 0) {
                refuse(1000);
                break;
            }
            from_inner.append(buffer.data(), static_cast<std::size_t>(received));
            for (auto end = from_inner.find('\n'); end != std::string::npos;
                 end = from_inner.find('\n')) {
                if (!send_all(client, frame(text, std::string_view{from_inner}.substr(0, end)))) {
                    refuse(1001);
                    break;
                }
                from_inner.erase(0, end + 1U);
            }
        }
    }
    if (closing != 0U) {
        static_cast<void>(send_all(client, frame(close, close_payload(closing))));
    }
    ::shutdown(inner, SHUT_RDWR);
    ::close(inner);
}

} // namespace trackknife::engine
