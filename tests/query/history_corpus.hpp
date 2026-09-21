// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <string_view>

namespace history_corpus {
struct Case {
    std::string_view dialect;
    std::string_view input_context;
    std::string_view source;
    std::string_view expected;
    std::string_view rationale;
};
inline constexpr std::array cases{
    Case{"tkq-1", "Melody history capability; whole library", "HISTORY(playcount) EQUAL 0",
         "(history-playcount == 0)", "Unplayed is a numeric zero, not absent history."},
    Case{"tkq-1", "Melody history capability; whole library", "HISTORY(albumplaycount) EQUAL 0",
         "(history-albumplaycount == 0)",
         "An album is unplayed only when all its tracks are unplayed."},
    Case{"tkq-1", "Melody history capability; whole library", "HISTORY(lastplayed) MISSING",
         "(history-lastplayed < 0)", "No qualified play has no timestamp."},
    Case{"tkq-1", "Melody history capability; query-start clock",
         "HISTORY(albumdayssinceplayed) GREATER 180",
         "((history-albumdayssinceplayed >= 0) AND (history-albumdayssinceplayed > 180))",
         "Age is in complete UTC 24-hour days, not calendar months."},
    Case{"tkq-1", "Melody history capability; file tags", "playcount EQUAL 3",
         "((playcount >= 3) AND (playcount <= 3))",
         "Bare names retain their prior file-tag semantics."},
};
} // namespace history_corpus
