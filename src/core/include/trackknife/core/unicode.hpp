// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace trackknife::core {

// Compares valid UTF-8 code point by code point using locale-independent simple
// lowercase mappings. This intentionally performs neither normalization nor
// multi-code-point full case folding.
[[nodiscard]] Result<bool> unicodeSimpleCaseEqual(std::string_view left, std::string_view right);

// Counts decoded Unicode scalar values, not UTF-8 bytes, grapheme clusters, or
// display cells. Invalid UTF-8 is rejected.
[[nodiscard]] Result<std::size_t> unicodeCodePointCount(std::string_view text);

// Returns the byte offset of a zero-based code-point boundary. An index beyond
// the end returns text.size(). The entire input is validated before returning.
[[nodiscard]] Result<std::size_t> unicodeByteOffset(std::string_view text,
                                                    std::size_t code_point_index);

[[nodiscard]] Result<std::string> unicodeSimpleLower(std::string_view text);
[[nodiscard]] Result<std::string> unicodeSimpleUpper(std::string_view text);
[[nodiscard]] Result<std::string> unicodeEncodeCodePoint(std::uint32_t code_point);

// Whether any letter is written right to left -- Hebrew, Arabic, Syriac,
// Thaana, and the rest of bidi classes R and AL. Invalid UTF-8 is not.
[[nodiscard]] bool unicodeHasRightToLeft(std::string_view text);

} // namespace trackknife::core
