// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/unicode.hpp"

#include <utf8proc.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace trackknife::core {
namespace {

struct DecodedCodePoint {
    utf8proc_int32_t value{};
    std::size_t bytes{};
};

[[nodiscard]] Result<DecodedCodePoint> decode(const std::string_view text,
                                              const std::size_t offset) {
    utf8proc_int32_t code_point{};
    const auto remaining = text.size() - offset;
    const auto decoded =
        utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t*>(text.data() + offset),
                         static_cast<utf8proc_ssize_t>(remaining), &code_point);
    if (decoded <= 0) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "invalid UTF-8 in Unicode comparison", {}});
    }
    return DecodedCodePoint{code_point, static_cast<std::size_t>(decoded)};
}

[[nodiscard]] utf8proc_int32_t compatibilityLower(const utf8proc_int32_t code_point) noexcept {
    // The current public foobar2000 SDK helper applies a one-code-point lowercase
    // mapping only within the BMP. Preserve that observable compatibility shape;
    // utf8proc supplies the independently implemented Unicode mapping table.
    return code_point < 0x10000 ? utf8proc_tolower(code_point) : code_point;
}

[[nodiscard]] utf8proc_int32_t compatibilityUpper(const utf8proc_int32_t code_point) noexcept {
    return code_point < 0x10000 ? utf8proc_toupper(code_point) : code_point;
}

using CaseMapper = utf8proc_int32_t (*)(utf8proc_int32_t);

[[nodiscard]] Result<std::string> transformCase(const std::string_view text,
                                                const CaseMapper mapper) {
    std::string output;
    output.reserve(text.size());
    std::size_t offset = 0;
    while (offset < text.size()) {
        auto code_point = decode(text, offset);
        if (!code_point) {
            return std::unexpected(std::move(code_point.error()));
        }
        std::array<utf8proc_uint8_t, 4> encoded{};
        const auto encoded_size = utf8proc_encode_char(mapper(code_point->value), encoded.data());
        if (encoded_size <= 0) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "invalid Unicode case mapping", {}});
        }
        output.append(reinterpret_cast<const char*>(encoded.data()),
                      static_cast<std::size_t>(encoded_size));
        offset += code_point->bytes;
    }
    return output;
}

} // namespace

Result<bool> unicodeSimpleCaseEqual(const std::string_view left, const std::string_view right) {
    std::size_t left_offset = 0;
    std::size_t right_offset = 0;
    bool equal = true;
    while (left_offset < left.size() || right_offset < right.size()) {
        if (left_offset == left.size()) {
            auto right_code_point = decode(right, right_offset);
            if (!right_code_point) {
                return std::unexpected(std::move(right_code_point.error()));
            }
            right_offset += right_code_point->bytes;
            equal = false;
            continue;
        }
        if (right_offset == right.size()) {
            auto left_code_point = decode(left, left_offset);
            if (!left_code_point) {
                return std::unexpected(std::move(left_code_point.error()));
            }
            left_offset += left_code_point->bytes;
            equal = false;
            continue;
        }

        auto left_code_point = decode(left, left_offset);
        if (!left_code_point) {
            return std::unexpected(std::move(left_code_point.error()));
        }
        auto right_code_point = decode(right, right_offset);
        if (!right_code_point) {
            return std::unexpected(std::move(right_code_point.error()));
        }
        if (compatibilityLower(left_code_point->value) !=
            compatibilityLower(right_code_point->value)) {
            equal = false;
        }
        left_offset += left_code_point->bytes;
        right_offset += right_code_point->bytes;
    }
    return equal;
}

Result<std::size_t> unicodeCodePointCount(const std::string_view text) {
    std::size_t offset = 0;
    std::size_t count = 0;
    while (offset < text.size()) {
        auto code_point = decode(text, offset);
        if (!code_point) {
            return std::unexpected(std::move(code_point.error()));
        }
        offset += code_point->bytes;
        ++count;
    }
    return count;
}

Result<std::size_t> unicodeByteOffset(const std::string_view text,
                                      const std::size_t code_point_index) {
    std::size_t offset = 0;
    std::size_t count = 0;
    std::size_t selected_offset = code_point_index == 0U ? 0U : text.size();
    while (offset < text.size()) {
        auto code_point = decode(text, offset);
        if (!code_point) {
            return std::unexpected(std::move(code_point.error()));
        }
        offset += code_point->bytes;
        ++count;
        if (count == code_point_index) {
            selected_offset = offset;
        }
    }
    return selected_offset;
}

Result<std::string> unicodeSimpleLower(const std::string_view text) {
    return transformCase(text, compatibilityLower);
}

Result<std::string> unicodeSimpleUpper(const std::string_view text) {
    return transformCase(text, compatibilityUpper);
}

Result<std::string> unicodeEncodeCodePoint(const std::uint32_t code_point) {
    if (!utf8proc_codepoint_valid(static_cast<utf8proc_int32_t>(code_point))) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "invalid Unicode scalar value", {}});
    }
    std::array<utf8proc_uint8_t, 4> encoded{};
    const auto encoded_size =
        utf8proc_encode_char(static_cast<utf8proc_int32_t>(code_point), encoded.data());
    // At most four bytes, which utf8proc promises and the compiler cannot see.
    if (encoded_size <= 0 || static_cast<std::size_t>(encoded_size) > encoded.size()) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "invalid Unicode scalar value", {}});
    }
    return std::string{reinterpret_cast<const char*>(encoded.data()),
                       static_cast<std::size_t>(encoded_size)};
}

} // namespace trackknife::core
