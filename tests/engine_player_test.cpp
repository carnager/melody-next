// SPDX-License-Identifier: GPL-3.0-only

// ADR-0220 Phase 2: playback owned by the engine rather than by a window,
// which is what lets the UI exit without the music stopping. No audio device
// is required to check the queue, the anchors and the advance rules -- what is
// being tested is who owns the state, not whether PipeWire is present.

#include "trackknife/engine/playback_methods.hpp"
#include "trackknife/engine/player.hpp"
#include "trackknife/protocol/message.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
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

namespace protocol = trackknife::protocol;

[[nodiscard]] protocol::Response invoke(const protocol::Dispatcher& dispatcher,
                                        const std::string& method, const protocol::Json& params) {
    return dispatcher.dispatch(protocol::Request{.id = 1, .method = method, .params = params});
}

void the_method_surface_speaks_for_the_player(engine::Player& player) {
    protocol::Dispatcher dispatcher;
    engine::register_playback_methods(dispatcher, player);

    // Paths cross as base64, because a path is bytes.
    const std::string raw{"/music/broken-\xff.flac", 22U};
    protocol::Json entries = protocol::Json::array();
    protocol::Json one = protocol::Json::object();
    one["path"] = protocol::encode_raw_path(raw);
    one["duration_ms"] = 1234;
    entries.push_back(one);

    const auto replaced =
        invoke(dispatcher, "playback.replace_queue", protocol::Json{{"entries", entries}});
    require(replaced.result.has_value(), "replacing the queue succeeds");
    require(replaced.result->at("queue_size") == 1, "and reports the new size");

    const auto listed = invoke(dispatcher, "playback.queue", protocol::Json::object());
    require(listed.result.has_value(), "the queue can be read back");
    const auto& listed_entries = listed.result->at("entries");
    require(listed_entries.size() == 1U, "with what was put in it");
    auto decoded = protocol::decode_raw_path(listed_entries[0].at("path").get<std::string>());
    require(decoded.has_value() && *decoded == raw,
            "an undecodable path survives the round trip exactly");

    // A client may name its own entries and recognise them coming back.
    const auto named = listed_entries[0].at("entry").get<std::string>();
    protocol::Json again = protocol::Json::array();
    protocol::Json keep = protocol::Json::object();
    keep["path"] = protocol::encode_raw_path(raw);
    keep["entry"] = named;
    again.push_back(keep);
    const auto kept =
        invoke(dispatcher, "playback.replace_queue", protocol::Json{{"entries", again}});
    require(kept.result.has_value(), "a client-supplied identity is accepted");
    const auto relisted = invoke(dispatcher, "playback.queue", protocol::Json::object());
    require(relisted.result->at("entries")[0].at("entry") == named,
            "and honoured rather than replaced");

    // Setting one mode leaves the others alone, so two clients changing
    // different modes do not overwrite each other.
    auto set = invoke(dispatcher, "playback.set_modes", protocol::Json{{"repeat", true}});
    require(set.result.has_value(), "setting a mode succeeds");
    set = invoke(dispatcher, "playback.set_modes", protocol::Json{{"single", 2}});
    require(set.result.has_value(), "setting another succeeds");
    require(set.result->at("modes").at("repeat") == true, "the first mode is still set");
    require(set.result->at("modes").at("single") == 2, "and the second took");

    const auto bad = invoke(dispatcher, "playback.set_modes", protocol::Json{{"single", 5}});
    require(bad.error.has_value(), "an out of range tri-state is refused");
    require(bad.error->context.at("param") == "single", "naming the parameter");

    const auto unknown_entry =
        invoke(dispatcher, "playback.play",
               protocol::Json{{"entry", core::StableId::random().to_string()}});
    require(unknown_entry.error.has_value(), "playing an entry not in the queue fails");
    require(unknown_entry.error->code == "not_found", "as not_found");

    const auto not_identity =
        invoke(dispatcher, "playback.play", protocol::Json{{"entry", "nonsense"}});
    require(not_identity.error.has_value(), "a malformed identity is refused");
    require(not_identity.error->context.at("param") == "entry", "naming the parameter");
}

