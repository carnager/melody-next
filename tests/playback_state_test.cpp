// SPDX-License-Identifier: GPL-3.0-only

// ADR-0220 Phase 0: playback policy must be exercisable without constructing
// BenchMainWindow. This suite links only Trackknife::Audio -- no Qt, no
// widgets -- which is the property the phase is actually after. The behaviour
// itself is unchanged; it simply lives somewhere a headless engine can reach.

#include "trackknife/audio/album_grouping.hpp"
#include "trackknife/audio/playback_anchors.hpp"
#include "trackknife/audio/playback_selection.hpp"
#include "trackknife/audio/resume_checkpoint.hpp"
#include "trackknife/audio/playback_modes.hpp"
#include "trackknife/audio/track_source.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

void modes_default_to_off() {
    namespace audio = trackknife::audio;
    const audio::PlaybackModes modes;
    require(!modes.repeat && !modes.random && !modes.album_random,
            "toggles must default to off");
    require(!modes.single_active() && !modes.consume_active(),
            "tri-state modes must default to inactive");
}

void cycling_visits_off_on_oneshot_and_wraps() {
    namespace audio = trackknife::audio;
    auto state = audio::ModeState::off;
    state = audio::next_mode_state(state);
    require(state == audio::ModeState::on, "off must cycle to on");
    state = audio::next_mode_state(state);
    require(state == audio::ModeState::oneshot, "on must cycle to one-shot");
    state = audio::next_mode_state(state);
    require(state == audio::ModeState::off, "one-shot must cycle back to off");
}

void one_shot_modes_expire_once_and_report_the_change() {
    namespace audio = trackknife::audio;
    audio::PlaybackModes modes;

    // Off and on are stable: expiring them is a no-op, and reporting no change
    // is what keeps the workspace from writing settings on every track.
    require(!modes.expire_single(), "expiring an off mode must report no change");
    modes.single = audio::ModeState::on;
    require(!modes.expire_single(), "a plain on mode must survive a track");
    require(modes.single == audio::ModeState::on, "expiry must not disturb on");

    modes.single = audio::ModeState::oneshot;
    require(modes.expire_single(), "a one-shot mode must report its expiry");
    require(modes.single == audio::ModeState::off, "a one-shot mode must revert to off");
    require(!modes.expire_single(), "expiry must not repeat");

    modes.consume = audio::ModeState::oneshot;
    require(modes.expire_consume(), "consume must expire independently of single");
    require(modes.consume == audio::ModeState::off, "consume must revert to off");
    require(modes.single == audio::ModeState::off, "expiring one mode must not touch the other");
}

