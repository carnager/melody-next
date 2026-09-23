// SPDX-License-Identifier: GPL-3.0-only

// ADR-0220 Phase 2: playback owned by the engine rather than by a window,
// which is what lets the UI exit without the music stopping. No audio device
// is required to check the queue, the anchors and the advance rules -- what is
// being tested is who owns the state, not whether PipeWire is present.

#include "trackknife/audio/audition.hpp"
#include "trackknife/engine/playback_methods.hpp"
#include "trackknife/engine/playback_store.hpp"
#include "trackknife/engine/player.hpp"
#include "trackknife/engine/recorder.hpp"
#include "trackknife/engine/workspace.hpp"
#include "trackknife/protocol/message.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
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

// ADR-0139/0141: the decoder's own tags the engine can read for itself; a
// sidecar value or a CUE sheet's REM lines it cannot, so they travel with the
// entry. Losing them means a normalised library plays unnormalised through the
// engine and sounds different from the same track played locally.
void an_explicit_replay_gain_travels_with_the_entry(engine::Player& player) {
    protocol::Dispatcher dispatcher;
    engine::register_playback_methods(dispatcher, player);

    protocol::Json gain = protocol::Json::object();
    gain["track_gain_db"] = -7.5;
    gain["album_peak"] = 0.98;
    protocol::Json one = protocol::Json::object();
    one["path"] = protocol::encode_raw_path("/music/normalised.flac");
    one["replay_gain"] = gain;
    protocol::Json entries = protocol::Json::array();
    entries.push_back(one);

    const auto replaced =
        invoke(dispatcher, "playback.replace_queue", protocol::Json{{"entries", entries}});
    require(replaced.result.has_value(), "an entry carrying a gain is accepted");
    const auto held = player.queue();
    require(held.size() == 1U, "and lands in the queue");
    require(held[0].replay_gain.has_value(), "carrying its override");
    require(held[0].replay_gain->track_gain_db == -7.5, "with the gain it was sent");
    require(held[0].replay_gain->album_peak == 0.98, "and the peak");
    require(!held[0].replay_gain->album_gain_db.has_value(),
            "members that were not sent stay absent rather than becoming zero");

    const auto listed = invoke(dispatcher, "playback.queue", protocol::Json::object());
    require(listed.result.has_value(), "the queue reads back");
    require(listed.result->at("entries")[0].at("replay_gain").at("track_gain_db") == -7.5,
            "and reports the override, so a second client sees the same decision");

    // No override is not an override of nothing: the decoder's tags must still
    // apply, so an empty object is the same as sending none.
    protocol::Json bare = protocol::Json::object();
    bare["path"] = protocol::encode_raw_path("/music/plain.flac");
    bare["replay_gain"] = protocol::Json::object();
    protocol::Json second = protocol::Json::array();
    second.push_back(bare);
    const auto plain =
        invoke(dispatcher, "playback.replace_queue", protocol::Json{{"entries", second}});
    require(plain.result.has_value(), "an empty override is accepted");
    require(!player.queue()[0].replay_gain.has_value(), "and means the decoder's own tags apply");

    protocol::Json wrong = protocol::Json::object();
    wrong["path"] = protocol::encode_raw_path("/music/plain.flac");
    wrong["replay_gain"] = protocol::Json{{"track_gain_db", "loud"}};
    protocol::Json third = protocol::Json::array();
    third.push_back(wrong);
    const auto refused =
        invoke(dispatcher, "playback.replace_queue", protocol::Json{{"entries", third}});
    require(refused.error.has_value(), "a gain that is not a number is refused");

    player.replace_queue({});
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
    // "An unchanged player is quiet" needs a player that is actually
    // unchanged. An earlier scenario may have left a track draining, and its
    // transition to stopped is a real change the watcher is right to report --
    // so settle first rather than assert into a moving state.
    static_cast<void>(player.stop());
    const auto settled = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (player.state().status != "stopped" && std::chrono::steady_clock::now() < settled) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    require(player.state().status == "stopped", "the player must settle before it is watched");

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

// Decodes one of the repository's real audio fixtures, because the gapless
// bookkeeping only runs while something is actually playing -- a queue with
// nothing playing takes an early return, which is how the first version of
// this test passed against a deliberately broken lookahead.
[[nodiscard]] bool materialise(const std::filesystem::path& encoded,
                               const std::filesystem::path& destination) {
    std::ifstream input{encoded};
    if (!input) {
        return false;
    }
    std::string base64((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::erase(base64, '\n');
    auto decoded = trackknife::protocol::decode_raw_path(base64);
    if (!decoded) {
        return false;
    }
    std::ofstream output{destination, std::ios::binary};
    output.write(decoded->data(), static_cast<std::streamsize>(decoded->size()));
    return output.good();
}

// Gapless is the last feature the workspace has and the engine did not, and
// the one that would be noticed immediately: an album that gaps between its
// tracks is obviously broken in a way a missing play count is not.
//
// What is checked here is the bookkeeping rather than the audio -- whether a
// continuation is offered, withdrawn and recomputed at the right moments --
// because whether two buffers actually join is the audition service's job and
// is tested there.
// A track ends and the next one starts. Nothing in the engine did this: a
// gapless handover covers the case where the continuation was accepted, and
// everything else left the output stopped at the end of the track -- which
// from the outside is playback stalling on every track boundary.
//
// The two sources are deliberately different formats, so the continuation is
// refused and the ordinary advance is what has to run. Handing it two copies
// of one file would prove the gapless path again and nothing else.
void a_finished_track_is_followed_by_the_next(engine::Player& player,
                                              const std::filesystem::path& first,
                                              const std::filesystem::path& second) {
    auto modes = player.modes();
    modes = {};
    player.set_modes(modes);
    const std::vector<engine::QueueEntry> entries{entry(first.string()), entry(second.string())};
    player.replace_queue(entries);
    if (!player.play_entry(entries[0].entry_id)) {
        // No output in this environment; the queue rules are covered without
        // one elsewhere, and pretending to test an advance that cannot happen
        // would be worse than saying so.
        std::cerr << "engine player: could not start playback; skipping the advance\n";
        return;
    }

    // Sampled the way the engine samples itself. Nothing here waits for a
    // fixed duration: the fixture's length is not the contract.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
    bool followed = false;
    while (std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(player.advance_if_ended());
        if (player.state().entry == entries[1].entry_id) {
            followed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }
    require(followed, "a finished track is followed by the next one in the queue");

    // And exactly once. "Ended" persists until the next load, so an advance
    // that did not remember it had already acted would race through the queue
    // on every tick.
    for (int tick = 0; tick < 5; ++tick) {
        static_cast<void>(player.advance_if_ended());
    }
    require(player.state().entry == entries[1].entry_id, "sampling again does not advance past it");

    static_cast<void>(player.stop());
    player.replace_queue({});
}

// Offering a continuation is not the same as one being accepted, and until
// now nothing checked the difference: a refused offer looks exactly like
// gapless working right up to the moment the track ends.
void a_continuation_is_actually_armed(engine::Player& player, const std::filesystem::path& audio) {
    player.set_modes({});
    const std::vector<engine::QueueEntry> entries{entry(audio.string()), entry(audio.string())};

    // Arming has to happen while the first entry is still current, and the
    // fixture is under a second: on a loaded machine the track ends first and
    // the engine moves on, which fails this for a reason that has nothing to
    // do with gapless. Pausing is the fix, and the attempt is retried when the
    // pause lands too late -- an asynchronous pipeline cannot promise
    // otherwise, and a test that pretends it can is a flaky test.
    const auto paused_on_first = [&player, &entries] {
        for (int attempt = 0; attempt < 5; ++attempt) {
            player.replace_queue(entries);
            if (!player.play_entry(entries[0].entry_id)) {
                return false;
            }
            static_cast<void>(player.pause());
            const auto settle = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (std::chrono::steady_clock::now() < settle) {
                const auto state = player.state();
                if (state.entry != entries[0].entry_id) {
                    break; // The track got away; set it up again.
                }
                if (state.status == "paused") {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
        }
        return false;
    };
    if (!paused_on_first()) {
        std::cerr << "engine player: could not hold playback on the first entry; skipping the "
                     "gapless arming\n";
        player.replace_queue({});
        return;
    }

    // The engine cannot arm a continuation before it knows the format it has
    // to match, so this is sampled the way a running engine samples itself
    // rather than asserted immediately.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    bool armed = false;
    while (std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(player.advance_if_ended());
        // The snapshot, not the engine's memory: offering a continuation and
        // the audition service holding one are different facts, and only the
        // second is gapless actually working.
        if (player.state().gapless_entry == entries[1].entry_id && player.armed_continuation()) {
            armed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    require(armed, "the next entry is armed to follow without a gap");

    // A seek drops the audition's queued continuation. The engine has to
    // notice and offer it again, because believing its own memory means the
    // rest of that track plays with nothing armed -- gapless silently stops
    // working for every track the user seeks in.
    require(player.seek_ms(10).has_value(), "seeking within the track succeeds");
    bool rearmed = false;
    const auto seek_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (std::chrono::steady_clock::now() < seek_deadline) {
        static_cast<void>(player.advance_if_ended());
        if (player.state().entry != entries[0].entry_id) {
            break;
        }
        if (player.state().gapless_entry == entries[1].entry_id && player.armed_continuation()) {
            rearmed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    require(rearmed, "the continuation is offered again after a seek drops it");

    require(player.resume().has_value(), "resuming succeeds");

    // And the handover itself. advance_if_ended returns true only when it had
    // to start the next track the ordinary way, so a transition it never
    // reports is one the audition service made seamlessly -- which is the
    // difference between gapless and a quick restart, and the only way to tell
    // them apart without listening.
    bool started_by_hand = false;
    bool moved = false;
    const auto hand_over = std::chrono::steady_clock::now() + std::chrono::seconds{20};
    while (std::chrono::steady_clock::now() < hand_over) {
        started_by_hand = player.advance_if_ended() || started_by_hand;
        if (player.state().entry == entries[1].entry_id) {
            moved = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    require(moved, "the queue moves on to the second entry");
    require(!started_by_hand, "and it got there without an ordinary load -- that is the gap");

    static_cast<void>(player.stop());
    player.replace_queue({});
}

void a_selection_and_segment_survive_the_wire(engine::Player& player) {
    protocol::Dispatcher dispatcher;
    engine::register_playback_methods(dispatcher, player);

    protocol::Json one = protocol::Json::object();
    one["path"] = protocol::encode_raw_path("/music/album.flac");
    one["selection"] = protocol::Json{{"stream_index", 2}, {"subsong_index", 5}};
    one["segment"] = protocol::Json{{"start_sample", 441'000}, {"end_sample", 882'000}};
    protocol::Json entries = protocol::Json::array();
    entries.push_back(one);

    const auto replaced =
        invoke(dispatcher, "playback.replace_queue", protocol::Json{{"entries", entries}});
    require(replaced.result.has_value(), "an entry naming a segment is accepted");
    const auto held = player.queue();
    require(held.size() == 1U, "and lands in the queue");
    require(held[0].source.selection.stream_index == 2, "with its stream");
    require(held[0].source.selection.subsong_index == 5, "and its subsong");
    require(held[0].source.segment.has_value(), "and its segment");
    require(held[0].source.segment->start_sample == 441'000, "starting where it was told");
    require(held[0].source.segment->end_sample == 882'000, "and ending there");

    const auto listed = invoke(dispatcher, "playback.queue", protocol::Json::object());
    require(listed.result.has_value(), "the queue reads back");
    require(listed.result->at("entries")[0].at("segment").at("start_sample") == 441'000,
            "and reports the segment, so a second client sees the same track");

    player.replace_queue({});
}

// An explicit ask interrupts the list; afterwards playback returns to where
// the list was, rather than continuing from wherever the asked-for track
// happened to sit. The rules are shared with the window (ADR-0196); what the
// engine had to learn is to record the return point at all.
void a_request_returns_to_where_the_list_was(engine::Player& player,
                                             const std::filesystem::path& audio) {
    player.set_modes({});
    const std::vector<engine::QueueEntry> entries{entry(audio.string()), entry(audio.string()),
                                                  entry(audio.string()), entry(audio.string())};
    player.replace_queue(entries);

    // Stating a whole order, which is what a client owning the up-next panel
    // does. Refusing the lot on an unknown entry matters: a half-applied order
    // leaves nothing saying which half took.
    require(player.set_requests({entries[3].entry_id}).has_value(), "an order can be stated");
    require(player.requests().size() == 1U, "and is held");
    const auto refused = player.set_requests({core::StableId::random()});
    require(!refused.has_value(), "an entry the engine does not hold is refused");
    require(player.requests().size() == 1U, "and the order it had is left alone");

    if (!player.play_entry(entries[0].entry_id)) {
        std::cerr << "engine player: could not start playback; skipping the request return\n";
        player.clear_requests();
        player.replace_queue({});
        return;
    }
    // Paused throughout: what is under test is which row is chosen, and a
    // fixture shorter than a second would otherwise advance underneath it.
    require(player.pause().has_value(), "pausing succeeds");
    require(player.state().entry == entries[0].entry_id, "the list is on its first entry");

    require(player.step(1).has_value(), "next honours the ask");
    require(player.state().entry == entries[3].entry_id, "and plays what was asked for");
    require(player.pause().has_value(), "pausing succeeds");

    require(player.step(1).has_value(), "next again leaves the request");
    require(player.state().entry == entries[1].entry_id,
            "and returns to where the list was rather than continuing past the asked-for track");
    require(player.pause().has_value(), "pausing succeeds");

    // The return point is spent: ordinary playback carries on from there.
    require(player.step(1).has_value(), "next continues");
    require(player.state().entry == entries[2].entry_id, "in list order");

    static_cast<void>(player.stop());
    player.clear_requests();
    player.replace_queue({});
}

// A track asked for from outside the list is held apart from it: the list a
// client mirrors does not grow a row, the order does not reach it, and once
// played it returns to where the list was. It was once appended to the queue,
// and every window watching the engine showed it as a new list entry.
void an_ask_from_outside_the_list_stays_out_of_it(engine::Player& player,
                                                  const std::filesystem::path& audio) {
    player.set_modes({});
    const std::vector<engine::QueueEntry> list{entry(audio.string()), entry(audio.string()),
                                               entry(audio.string())};
    player.replace_queue(list);
    const auto ask = entry(audio.string());
    player.enqueue({ask});
    require(player.set_requests({ask.entry_id}).has_value(), "an ask held apart can be requested");
    require(player.queue().size() == 3U, "and the list does not grow");
    require(player.state().queue_size == 3U, "nor does the size a client renders");

    if (!player.play_entry(list[0].entry_id)) {
        std::cerr << "engine player: could not start playback; skipping the ask\n";
        player.clear_requests();
        player.replace_queue({});
        return;
    }
    require(player.pause().has_value(), "pausing succeeds");
    require(player.step(1).has_value(), "next plays the ask");
    require(player.state().entry == ask.entry_id, "which is what plays");
    require(player.pause().has_value(), "pausing succeeds");
    require(player.queue().size() == 3U, "still outside the list");
    require(player.step(1).has_value(), "next again");
    require(player.state().entry == list[1].entry_id, "returns to where the list was");
    require(player.pause().has_value(), "pausing succeeds");
    require(!player.set_requests({ask.entry_id}).has_value(),
            "and a played ask is let go rather than kept for ever");

    static_cast<void>(player.stop());
    player.replace_queue({});
}

// Consume removes what has been played. The engine decides it, because the
// engine owns the queue -- and it reports which entry went, so a client
// mirrors the same drop onto its list instead of deducing it from a mode that
// may already have expired.
void consume_drops_what_has_been_played(engine::Player& player,
                                        const std::filesystem::path& audio) {
    audio::PlaybackModes modes;
    modes.consume = audio::ModeState::on;
    player.set_modes(modes);
    const std::vector<engine::QueueEntry> entries{entry(audio.string()), entry(audio.string()),
                                                  entry(audio.string())};
    player.replace_queue(entries);
    if (!player.play_entry(entries[0].entry_id)) {
        std::cerr << "engine player: could not start playback; skipping consume\n";
        player.set_modes({});
        player.replace_queue({});
        return;
    }
    require(player.pause().has_value(), "pausing succeeds");
    require(player.queue().size() == 3U, "nothing is dropped before anything is played");
    require(player.state().consumed.is_nil(), "and nothing is reported as dropped");

    require(player.step(1).has_value(), "next moves on");
    require(player.pause().has_value(), "pausing succeeds");
    require(player.queue().size() == 2U, "the entry that was left is dropped");
    require(player.state().consumed == entries[0].entry_id, "and is named, so a client can mirror");
    require(player.state().entry == entries[1].entry_id, "while the new one plays");
    require(player.queue()[0].entry_id == entries[1].entry_id, "the queue closes up");

    // One-shot fires once and reverts, and the mode the client sees is the
    // engine's rather than whatever it last sent.
    modes.consume = audio::ModeState::oneshot;
    player.set_modes(modes);
    require(player.step(1).has_value(), "next moves on again");
    require(player.pause().has_value(), "pausing succeeds");
    require(player.queue().size() == 1U, "the one-shot consumed its track");
    require(player.modes().consume == audio::ModeState::off, "and then reverted");

    require(player.step(1).has_value() == false, "nothing follows the last entry");
    require(player.queue().size() == 1U, "and a refused step drops nothing");

    static_cast<void>(player.stop());
    player.set_modes({});
    player.replace_queue({});
}

// Consume drops every track it finishes, however playback left it: by a
// gapless handover (two tracks of one format arm one), and at the end of the
// queue where nothing follows. Both once kept the entry -- a list played in
// consume mode was never emptied.
void consume_takes_every_finished_track(engine::Player& player,
                                        const std::filesystem::path& audio) {
    audio::PlaybackModes modes;
    modes.consume = audio::ModeState::on;
    player.set_modes(modes);
    const std::vector<engine::QueueEntry> entries{entry(audio.string()), entry(audio.string())};
    player.replace_queue(entries);
    if (!player.play_entry(entries[0].entry_id)) {
        std::cerr << "engine player: could not start playback; skipping consume to the end\n";
        player.set_modes({});
        player.replace_queue({});
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
    bool first_dropped = false;
    while (std::chrono::steady_clock::now() < deadline && !player.queue().empty()) {
        static_cast<void>(player.advance_if_ended());
        if (player.state().consumed == entries[0].entry_id) {
            first_dropped = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }
    require(first_dropped, "the track playback moved on from is consumed");
    require(player.queue().empty(), "and so is the last one, with nothing after it");
    require(player.state().consumed == entries[1].entry_id, "and is named for clients");

    static_cast<void>(player.stop());
    player.set_modes({});
    player.replace_queue({});
}

// Album shuffle: albums in a random order, tracks within an album in list
// order. The engine's queue is paths, so the release an entry belongs to
// travels with it -- a tagging decision the client has already made, and one
// the engine cannot redo from a filename.
void album_shuffle_keeps_albums_together(engine::Player& player,
                                         const std::filesystem::path& audio) {
    // Interleaved on purpose: album A track 1, album B track 1, album C track
    // 1, album A track 2 ... Laid out album by album, plain list order would
    // already play each album as a unit and this would pass with album
    // shuffle doing nothing -- which is exactly what it did when I wrote it
    // that way.
    std::vector<engine::QueueEntry> entries;
    for (int track = 0; track < 3; ++track) {
        for (int album = 0; album < 3; ++album) {
            auto made = entry(audio.string());
            made.group.album = "Album " + std::to_string(album);
            made.group.album_artist = "Artist " + std::to_string(album);
            entries.push_back(made);
        }
    }
    audio::PlaybackModes modes;
    modes.album_random = true;
    player.set_modes(modes);
    player.replace_queue(entries);
    if (!player.play_entry(entries[0].entry_id)) {
        std::cerr << "engine player: could not start playback; skipping album shuffle\n";
        player.set_modes({});
        player.replace_queue({});
        return;
    }
    require(player.pause().has_value(), "pausing succeeds");

    const auto album_of = [&entries](const core::StableId& id) {
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (entries[index].entry_id == id) {
                return static_cast<int>(index % 3);
            }
        }
        return -1;
    };
    const auto track_of = [&entries](const core::StableId& id) {
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (entries[index].entry_id == id) {
                return static_cast<int>(index / 3);
            }
        }
        return -1;
    };

    std::vector<core::StableId> played{player.state().entry};
    for (int step = 0; step < 8; ++step) {
        if (!player.step(1)) {
            break;
        }
        require(player.pause().has_value(), "pausing succeeds");
        played.push_back(player.state().entry);
    }
    require(played.size() == 9U, "every entry is reached exactly once");

    std::set<int> albums_seen;
    for (std::size_t index = 0; index < played.size(); index += 3) {
        const auto album = album_of(played[index]);
        require(album >= 0, "each played entry is one of the queued ones");
        require(albums_seen.insert(album).second, "an album is not returned to");
        for (int track = 0; track < 3; ++track) {
            require(album_of(played[index + static_cast<std::size_t>(track)]) == album,
                    "an album plays as a unit rather than interleaved with another");
            require(track_of(played[index + static_cast<std::size_t>(track)]) == track,
                    "and its tracks keep list order inside it");
        }
    }
    require(albums_seen.size() == 3U, "all three albums play");

    static_cast<void>(player.stop());
    player.set_modes({});
    player.replace_queue({});
}

// ADR-0220: the queue belongs to the engine, so it survives the engine. A
// window is a view of what the engine holds, and a view cannot be what brings
// the queue back -- if it were, the first client to connect after a restart
// would be the one deciding what the engine is playing.
void a_restarted_engine_comes_back_with_its_queue(const std::filesystem::path& directory,
                                                  const std::filesystem::path& audio) {
    const auto database = directory / "restart-workspace.sqlite3";
    std::error_code ignored;
    std::filesystem::remove(database, ignored);

    std::vector<core::StableId> queued;
    core::StableId playing;
    std::int64_t left_at = 0;
    {
        auto workspace = engine::Workspace::open(database);
        require(workspace.has_value(), "the workspace opens");
        auto player = engine::Player::create();
        require(player.has_value(), "a player is created");

        std::vector<engine::QueueEntry> entries{entry(audio.string()), entry(audio.string()),
                                                entry(audio.string())};
        entries[1].group.album = "Second";
        for (const auto& made : entries) {
            queued.push_back(made.entry_id);
        }
        audio::PlaybackModes modes;
        modes.repeat = true;
        modes.consume = audio::ModeState::oneshot;
        (*player)->set_modes(modes);
        (*player)->replace_queue(entries);
        require((*player)->request(entries[2].entry_id).has_value(), "an ask is held");

        engine::PlaybackStore store{**player, *workspace};
        require(!store.restore(), "an engine that has never run has nothing to restore");

        if ((*player)->play_entry(entries[1].entry_id)) {
            require((*player)->pause().has_value(), "pausing succeeds");
            // The load is asynchronous, and what is being stored includes what
            // the file looked like -- which the engine only knows once it has
            // opened it.
            const auto settle = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (std::chrono::steady_clock::now() < settle &&
                   (*player)->state().status != "paused") {
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
            if ((*player)->state().status == "paused") {
                playing = entries[1].entry_id;
                left_at = (*player)->state().position_ms;
            }
        }
        store.persist();
    }

    // A new process, with nothing but the database.
    auto workspace = engine::Workspace::open(database);
    require(workspace.has_value(), "the workspace reopens");
    auto player = engine::Player::create();
    require(player.has_value(), "a player is created");
    engine::PlaybackStore store{**player, *workspace};
    require(store.restore(), "the stored queue is found");

    const auto restored = (*player)->queue();
    require(restored.size() == 3U, "the queue comes back whole");
    for (std::size_t index = 0; index < restored.size(); ++index) {
        require(restored[index].entry_id == queued[index],
                "with its identities, so a client's rows still name the same entries");
    }
    require(restored[1].group.album == "Second", "and what each entry is, tags included");
    require((*player)->modes().repeat, "the modes come back");
    require((*player)->modes().consume == audio::ModeState::oneshot,
            "including a one-shot that had not fired");
    require((*player)->requests().size() == 1U, "and the asks that had not been played");

    if (!playing.is_nil()) {
        // Restoring is asynchronous too: the engine has to open the file
        // again before it can be paused inside it.
        const auto settle = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < settle && (*player)->state().status != "paused") {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        const auto state = (*player)->state();
        require(state.entry == playing, "playback is anchored where it was");
        require(state.status == "paused",
                "and paused rather than playing: coming back making noise unasked is a surprise");
        require(state.position_ms >= left_at - 1'000 && state.position_ms <= left_at + 1'000,
                "at roughly where it left off");
    }

    std::filesystem::remove(database, ignored);
}

void gapless_is_offered_and_recomputed(engine::Player& player, const std::filesystem::path& audio) {
    const std::vector<engine::QueueEntry> entries{entry(audio.string()), entry(audio.string()),
                                                  entry(audio.string())};
    player.replace_queue(entries);

    // Nothing is playing, so there is nothing to follow and the engine must
    // not be left holding a continuation from an earlier queue.
    const auto idle = player.observe(1000);
    require(!idle.listened_entry.has_value(), "nothing plays, nothing is credited");

    // Observing repeatedly must not consume the order: asking what comes next
    // is a question, and a lookahead that advanced would skip a track every
    // time the engine sampled itself.
    for (std::int64_t at = 2000; at <= 5000; at += 1000) {
        static_cast<void>(player.observe(at));
    }
    const auto after = player.state();
    require(after.queue_size == 3U, "sampling leaves the queue alone");
    require(after.entry.is_nil(), "and starts nothing on its own");

    // A mode change invalidates whatever was queued under the old one.
    auto modes = player.modes();
    modes.repeat = true;
    player.set_modes(modes);
    require(player.modes().repeat, "the mode took");
    require(player.state().queue_size == 3U, "and the queue is intact");

    // With something actually playing, the lookahead runs. Asking what comes
    // next must not consume the order: if it did, every observation would
    // advance it and the engine would skip a track each time it sampled
    // itself.
    player.replace_queue(entries);
    modes.repeat = false;
    // Random, because a shuffle is where repeated sampling could plausibly
    // disturb the traversal. It does not -- PlaybackOrder holds its draw until
    // advance() commits it -- and this asserts the property rather than
    // guarding a defence: a three entry shuffle must still reach all three
    // however often the engine samples itself in between.
    modes.random = true;
    player.set_modes(modes);
    if (player.play_entry(entries[0].entry_id).has_value()) {
        require(player.state().entry == entries[0].entry_id, "the first entry is playing");
        std::set<core::StableId> visited{entries[0].entry_id};
        for (int step = 0; step < 2; ++step) {
            // Sample repeatedly between tracks, as a running engine does.
            for (std::int64_t at = 0; at < 5; ++at) {
                static_cast<void>(player.observe(10'000 + step * 10'000 + at * 1000));
            }
            const auto moved = player.step(1);
            require(moved.has_value(),
                    "a three entry shuffle must reach every entry however often it is sampled");
            visited.insert(player.state().entry);
        }
        require(visited.size() == 3U, "a shuffle must visit each entry exactly once");
        static_cast<void>(player.stop());
    } else {
        std::cerr << "engine player: no audio output; gapless lookahead not exercised\n";
    }
    modes.random = false;
    player.set_modes(modes);

    // Emptying the queue must withdraw any continuation rather than leave the
    // engine holding a track that is no longer anywhere.
    player.replace_queue({});
    require(player.state().queue_size == 0U, "the queue empties");
    static_cast<void>(player.observe(6000));
    modes.repeat = false;
    player.set_modes(modes);
}

// ADR-0220: what the player accumulates has to reach a store, or the engine
// plays and remembers nothing. The player pulls rather than pushes so it can
// be tested without a database; this is the piece that joins them, and this
// checks the join rather than the accounting rules, which are tested in
// playback-state.
void the_recorder_drains_into_a_workspace(engine::Player& player,
                                          const std::filesystem::path& directory,
                                          const std::filesystem::path& audio) {
    auto workspace = engine::Workspace::open(directory / "workspace.sqlite3");
    require(workspace.has_value(), "the workspace must open");

    engine::Recorder recorder{player, *workspace, std::chrono::milliseconds{10}};
    recorder.start();

    player.replace_queue({entry(audio.string())});
    const auto started = player.play_entry(player.queue().front().entry_id);
    // Sampling an idle or playing engine must both be harmless; what is being
    // checked is that draining runs without disturbing playback or the store.
    std::this_thread::sleep_for(std::chrono::milliseconds{120});
    if (started) {
        require(player.state().queue_size == 1U, "draining leaves the queue alone");
    }
    recorder.stop();
    // Stopping twice is what shutdown does after an error path.
    recorder.stop();
    static_cast<void>(player.stop());
}

} // namespace

// ADR-0228: the player plays on whatever output it is given. A fake one here
// records what it is asked, which is all an output agent is from the
// player's side: the decisions stay in the player.
class RecordingAudition final : public audio::Audition {
  public:
    [[nodiscard]] audio::LocalAuditionSnapshot snapshot() const override {
        const std::lock_guard guard{mutex_};
        audio::LocalAuditionSnapshot current;
        current.state = state_;
        current.raw_path = loaded_;
        // Milliseconds as samples at 1 kHz: exact, and what the player's
        // arithmetic expects.
        current.format = trackknife::formats::PcmFormat{1000, 2, "stereo"};
        current.position_sample = position_ms_;
        current.replay_gain_mode = gain_mode_;
        return current;
    }
    [[nodiscard]] core::Result<void>
    load_selected_and_play(std::string raw_path, trackknife::formats::AudioSourceSelection,
                           std::optional<trackknife::formats::ReplayGainInfo>) override {
        const std::lock_guard guard{mutex_};
        loaded_ = std::move(raw_path);
        position_ms_ = 0;
        state_ = audio::LocalAuditionState::playing;
        return {};
    }
    [[nodiscard]] core::Result<void>
    load_selected_segment_and_play(std::string raw_path, trackknife::formats::AudioSourceSelection,
                                   trackknife::formats::SampleRange,
                                   std::optional<trackknife::formats::ReplayGainInfo>) override {
        return load_selected_and_play(std::move(raw_path), {}, {});
    }
    [[nodiscard]] core::Result<void>
    restore_paused(std::string raw_path, core::LocalSourceRevision,
                   trackknife::formats::AudioSourceSelection,
                   std::optional<trackknife::formats::SampleRange>, const std::int64_t position_ms,
                   std::optional<trackknife::formats::ReplayGainInfo>) override {
        const std::lock_guard guard{mutex_};
        loaded_ = std::move(raw_path);
        position_ms_ = position_ms;
        state_ = audio::LocalAuditionState::paused;
        return {};
    }
    [[nodiscard]] core::Result<void>
    queue_gapless_next_selected(std::string, trackknife::formats::AudioSourceSelection,
                                std::optional<trackknife::formats::ReplayGainInfo>,
                                std::uint64_t) override {
        return {};
    }
    [[nodiscard]] core::Result<void> queue_gapless_next_selected_segment(
        std::string, trackknife::formats::AudioSourceSelection, trackknife::formats::SampleRange,
        std::optional<trackknife::formats::ReplayGainInfo>, std::uint64_t) override {
        return {};
    }
    [[nodiscard]] core::Result<void> clear_gapless_next() override { return {}; }
    [[nodiscard]] core::Result<void> play() override {
        const std::lock_guard guard{mutex_};
        state_ = audio::LocalAuditionState::playing;
        return {};
    }
    [[nodiscard]] core::Result<void> pause() override {
        const std::lock_guard guard{mutex_};
        state_ = audio::LocalAuditionState::paused;
        return {};
    }
    [[nodiscard]] core::Result<void> stop() override {
        const std::lock_guard guard{mutex_};
        state_ = audio::LocalAuditionState::empty;
        loaded_.clear();
        return {};
    }
    [[nodiscard]] core::Result<void> seek_to_seconds(double) override { return {}; }
    [[nodiscard]] core::Result<void> set_volume_percent(int) override { return {}; }
    [[nodiscard]] core::Result<void>
    set_replay_gain_mode(const audio::ReplayGainMode mode) override {
        const std::lock_guard guard{mutex_};
        gain_mode_ = mode;
        return {};
    }
    [[nodiscard]] core::Result<void> set_replay_gain_preamps(audio::ReplayGainPreamps) override {
        return {};
    }
    [[nodiscard]] core::Result<void>
    set_buffer_config(audio::PlaybackBufferDurationConfig) override {
        return {};
    }
    [[nodiscard]] core::Result<void> refresh_output_devices() override { return {}; }
    [[nodiscard]] core::Result<void> set_output_target(std::optional<std::string>) override {
        return {};
    }

    void advance_to(const std::int64_t position_ms) {
        const std::lock_guard guard{mutex_};
        position_ms_ = position_ms;
    }

  private:
    mutable std::mutex mutex_;
    audio::LocalAuditionState state_{audio::LocalAuditionState::empty};
    std::string loaded_;
    std::int64_t position_ms_{0};
    audio::ReplayGainMode gain_mode_{audio::ReplayGainMode::off};
};

void the_player_plays_on_the_output_it_is_given() {
    auto player = engine::Player::create_without_audio();
    require(player->local_output() == nullptr, "a headless player has no audio of its own");
    const std::vector<engine::QueueEntry> entries{entry("/music/one.flac"),
                                                  entry("/music/two.flac")};
    player->replace_queue(entries);
    require(!player->play_entry(entries[0].entry_id).has_value(),
            "with no output chosen, nothing plays");
    require(player->queue().size() == 2U, "and the queue is kept");

    RecordingAudition bedroom;
    require(player->set_output(&bedroom).has_value(), "an output can be chosen");
    require(player->play_entry(entries[0].entry_id).has_value(), "and then it plays");
    require(bedroom.snapshot().raw_path == "/music/one.flac", "on that output");
    require(player->set_replay_gain_mode(audio::ReplayGainMode::album).has_value(),
            "settings reach it");
    bedroom.advance_to(12'000);

    // The music moves rooms: the old output stops, the new one takes up the
    // same track at the same place, still playing, at the same gain.
    RecordingAudition kitchen;
    require(player->set_output(&kitchen).has_value(), "moving to another output succeeds");
    require(bedroom.snapshot().state == audio::LocalAuditionState::empty, "the old one stops");
    const auto moved = kitchen.snapshot();
    require(moved.raw_path == "/music/one.flac", "the new one takes up the same track");
    require(moved.position_sample == 12'000, "at the same place");
    require(moved.state == audio::LocalAuditionState::playing, "still playing");
    require(moved.replay_gain_mode == audio::ReplayGainMode::album, "at the same gain");
    require(player->state().entry == entries[0].entry_id, "and the player still names it");

    require(player->set_output(nullptr).has_value(), "choosing no output succeeds");
    require(kitchen.snapshot().state == audio::LocalAuditionState::empty, "and silences it");
    require(player->current_output() == nullptr, "with nothing chosen");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: engine_player_test <audio-fixture-dir>\n";
        return EXIT_FAILURE;
    }
    const std::filesystem::path fixtures{argv[1]};
    const auto directory = std::filesystem::temp_directory_path() /
                           ("trackknife-engine-player-" + core::StableId::random().to_string());
    std::filesystem::create_directory(directory);
    const auto audio = directory / "track.flac";
    if (!materialise(fixtures / "rich-metadata-flac.b64", audio)) {
        std::cerr << "engine player: could not materialise the fixture\n";
        return EXIT_FAILURE;
    }

    const auto longer = directory / "long.flac";
    if (!materialise(fixtures / "rich-metadata-long-flac.b64", longer)) {
        std::cerr << "engine player: could not materialise the long fixture\n";
        return EXIT_FAILURE;
    }

    const auto other = directory / "second.opus";
    if (!materialise(fixtures / "tagged-tone-opus.b64", other)) {
        std::cerr << "engine player: could not materialise the second fixture\n";
        return EXIT_FAILURE;
    }

    // Needs no audio device, so it runs before the check for one.
    the_player_plays_on_the_output_it_is_given();

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
    gapless_is_offered_and_recomputed(**player, audio);
    a_finished_track_is_followed_by_the_next(**player, audio, other);
    a_continuation_is_actually_armed(**player, longer);
    a_selection_and_segment_survive_the_wire(**player);
    a_request_returns_to_where_the_list_was(**player, audio);
    an_ask_from_outside_the_list_stays_out_of_it(**player, audio);
    consume_drops_what_has_been_played(**player, audio);
    consume_takes_every_finished_track(**player, audio);
    album_shuffle_keeps_albums_together(**player, audio);
    a_restarted_engine_comes_back_with_its_queue(directory, audio);
    the_recorder_drains_into_a_workspace(**player, directory, audio);
    the_method_surface_speaks_for_the_player(**player);
    an_explicit_replay_gain_travels_with_the_entry(**player);
    changes_are_pushed_without_asking(**player);
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::cout << "engine player: 23 scenarios\n";
    return EXIT_SUCCESS;
}
