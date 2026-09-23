// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/token.hpp"

#include <sys/random.h>

#include <array>
#include <cerrno>
#include <cstring>

namespace trackknife::engine {
namespace {

[[nodiscard]] core::Result<std::string> generate() {
    std::array<unsigned char, 32> bytes{};
    std::size_t filled = 0;
    while (filled < bytes.size()) {
        const auto got = ::getrandom(bytes.data() + filled, bytes.size() - filled, 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(
                core::Error{.code = core::ErrorCode::io,
                            .message = "could not gather randomness",
                            .context = {{.key = "errno", .value = std::strerror(errno)}}});
        }
        filled += static_cast<std::size_t>(got);
    }
    constexpr std::string_view digits{"0123456789abcdef"};
    std::string text;
    text.reserve(bytes.size() * 2U);
    for (const auto byte : bytes) {
        text.push_back(digits[byte >> 4U]);
        text.push_back(digits[byte & 0x0FU]);
    }
    return text;
}

} // namespace

} // namespace trackknife::engine

namespace trackknife::engine {

bool same_token(const std::string_view offered, const std::string_view expected) {
    unsigned char difference = offered.size() == expected.size() ? 0U : 1U;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto left = index < offered.size() ? static_cast<unsigned char>(offered[index]) : 0U;
        difference = static_cast<unsigned char>(
            difference | (left ^ static_cast<unsigned char>(expected[index])));
    }
    return difference == 0U;
}

core::Result<std::string> random_token() { return generate(); }

} // namespace trackknife::engine
