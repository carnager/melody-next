// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/ratings.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace trackknife::metadata {
namespace {

constexpr std::array<std::uint8_t, 11> popularimeter_bytes{0,   13,  1,   54,  64, 118,
                                                           128, 186, 196, 242, 255};

[[nodiscard]] std::optional<double> number(const std::string_view text) {
    const std::string owned{text};
    char* end = nullptr;
    const auto value = std::strtod(owned.c_str(), &end);
    if (owned.empty() || end == owned.c_str() || !std::isfinite(value) || value < 0.0) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] unsigned clamped(const double value) {
    return static_cast<unsigned>(std::clamp(std::lround(value), 0L, 10L));
}

} // namespace

std::string fmps_rating_text(const unsigned rating) {
    std::array<char, 8> text{};
    std::snprintf(text.data(), text.size(), "%.1f",
                  static_cast<double>(std::min(rating, 10U)) / 10.0);
    return text.data();
}

std::optional<unsigned> rating_from_fmps(const std::string_view text) {
    const auto value = number(text);
    if (!value || *value > 1.0) {
        return std::nullopt;
    }
    return clamped(*value * 10.0);
}

std::uint8_t popularimeter_byte(const unsigned rating) {
    return popularimeter_bytes[std::min(rating, 10U)];
}

unsigned rating_from_popularimeter(const std::uint8_t byte) {
    const auto exact = std::ranges::find(popularimeter_bytes, byte);
    if (exact != popularimeter_bytes.end()) {
        return static_cast<unsigned>(exact - popularimeter_bytes.begin());
    }
    // Whole stars, as Windows reads them.
    if (byte < 32U) {
        return 2U;
    }
    if (byte < 96U) {
        return 4U;
    }
    if (byte < 160U) {
        return 6U;
    }
    if (byte < 224U) {
        return 8U;
    }
    return 10U;
}

std::optional<unsigned> rating_from_plain(const std::string_view text,
                                          const PlainRatingScale scale) {
    const auto value = number(text);
    if (!value) {
        return std::nullopt;
    }
    switch (scale) {
    case PlainRatingScale::off:
        return std::nullopt;
    case PlainRatingScale::five:
        return *value <= 5.0 ? std::optional{clamped(*value * 2.0)} : std::nullopt;
    case PlainRatingScale::ten:
        return *value <= 10.0 ? std::optional{clamped(*value)} : std::nullopt;
    case PlainRatingScale::hundred:
        return *value <= 100.0 ? std::optional{clamped(*value / 10.0)} : std::nullopt;
    }
    return std::nullopt;
}

std::string_view plain_rating_scale_name(const PlainRatingScale scale) {
    switch (scale) {
    case PlainRatingScale::off:
        return "off";
    case PlainRatingScale::five:
        return "5";
    case PlainRatingScale::ten:
        return "10";
    case PlainRatingScale::hundred:
        return "100";
    }
    return "off";
}

std::optional<PlainRatingScale> plain_rating_scale_named(const std::string_view name) {
    for (const auto scale : {PlainRatingScale::off, PlainRatingScale::five, PlainRatingScale::ten,
                             PlainRatingScale::hundred}) {
        if (plain_rating_scale_name(scale) == name) {
            return scale;
        }
    }
    return std::nullopt;
}

} // namespace trackknife::metadata