void persisted_values_are_clamped_rather_than_trusted() {
    namespace audio = trackknife::audio;
    require(audio::mode_state_from_int(0) == audio::ModeState::off, "0 is off");
    require(audio::mode_state_from_int(1) == audio::ModeState::on, "1 is on");
    require(audio::mode_state_from_int(2) == audio::ModeState::oneshot, "2 is one-shot");
    // A settings file is user-writable and may hold anything.
    require(audio::mode_state_from_int(3) == audio::ModeState::off, "out of range means off");
    require(audio::mode_state_from_int(-1) == audio::ModeState::off, "negative means off");
    require(audio::mode_state_from_int(99'999) == audio::ModeState::off, "far out of range is off");
}

void active_predicates_cover_both_live_states() {
    namespace audio = trackknife::audio;
    audio::PlaybackModes modes;
    modes.single = audio::ModeState::on;
    modes.consume = audio::ModeState::oneshot;
    require(modes.single_active(), "on counts as active");
    require(modes.consume_active(), "one-shot counts as active");
    modes.single = audio::ModeState::off;
    require(!modes.single_active(), "off does not count as active");
}

void a_track_source_reports_emptiness_and_compares_by_value() {
    namespace audio = trackknife::audio;
    namespace formats = trackknife::formats;

    const audio::TrackSource nothing;
    require(nothing.empty(), "a default source means nothing is playing");

    audio::TrackSource file;
    file.raw_path = "/music/track.flac";
    require(!file.empty(), "a source with a path is not empty");

    // Raw OS bytes, not assumed to be UTF-8.
    audio::TrackSource invalid_utf8;
    invalid_utf8.raw_path = std::string{"/music/broken-\xff.flac", 22U};
    require(!invalid_utf8.empty(), "an undecodable path is still a path");

    // The selection and segment are part of identity: the same file, a
    // different subsong, is a different source.
    auto subsong = file;
    subsong.selection.subsong_index = 2;
    require(!(file == subsong), "a different subsong is a different source");

    auto ranged = file;
    ranged.segment = formats::SampleRange{.start_sample = 0, .end_sample = 44'100};
    require(!(file == ranged), "a different span is a different source");
    require(ranged == ranged, "a source equals itself");
}

void anchors_describe_position_without_rows() {
    namespace audio = trackknife::audio;
    namespace core = trackknife::core;

    audio::PlaybackAnchors anchors;
    require(!anchors.playing(), "default anchors mean nothing is playing");

    // A current entry alone is not playback: without a document there is no
    // list to resolve it against.
    anchors.current = core::StableId::random();
    require(!anchors.playing(), "an entry without a document is not playback");
    anchors.document = core::StableId::random();
    require(anchors.playing(), "an entry in a document is playback");

    // The two in-flight anchors are abandoned as a unit, which is the
    // invariant forget_transition exists to hold.
    anchors.queued = core::StableId::random();
    anchors.requested = core::StableId::random();
    anchors.request_return = core::StableId::random();
    const auto return_point = anchors.request_return;
    anchors.forget_transition();
    require(anchors.queued.is_nil() && anchors.requested.is_nil(),
            "forgetting a transition must drop both halves");
    require(anchors.request_return == return_point,
            "a transition is not the return point; forgetting one must not drop the other");
    require(anchors.playing(), "forgetting a transition must not stop playback");

    anchors.source.raw_path = "/music/track.flac";
    anchors.clear();
    require(!anchors.playing() && anchors.source.empty() && anchors.request_return.is_nil(),
            "clearing must leave nothing behind");
}

void anchors_compare_by_value() {
    namespace audio = trackknife::audio;
    namespace core = trackknife::core;
    audio::PlaybackAnchors left;
    left.document = core::StableId::random();
    left.current = core::StableId::random();
    auto right = left;
    require(left == right, "a copy equals its source");
    right.current = core::StableId::random();
    require(!(left == right), "a different current entry is a different position");
}

// A list with no Qt in it, which is the point.
class FakeList final : public trackknife::audio::PlaybackList {
  public:
    explicit FakeList(std::vector<trackknife::core::StableId> entries)
        : entries_(std::move(entries)) {}

    [[nodiscard]] int row_count() const override {
        return static_cast<int>(entries_.size());
    }
    [[nodiscard]] int row_of_entry(const trackknife::core::StableId& entry,
                                   const int hint_row) const override {
        if (hint_row >= 0 && hint_row < row_count() &&
            entries_[static_cast<std::size_t>(hint_row)] == entry) {
            return hint_row;
        }
        for (std::size_t index = 0; index < entries_.size(); ++index) {
            if (entries_[index] == entry) {
                return static_cast<int>(index);
            }
        }
        return -1;
    }
    [[nodiscard]] trackknife::audio::TrackSource source_at(const int row) const override {
        trackknife::audio::TrackSource source;
        source.raw_path = "/music/" + std::to_string(row) + ".flac";
        return source;
    }

    void drop(const int row) {
        entries_.erase(entries_.begin() + row);
    }

  private:
    std::vector<trackknife::core::StableId> entries_;
};

struct Fixture {
    std::vector<trackknife::core::StableId> ids;
    FakeList list;
    trackknife::audio::PlaybackAnchors anchors;
    trackknife::audio::PlaybackModes modes;
    trackknife::audio::PlaybackOrder order{7U};

    explicit Fixture(const int count)
        : ids(make_ids(count)), list(ids) {
        anchors.document = trackknife::core::StableId::random();
        anchors.current = ids.front();
        anchors.source.raw_path = "/music/0.flac";
        order.reset(count, 0, false);
    }

    static std::vector<trackknife::core::StableId> make_ids(const int count) {
        std::vector<trackknife::core::StableId> ids;
        for (int index = 0; index < count; ++index) {
            ids.push_back(trackknife::core::StableId::random());
        }
        return ids;
    }
};

void advancing_walks_the_list_and_stops_at_the_end() {
    namespace audio = trackknife::audio;
    Fixture fixture{3};
    const audio::RequestQueueState idle;

    const auto next = audio::adjacent_playback_row(fixture.list, fixture.anchors, fixture.modes,
                                                   fixture.order, idle, 1, 0);
    require(next && next->row == 1, "advancing must land on the following row");
    require(next->source.raw_path == "/music/1.flac", "the choice carries that row's source");

    // Walk to the end; without repeat there is nothing after the last row.
    fixture.order.reset(3, 2, false);
    fixture.anchors.current = fixture.ids[2];
    const auto past_end = audio::adjacent_playback_row(fixture.list, fixture.anchors, fixture.modes,
                                                       fixture.order, idle, 1, 2);
    require(!past_end, "the end of a list without repeat stops playback");
}

void an_entry_that_left_the_list_stops_playback() {
    namespace audio = trackknife::audio;
    Fixture fixture{3};
    fixture.list.drop(0);
    const auto choice = audio::adjacent_playback_row(fixture.list, fixture.anchors, fixture.modes,
                                                     fixture.order, {}, 1, 0);
    require(!choice, "an identity that outlived its row must not resolve to a guess");
}

void a_request_return_point_outranks_the_order() {
    namespace audio = trackknife::audio;
    Fixture fixture{4};
    fixture.anchors.request_return = fixture.ids[3];
    const audio::RequestQueueState serving{.active = true, .pending_empty = false};

    const auto forward = audio::adjacent_playback_row(fixture.list, fixture.anchors, fixture.modes,
                                                      fixture.order, serving, 1, 0);
    require(forward && forward->row == 3, "a served request returns where the list was left");

    // Backwards never uses the return point.
    fixture.order.reset(4, 2, false);
    fixture.anchors.current = fixture.ids[2];
    const auto backward = audio::adjacent_playback_row(fixture.list, fixture.anchors, fixture.modes,
                                                       fixture.order, serving, -1, 2);
    require(backward && backward->row == 1, "going back ignores the return point");

    // Nor does it when no request is being served.
    const auto quiet = audio::adjacent_playback_row(fixture.list, fixture.anchors, fixture.modes,
                                                    fixture.order, {}, 1, 2);
    require(quiet && quiet->row == 3, "without an active request the order decides");
}

void consume_refuses_to_land_back_on_the_playing_row() {
    namespace audio = trackknife::audio;
    Fixture fixture{1};
    fixture.modes.repeat = true;
    fixture.modes.consume = audio::ModeState::on;
    // A single-row list with repeat would otherwise return row 0 forever, but
    // consume is about to remove it.
    const auto choice = audio::adjacent_playback_row(fixture.list, fixture.anchors, fixture.modes,
                                                     fixture.order, {}, 1, 0);
    require(!choice, "consume must not replay the row it is about to drop");
}

void single_stops_unless_repeat_turns_it_into_a_loop() {
    namespace audio = trackknife::audio;
    Fixture fixture{3};
    const audio::RequestQueueState idle;

    fixture.modes.single = audio::ModeState::on;
    require(!audio::automatic_playback_row(fixture.list, fixture.anchors, fixture.modes,
                                           fixture.order, idle, 0),
            "single stops after the current track");

    fixture.modes.repeat = true;
    const auto looped = audio::automatic_playback_row(fixture.list, fixture.anchors, fixture.modes,
                                                      fixture.order, idle, 0);
    require(looped && looped->row == 0, "single with repeat repeats this track");

    // Consume outranks the loop: repeating a row about to be dropped is
    // incoherent.
    fixture.modes.consume = audio::ModeState::on;
    require(!audio::automatic_playback_row(fixture.list, fixture.anchors, fixture.modes,
                                           fixture.order, idle, 0),
            "consume outranks single+repeat");
    fixture.modes.consume = audio::ModeState::off;

    // So does anything waiting in the request queue: a request is an explicit
    // ask and single+repeat must not starve it.
    require(!audio::automatic_playback_row(fixture.list, fixture.anchors, fixture.modes,
                                           fixture.order,
                                           {.active = false, .pending_empty = false}, 0),
            "a pending request outranks single+repeat");
    require(!audio::automatic_playback_row(fixture.list, fixture.anchors, fixture.modes,
                                           fixture.order,
                                           {.active = true, .pending_empty = true}, 0),
            "an active request outranks single+repeat");
}

void nothing_playing_chooses_nothing() {
    namespace audio = trackknife::audio;
    Fixture fixture{3};
    fixture.anchors.source = {};
    require(!audio::adjacent_playback_row(fixture.list, fixture.anchors, fixture.modes,
                                          fixture.order, {}, 1, 0),
            "with no source there is nothing to advance from");
}

trackknife::audio::LocalAuditionSnapshot playing_snapshot() {
    namespace audio = trackknife::audio;
    namespace formats = trackknife::formats;
    audio::LocalAuditionSnapshot snapshot;
    snapshot.state = audio::LocalAuditionState::playing;
    snapshot.source_revision = trackknife::core::LocalSourceRevision{};
    formats::PcmFormat format;
    format.sample_rate = 44'100;
    snapshot.format = format;
    snapshot.position_sample = 0;
    return snapshot;
}

void only_live_measurable_playback_is_resumable() {
    namespace audio = trackknife::audio;
    auto snapshot = playing_snapshot();
    require(audio::resumable(snapshot), "ordinary playback is resumable");

    for (const auto state : {audio::LocalAuditionState::paused, audio::LocalAuditionState::buffering,
                             audio::LocalAuditionState::draining}) {
        snapshot.state = state;
        require(audio::resumable(snapshot), "every live state is resumable");
    }
    for (const auto state : {audio::LocalAuditionState::loading, audio::LocalAuditionState::empty,
                             audio::LocalAuditionState::ended, audio::LocalAuditionState::failed}) {
        snapshot.state = state;
        require(!audio::resumable(snapshot), "a state with no offset is not resumable");
    }

    // Without a revision a restore could seek into a file that changed.
    snapshot = playing_snapshot();
    snapshot.source_revision.reset();
    require(!audio::resumable(snapshot), "an unrevisioned source is not resumable");

    snapshot = playing_snapshot();
    snapshot.format.reset();
    require(!audio::resumable(snapshot), "an unknown format is not resumable");

    snapshot = playing_snapshot();
    snapshot.format->sample_rate = 0;
    require(!audio::resumable(snapshot), "a zero rate would divide by zero downstream");

    snapshot = playing_snapshot();
    snapshot.position_sample = -1;
    require(!audio::resumable(snapshot), "a negative offset is not a position");

    // At or past a segment end is a finished track, not a place to return to.
    snapshot = playing_snapshot();
    snapshot.position_sample = 1'000;
    snapshot.end_sample = 1'000;
    require(!audio::resumable(snapshot), "a position at the segment end is finished");
    snapshot.position_sample = 999;
    require(audio::resumable(snapshot), "a position inside the segment is resumable");
}

void resume_position_matches_the_direct_computation() {
    namespace audio = trackknife::audio;
    auto snapshot = playing_snapshot();

    require(audio::resume_position_ms(snapshot) == 0, "the start is zero");
    snapshot.position_sample = 44'100;
    require(audio::resume_position_ms(snapshot) == 1'000, "one second is a thousand milliseconds");
    snapshot.position_sample = 22'050;
    require(audio::resume_position_ms(snapshot) == 500, "half a second is five hundred");

    // Truncation, not rounding: 44 samples is 0.997ms.
    snapshot.position_sample = 44;
    require(audio::resume_position_ms(snapshot) == 0, "sub-millisecond offsets truncate to zero");

    // The split form is exactly the direct computation, just without forming
    // the large intermediate product. Check that across awkward remainders.
    for (const auto rate : {8'000, 44'100, 48'000, 96'000, 192'000}) {
        snapshot.format->sample_rate = rate;
        for (const auto samples : {0, 1, 7, 999, 44'099, 44'100, 123'457, 7'654'321}) {
            snapshot.position_sample = samples;
            const auto direct = static_cast<std::int64_t>(samples) * 1'000 / rate;
            require(audio::resume_position_ms(snapshot) == direct,
                    "the split form must equal the direct computation");
        }
    }

    // A long file: three hours at 192 kHz is still exact.
    snapshot.format->sample_rate = 192'000;
    snapshot.position_sample = 192'000LL * 60 * 60 * 3;
    require(audio::resume_position_ms(snapshot) == 3LL * 60 * 60 * 1'000,
            "a long offset stays exact");
}

trackknife::audio::AlbumRowKey album_row(std::string album_artist, std::string artist,
                                         std::string album, std::string date = "2001") {
    return {.album_artist = std::move(album_artist),
            .artist = std::move(artist),
            .album = std::move(album),
            .date = std::move(date)};
}

void album_grouping_follows_the_album_artist_and_keeps_list_order() {
    namespace audio = trackknife::audio;
    audio::AlbumGrouper grouper;
    require(grouper.admits(10), "an ordinary list may be grouped");

    // Rows 0 and 2 are one release, row 1 another; grouping must not disturb
    // the order within a group.
    require(grouper.add(album_row("Credit", "First", "A"), 0), "row 0");
    require(grouper.add(album_row("Credit", "First", "B"), 1), "row 1");
    require(grouper.add(album_row("Credit", "Second", "A"), 2), "row 2");
    const auto groups = grouper.take();
    require(groups.size() == 2U, "two distinct albums make two groups");
    require(groups[0] == std::vector<int>{0, 2}, "a group keeps its rows in list order");
    require(groups[1] == std::vector<int>{1}, "the second album holds its own row");
}

void a_track_artist_stands_in_for_a_missing_album_artist() {
    namespace audio = trackknife::audio;
    audio::AlbumGrouper grouper;
    // Tagged only per-track: the release must still group.
    require(grouper.add(album_row("", "Only Artist", "Record"), 0), "row 0");
    require(grouper.add(album_row("", "Only Artist", "Record"), 1), "row 1");
    // And a row that does carry the album artist joins the same release only
    // when that artist matches the fallback.
    require(grouper.add(album_row("Only Artist", "Guest", "Record"), 2), "row 2");
    const auto groups = grouper.take();
    require(groups.size() == 1U, "the album artist fallback must group the release");
    require(groups[0] == std::vector<int>{0, 1, 2}, "all three rows belong to it");
}

void untitled_rows_each_stand_alone() {
    namespace audio = trackknife::audio;
    audio::AlbumGrouper grouper;
    require(grouper.add(album_row("Artist", "Artist", ""), 0), "row 0");
    require(grouper.add(album_row("Artist", "Artist", ""), 1), "row 1");
    const auto groups = grouper.take();
    // Unrelated untagged files sharing an empty album name are not a release.
    require(groups.size() == 2U, "rows with no album must not be merged into one");
    require(groups[0] == std::vector<int>{0} && groups[1] == std::vector<int>{1},
            "each untitled row is its own group");
}

void the_date_separates_editions() {
    namespace audio = trackknife::audio;
    audio::AlbumGrouper grouper;
    require(grouper.add(album_row("Artist", "Artist", "Record", "1994"), 0), "row 0");
    require(grouper.add(album_row("Artist", "Artist", "Record", "2011"), 1), "row 1");
    const auto groups = grouper.take();
    require(groups.size() == 2U, "a reissue is a different release");
}

void grouping_is_refused_rather_than_truncated() {
    namespace audio = trackknife::audio;
    const audio::AlbumGroupingLimits tight{
        .max_row_key_bytes = 16U, .max_total_key_bytes = 64U, .max_rows = 4U};

    require(!audio::AlbumGrouper{tight}.admits(5), "a list beyond the row limit is refused");
    require(audio::AlbumGrouper{tight}.admits(4), "a list at the row limit is allowed");

    // One row carrying an enormous key is refused on its own.
    audio::AlbumGrouper single{tight};
    require(!single.add(album_row("", "", std::string(100U, 'x'), ""), 0),
            "a single oversized key is refused");

    // So is an accumulation of individually acceptable ones. A truncated album
    // order would silently play the tail of the list in the wrong order.
    audio::AlbumGrouper accumulating{tight};
    bool refused = false;
    for (int row = 0; row < 20 && !refused; ++row) {
        refused = !accumulating.add(album_row("", "", "album" + std::to_string(row), ""), row);
    }
    require(refused, "an accumulated budget overrun must be refused");
}

} // namespace

int main() {
    album_grouping_follows_the_album_artist_and_keeps_list_order();
    a_track_artist_stands_in_for_a_missing_album_artist();
    untitled_rows_each_stand_alone();
    the_date_separates_editions();
    grouping_is_refused_rather_than_truncated();
    only_live_measurable_playback_is_resumable();
    resume_position_matches_the_direct_computation();
    advancing_walks_the_list_and_stops_at_the_end();
    an_entry_that_left_the_list_stops_playback();
    a_request_return_point_outranks_the_order();
    consume_refuses_to_land_back_on_the_playing_row();
    single_stops_unless_repeat_turns_it_into_a_loop();
    nothing_playing_chooses_nothing();
    anchors_describe_position_without_rows();
    anchors_compare_by_value();
    a_track_source_reports_emptiness_and_compares_by_value();
    modes_default_to_off();
    cycling_visits_off_on_oneshot_and_wraps();
    one_shot_modes_expire_once_and_report_the_change();
    persisted_values_are_clamped_rather_than_trusted();
    active_predicates_cover_both_live_states();
    return EXIT_SUCCESS;
}
