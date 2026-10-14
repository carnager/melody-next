// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/track_description.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

// What list.describe tells of a track comes back as it was sent -- its
// tags, technicals, ratings, and (ADR-0283) when it came into the library.
void aDescriptionRoundTrips() {
    trackknife::persistence::LibraryTrackSnapshot track;
    track.raw_path = "/music/one.flac";
    track.facts.fields["title"] = {{"One", "one"}};
    track.facts.duration_ms = 61'000;
    track.facts.codec = "flac";
    track.facts.sample_rate = 44'100;
    track.facts.bits = 16;
    track.facts.channels = 2;
    track.facts.rating = 8;
    track.facts.album_rating = -1;
    track.facts.added = 1'700'000'000;
    track.facts.album_added = 1'700'086'400;
    const auto sent = trackknife::engine::describe_track(track);
    const auto read = trackknife::engine::described_track(sent, track.raw_path);
    CHECK(read.has_value());
    if (!read) {
        return;
    }
    CHECK(read->facts.fields.at("title").front().first == "One");
    CHECK(read->facts.duration_ms == 61'000 && read->facts.codec == "flac");
    CHECK(read->facts.rating == 8 && read->facts.album_rating == -1);
    CHECK(read->facts.added == 1'700'000'000 && read->facts.album_added == 1'700'086'400);
    // An older engine's description has no dates: not known.
    auto older = sent;
    older.erase("added");
    older.erase("album_added");
    const auto old_read = trackknife::engine::described_track(older, track.raw_path);
    CHECK(old_read && old_read->facts.added == 0 && old_read->facts.album_added == 0);
}

} // namespace

int main() {
    aDescriptionRoundTrips();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