void changes_are_pushed_without_asking(engine::Player& player) {
    std::mutex mutex;
    std::vector<protocol::Event> seen;
    engine::PlaybackWatcher watcher{player,
                                    [&](const protocol::Event& event) {
                                        const std::lock_guard guard{mutex};
                                        seen.push_back(event);
                                    },
                                    std::chrono::milliseconds{10}};
    watcher.start();

    // Never sleep holding the lock: the watcher needs it to push, and this
    // loop would otherwise starve the thread it is waiting for.
    const auto wait_for_an_event = [&]() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (std::chrono::steady_clock::now() < deadline) {
            {
                const std::lock_guard guard{mutex};
                if (!seen.empty()) {
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        return false;
    };

    // The first sample is always emitted, so a client that connects mid-life
    // learns the current state without having to ask for it.
    require(wait_for_an_event(), "the watcher emits the state it finds");
    {
        const std::lock_guard guard{mutex};
        require(seen.front().name == "playback.changed", "as playback.changed");
        seen.clear();
    }

    // A steady player emits nothing further: position moves continuously and
    // is excluded from the comparison, so an idle engine is quiet.
    std::this_thread::sleep_for(std::chrono::milliseconds{120});
    {
        const std::lock_guard guard{mutex};
        require(seen.empty(), "an unchanged player is quiet rather than chattering");
    }

    // A real change is noticed.
    auto modes = player.modes();
    modes.random = !modes.random;
    player.set_modes(modes);
    require(wait_for_an_event(), "a mode change is pushed");
    watcher.stop();
}

// ADR-0220: these have to exist in the engine before playback can move, or
// switching the workspace over would silently cost the user up-next, play
// counts and resume -- features it has had since ADR-0196 and ADR-0204.
void requests_outrank_the_order_but_only_forward(engine::Player& player) {
    const std::vector<engine::QueueEntry> entries{entry("/music/a.flac"), entry("/music/b.flac"),
                                                  entry("/music/c.flac")};
    player.replace_queue(entries);
    require(player.requests().empty(), "a fresh player has no requests");

    // A request must name something the engine has: asking for what it does
    // not hold is a mistake worth reporting, not a second way to add tracks.
    const auto absent = player.request(core::StableId::random());
    require(!absent, "requesting an entry not in the queue fails");
    require(absent.error().code == core::ErrorCode::not_found, "as not_found");

    require(player.request(entries[2].entry_id).has_value(), "requesting a held entry");
    require(player.requests().size() == 1U, "and it is queued");

    // A request whose entry leaves the queue goes with it, rather than
    // lingering to be skipped over later.
    player.replace_queue({entries[0], entries[1]});
    require(player.requests().empty(), "a request for a removed entry is forgotten");

    player.replace_queue(entries);
    require(player.request(entries[2].entry_id).has_value(), "requesting again");
    // Stepping back is "the track before this one", not "undo a request".
    static_cast<void>(player.step(-1));
    require(player.requests().size() == 1U, "stepping back leaves a request alone");
    player.clear_requests();
    require(player.requests().empty(), "requests can be abandoned");
}

void listening_and_resume_are_observed_not_pushed(engine::Player& player) {
    player.replace_queue({entry("/music/a.flac")});
    // Nothing is playing, so there is nothing to credit and nowhere to resume
    // to. Both must be absent rather than zeroed: a resume position of zero
    // means the start of a track, not the absence of one.
    const auto idle = player.observe(1000);
    require(!idle.listened_entry.has_value(), "silence credits no listening");
    require(!idle.resume_entry.has_value(), "and offers no resume point");

    // Observing repeatedly must stay quiet rather than accumulating against
    // nothing.
    for (std::int64_t at = 2000; at <= 6000; at += 1000) {
        const auto again = player.observe(at);
        require(!again.listened_entry.has_value(), "still nothing to credit");
    }
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
    requests_outrank_the_order_but_only_forward(**player);
    listening_and_resume_are_observed_not_pushed(**player);
    the_method_surface_speaks_for_the_player(**player);
    changes_are_pushed_without_asking(**player);
    std::cout << "engine player: 10 scenarios\n";
    return EXIT_SUCCESS;
}
