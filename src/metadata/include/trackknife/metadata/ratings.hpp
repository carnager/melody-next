// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace trackknife::metadata {

// ADR-0237 stage 2: ratings as they are kept in files, against melody's 0-10
// scale (0 unrated; stars are only how it is drawn).
//
// FMPS_RATING is the freedesktop rating, 0.0-1.0: the rating over ten, one
// decimal. Written in every format; in ID3v2 and MP4 under the spelling the
// specification gives, FMPS_Rating.
inline constexpr std::string_view fmps_rating_field = "FMPS_RATING";
inline constexpr std::string_view fmps_rating_spelling = "FMPS_Rating";
[[nodiscard]] std::string fmps_rating_text(unsigned rating);
[[nodiscard]] std::optional<unsigned> rating_from_fmps(std::string_view text);

// POPM, the ID3v2 popularimeter: one byte, 0-255, whose meaning the
// specification leaves open. Written as Windows Media Player's owner, which
// Windows and foobar2000 read, with its whole-star values and MediaMonkey's
// and MusicBee's half stars between -- so every level survives there:
//   rating  1  2  3  4   5   6   7   8   9  10
//   byte   13  1 54 64 118 128 186 196 242 255
// Read back exactly; any other byte by the ranges every player shares.
inline constexpr std::string_view popularimeter_owner = "Windows Media Player 9 Series";
[[nodiscard]] std::uint8_t popularimeter_byte(unsigned rating);
[[nodiscard]] unsigned rating_from_popularimeter(std::uint8_t byte);

// A plain RATING tag has no agreed scale; which one the files use is the
// user's to say. `off` does not read it.
enum class PlainRatingScale : std::uint8_t { off, five, ten, hundred };
[[nodiscard]] std::optional<unsigned> rating_from_plain(std::string_view text,
                                                        PlainRatingScale scale);
[[nodiscard]] std::string_view plain_rating_scale_name(PlainRatingScale scale);
[[nodiscard]] std::optional<PlainRatingScale> plain_rating_scale_named(std::string_view name);

// ADR-0245: a second copy of a track's rating, as its plain 0-10 number, in
// a tag the user names -- apart from FMPS_RATING and POPM, which other
// players rewrite, so the rating survives them. Unrated removes it.
inline constexpr std::string_view default_rating_backup_tag = "TRACKKNIFE_RATING";
// Why `name` cannot hold the copy -- empty, FMPS_RATING itself, or not a
// name every format can carry (plain ASCII, no '=') -- or nothing.
[[nodiscard]] std::optional<std::string> rating_backup_tag_problem(std::string_view name);
// Whether `name` is an official tag other players show and use, such as
// COMMENT or TITLE: the copy would replace what it holds.
[[nodiscard]] bool official_tag_name(std::string_view name);

} // namespace trackknife::metadata
