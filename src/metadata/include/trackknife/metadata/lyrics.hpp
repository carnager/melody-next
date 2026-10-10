// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::metadata {

// ADR-0276: one line of lyrics, with the moment it is sung when the lyrics
// are synced.
struct LyricsLine {
    std::optional<std::int64_t> time_ms;
    std::string text;

    friend bool operator==(const LyricsLine&, const LyricsLine&) = default;
};

struct Lyrics {
    // Where they came from: "sidecar", "sylt", "tag:LYRICS", "tag:UNSYNCEDLYRICS".
    std::string source;
    // Synced: every line has a time, and the lines are in time order.
    bool synced{false};
    std::vector<LyricsLine> lines;

    friend bool operator==(const Lyrics&, const Lyrics&) = default;
};

// Parses lyrics text. Text with LRC timings ([mm:ss], [mm:ss.xx]) is synced:
// a line with several timings is one line per timing, `[offset:]` moves them
// all, word timings (<mm:ss.xx>) and header tags ([ar:], [ti:]) are dropped,
// and lines without a timing are left out. Text without timings is plain,
// line by line. Pure; `source` is left empty.
[[nodiscard]] Lyrics parse_lyrics(std::string_view text);

// The sidecar beside an audio file, if there is one: the same name with an
// `.lrc` extension, compared case-insensitively.
[[nodiscard]] std::optional<std::string> lyrics_sidecar_path(const std::string& raw_audio_path);

// The lyrics of one file: the first synced lyrics among the `.lrc` beside
// it, ID3 SYLT, the LYRICS tag and the UNSYNCEDLYRICS tag, or else the first
// plain ones; none, nullopt. A file TagLib cannot read may still have a
// sidecar. Runs on a worker, never the UI thread.
[[nodiscard]] core::Result<std::optional<Lyrics>>
read_lyrics(const std::string& raw_path, const core::CancellationToken& cancellation = {});

} // namespace trackknife::metadata
