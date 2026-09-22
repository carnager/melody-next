// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <string_view>

namespace search_preset_corpus {
struct Case {
    std::string_view dialect;
    std::string_view input_context;
    std::string_view source;
    std::string_view expected;
    std::string_view rationale;
};
inline constexpr std::array cases{
    Case{"tkq-1", "year", "2001", "date EQUAL 2001",
         "Year prefix also matches full release dates."},
    Case{"tkq-1", "decade", "1990", "date GREATER 1989 AND date LESS 2000",
         "Both decade boundaries are inclusive."},
    Case{"tkq-1", "track-rating", "8", "rating GREATER 7", "At least includes the chosen rating."},
    Case{"tkq-1", "recently-played", "30", "HISTORY(dayssinceplayed) LESS 30",
         "Never-played tracks have no age and do not match."},
    Case{"tkq-1", "artist", "A \"quote\" AND B", "artist HAS \"A \"\"quote\"\" AND B\"",
         "Input is data, never extra query syntax."},
    Case{"tkq-1", "codec", "flac", "codec IS \"flac\"",
         "Exact codec name, not an incomplete lossless-codec classification."},
    Case{"tkq-1", "missing-album-gain", "", "REPLAYGAIN_ALBUM_GAIN MISSING",
         "Tests cached tags, not sidecar scan status."},
};
} // namespace search_preset_corpus
