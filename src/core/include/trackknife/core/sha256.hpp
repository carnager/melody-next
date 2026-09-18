// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace trackknife::core {

// One-shot FIPS 180-4 SHA-256 over a byte string. Self-contained so that
// Qt-free modules can derive stable content identities without pulling in a
// media or crypto dependency.
[[nodiscard]] std::array<std::uint8_t, 32> sha256(std::string_view bytes);

// The digest as 64 lowercase hexadecimal characters.
[[nodiscard]] std::string sha256_hex(std::string_view bytes);

} // namespace trackknife::core
