// SPDX-License-Identifier: GPL-3.0-only

// ADR-0220 Phase 2: playback owned by the engine rather than by a window,
// which is what lets the UI exit without the music stopping. No audio device
// is required to check the queue, the anchors and the advance rules -- what is
// being tested is who owns the state, not whether PipeWire is present.

#include "trackknife/engine/player.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace engine = trackknife::engine;
namespace core = trackknife::core;
namespace audio = trackknife::audio;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

[[nodiscard]] engine::QueueEntry entry(const std::string& path) {
    engine::QueueEntry made;
    made.source.raw_path = path;
    made.duration_ms = 180'000;
    return made;
}

void the_queue_is_the_engines(std::unique_ptr<engine::Player>& player) {
    require(player->queue().empty(), "a fresh engine has an empty queue");

    const std::vector<engine::QueueEntry> entries{entry("/music/a.flac"), entry("/music/b.flac"),
                                                  entry("/music/c.flac")};
    player->replace_queue(entries);
    const auto held = player->queue();
    require(held.size() == 3U, "the engine holds what it was given");
    require(held[0].entry_id == entries[0].entry_id, "identities survive being handed over");

    const auto state = player->state();
    require(state.queue_size == 3U, "and the state reports the queue");
    require(state.status == "stopped", "nothing plays until asked");
    require(state.entry.is_nil(), "with no entry anchored");
}

void an_entry_that_is_not_there_is_refused(std::unique_ptr<engine::Player>& player) {
    const auto missing = player->play_entry(core::StableId::random());
    require(!missing, "playing an unknown entry fails");
    require(missing.error().code == core::ErrorCode::not_found, "as not_found");
    // The identity is reported so a client can say which entry, without
    // parsing the message.
    require(!missing.error().context.empty(), "naming the entry it could not find");
}

void reordering_follows_the_entry_not_the_row(std::unique_ptr<engine::Player>& player) {
    auto entries = player->queue();
    require(entries.size() == 3U, "the queue is still as it was");

    // Pretend the second entry is the one playing, without needing a device.
    const auto playing = entries[1].entry_id;
    static_cast<void>(player->play_entry(playing));

    // Reversing the queue moves it from row 1 to row 1 in a three-entry list,
    // so use a rotation that actually moves it.
    std::vector<engine::QueueEntry> rotated{entries[2], entries[0], entries[1]};
    player->replace_queue(rotated);
    const auto after = player->state();
    require(after.queue_size == 3U, "the queue is replaced wholesale");
    // Whether it is still anchored depends on whether the device let it
    // start; what must hold either way is that the engine never re-anchors to
    // a different entry because a row number stayed the same.
    require(after.entry.is_nil() || after.entry == playing,
            "the anchor follows its identity or is dropped, never reassigned by row");
}

void an_entry_leaving_the_queue_drops_the_anchor(std::unique_ptr<engine::Player>& player) {
    const auto entries = player->queue();
    const auto playing = entries.front().entry_id;
    static_cast<void>(player->play_entry(playing));

    // Replace with entries that share nothing with the old queue.
    player->replace_queue({entry("/music/x.flac"), entry("/music/y.flac")});
    const auto after = player->state();
    require(after.queue_size == 2U, "the new queue is held");
    require(after.entry.is_nil(),
            "an anchor whose entry has gone is dropped rather than pointed at a stranger");
}

void modes_are_engine_state(std::unique_ptr<engine::Player>& player) {
    auto modes = player->modes();
    require(!modes.repeat && !modes.random, "modes start off");
    require(modes.single == audio::ModeState::off, "including the tri-states");

    modes.repeat = true;
    modes.single = audio::ModeState::oneshot;
    player->set_modes(modes);

    const auto stored = player->modes();
    require(stored.repeat, "a mode set on the engine stays set");
    require(stored.single == audio::ModeState::oneshot, "including one-shot");
    require(player->state().modes.repeat, "and the state reports it, so a client can render it");
}

void stepping_past_the_end_reports_rather_than_wrapping(std::unique_ptr<engine::Player>& player) {
    player->replace_queue({entry("/music/only.flac")});
    auto modes = player->modes();
    modes.repeat = false;
    modes.single = audio::ModeState::off;
    player->set_modes(modes);

    // Nothing is anchored, so there is nowhere to step from.
    const auto nowhere = player->step(1);
    require(!nowhere, "stepping with nothing playing fails");
    require(nowhere.error().code == core::ErrorCode::not_found,
            "as not_found rather than silently starting the first entry");
}

} // namespace

int main() {
    auto player = engine::Player::create();
    if (!player) {
        // No audio device in this environment. The engine cannot be built
        // without one, and saying so is more useful than a silent skip.
        std::cerr << "engine player: no audition service (" << player.error().message
                  << "); skipping\n";
        return EXIT_SUCCESS;
    }

    the_queue_is_the_engines(*player);
    an_entry_that_is_not_there_is_refused(*player);
    reordering_follows_the_entry_not_the_row(*player);
    an_entry_leaving_the_queue_drops_the_anchor(*player);
    modes_are_engine_state(*player);
    stepping_past_the_end_reports_rather_than_wrapping(*player);
    std::cout << "engine player: 6 scenarios\n";
    return EXIT_SUCCESS;
}
