// SPDX-License-Identifier: GPL-3.0-only

// ADR-0253: a list continues with its rule -- from the engine's own library,
// once the list would end, not with what is queued, kept across a restart.

#include "recording_audition.hpp"

#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/list_continuation.hpp"
#include "trackknife/engine/player.hpp"
#include "trackknife/engine/workspace.hpp"
#include "trackknife/protocol/message.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace core = trackknife::core;
namespace engine = trackknife::engine;
namespace protocol = trackknife::protocol;
using trackknife::testing::RecordingAudition;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

[[nodiscard]] std::filesystem::path materialize(const std::filesystem::path& fixtures,
                                                const std::string& name,
                                                const std::filesystem::path& target) {
    std::ifstream input{fixtures / (name + ".b64")};
    std::string base64((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::erase(base64, '\n');
    const auto decoded = protocol::decode_raw_path(base64);
    require(decoded.has_value(), "a fixture decodes");
    std::ofstream output{target, std::ios::binary};
    output.write(decoded->data(), static_cast<std::streamsize>(decoded->size()));
    return target;
}

} // namespace

int main(const int argc, char** argv) {
    require(argc == 2, "the fixtures folder is given");
    const std::filesystem::path fixtures{argv[1]};
    const auto directory = std::filesystem::temp_directory_path() /
                           ("trackknife-continuation-" + core::StableId::random().to_string());
    const auto music = directory / "music";
    std::filesystem::create_directories(music);
    std::vector<std::string> files;
    // Named as the library knows audio files: it indexes by extension.
    for (const auto& [name, extension] :
         {std::pair{"tagged-tone-flac", ".flac"}, std::pair{"tagged-tone-mp3", ".mp3"},
          std::pair{"tagged-tone-opus", ".opus"}, std::pair{"tagged-tone-vorbis", ".ogg"},
          std::pair{"tagged-tone-m4a", ".m4a"}}) {
        files.push_back(
            materialize(fixtures, name, music / (std::string{name} + extension)).string());
    }
    const auto database = directory / "engine.sqlite3";
    engine::LocalCatalogue catalogue{database};
    require(catalogue.prepare().has_value(), "a library");
    require(catalogue.add_root(music.string()).has_value(), "its folder is added");
    trackknife::persistence::LibraryScanProgress progress;
    require(catalogue.scan({}, progress).has_value(), "scanned");
    auto workspace = engine::Workspace::open(database);
    require(workspace.has_value(), "the engine's store");

    auto player = engine::Player::create_without_audio();
    RecordingAudition audition;
    require(player->set_output(&audition).has_value(), "the player plays on the test's output");
    const auto list = core::StableId::random().to_string();
    engine::QueueEntry first;
    first.source.raw_path = files.front();
    first.title = "First";
    player->replace_queue({first}, list);
    require(player->play_entry(first.entry_id).has_value(), "the list's only track plays");
    require(player->queue_list() == list, "the queue knows its list");
    require(player->ends_after_current(), "and the list ends after it");

    {
        engine::ListContinuation continuation{*workspace, catalogue, {}};
        require(continuation.continue_if_ending(*player) == 0U,
                "a list without a continuation ends");
        require(!continuation
                     .set(list, engine::ContinuationRule{.rule_id = "r",
                                                         .name = "Broken",
                                                         .query = "codec (((",
                                                         .group_by = {},
                                                         .groups = 0U,
                                                         .per_group = 0U,
                                                         .limit = 100U,
                                                         .shuffle = false})
                     .has_value(),
                "a rule that does not compile is refused");
        require(continuation
                    .set(list, engine::ContinuationRule{.rule_id = "r",
                                                        .name = "Everything",
                                                        .query = "codec PRESENT",
                                                        .group_by = {},
                                                        .groups = 0U,
                                                        .per_group = 0U,
                                                        .limit = 100U,
                                                        .shuffle = false})
                    .has_value(),
                "the list continues with a rule");

        const auto appended = continuation.continue_if_ending(*player);
        require(appended == files.size() - 1U, "everything the rule finds, less what is queued");
        const auto queue = player->queue();
        require(queue.size() == files.size(), "appended to the queue");
        std::set<std::string> paths;
        for (const auto& entry : queue) {
            paths.insert(entry.source.raw_path);
        }
        require(paths.size() == queue.size(), "nothing queued twice");
        require(queue.front().entry_id == first.entry_id, "what plays keeps its place");
        require(!queue.back().title.empty(), "an added track is named from the library");
        require(!player->ends_after_current(), "the list goes on now");
        require(continuation.continue_if_ending(*player) == 0U, "and is not continued again");

        // Played to its new end: the rule has nothing left that is not
        // queued, so nothing is added and the list ends.
        require(player->play_entry(queue.back().entry_id).has_value(), "the last added plays");
        require(player->ends_after_current(), "the list would end after it");
        require(continuation.continue_if_ending(*player) == 0U,
                "nothing is added that is queued already");

        // Repeat never ends a list.
        auto modes = player->state().modes;
        modes.repeat = true;
        player->set_modes(modes);
        require(!player->ends_after_current(), "a repeated list does not end");
        modes.repeat = false;
        player->set_modes(modes);
    }

    // What played lately is not added -- unless nothing else is left.
    {
        const auto listened = [&workspace](const std::string& path) {
            trackknife::persistence::ListItem item;
            item.source = trackknife::persistence::ListSource::local;
            item.source_reference = path;
            auto revision = core::observe_local_source_revision(path);
            require(revision.has_value(), "the file is there");
            item.source_revision = *revision;
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
            const auto recorded =
                workspace->record_local_listen(item, core::StableId::random(), now);
            if (!recorded) {
                std::cerr << recorded.error().message << '\n';
            }
            require(recorded.has_value(), "a listen is recorded");
        };
        listened(files[1]);
        listened(files[2]);
        engine::ListContinuation continuation{*workspace, catalogue, {}};
        const auto replay = [&player, &files, &list] {
            engine::QueueEntry only;
            only.source.raw_path = files.front();
            player->replace_queue({only}, list);
            require(player->play_entry(only.entry_id).has_value(), "the list plays again");
            return only;
        };
        replay();
        require(continuation.continue_if_ending(*player) == 2U,
                "only what did not play lately is added");
        std::set<std::string> added;
        for (const auto& entry : player->queue()) {
            added.insert(entry.source.raw_path);
        }
        require(!added.contains(files[1]) && !added.contains(files[2]),
                "what played lately is left out");
        listened(files[3]);
        listened(files[4]);
        replay();
        require(continuation.continue_if_ending(*player) == 4U,
                "with everything played lately, the rule alone decides");
    }

    // Kept by the engine: another start finds it.
    {
        engine::ListContinuation restarted{*workspace, catalogue, {}};
        const auto kept = restarted.all();
        require(kept.size() == 1U && kept.contains(list) && kept.at(list).name == "Everything",
                "the continuation is kept across a restart");
        require(restarted.set(list, std::nullopt).has_value(), "and ended");
        require(restarted.all().empty(), "ended");
    }
    {
        engine::ListContinuation again{*workspace, catalogue, {}};
        require(again.all().empty(), "an ended continuation stays ended");
    }

    // ADR-0258: a grouped rule appends its selection -- here the one group a
    // constant makes of the five files, whole, less what is queued; or one
    // track of it.
    {
        engine::ListContinuation continuation{*workspace, catalogue, {}};
        const auto album = core::StableId::random().to_string();
        const auto play_alone = [&player, &files, &album] {
            engine::QueueEntry only;
            only.source.raw_path = files.front();
            player->replace_queue({only}, album);
            require(player->play_entry(only.entry_id).has_value(), "the list plays");
        };
        const auto rule = [](const std::size_t per_group) {
            return engine::ContinuationRule{.rule_id = "shipped:random-album",
                                            .name = "Random album",
                                            .query = "ALL",
                                            .group_by = "one",
                                            .groups = 1U,
                                            .per_group = per_group,
                                            .limit = 100U,
                                            .shuffle = false};
        };
        require(continuation.set(album, rule(0U)).has_value(), "a grouped rule is taken");
        play_alone();
        require(continuation.continue_if_ending(*player) == files.size() - 1U,
                "the whole group is appended, less what is queued");
        std::vector<std::string> order;
        for (const auto& entry : player->queue()) {
            order.push_back(entry.source.raw_path);
        }
        require(std::set<std::string>(order.begin(), order.end()).size() == files.size(),
                "nothing queued twice");
        require(continuation.all().at(album).group_by == "one" &&
                    continuation.all().at(album).groups == 1U,
                "the grouping is kept");
        {
            engine::ListContinuation restarted{*workspace, catalogue, {}};
            require(restarted.all().at(album).group_by == "one", "and kept across a restart");
        }
        require(continuation.set(album, rule(1U)).has_value(), "one track a group");
        play_alone();
        require(continuation.continue_if_ending(*player) == 1U, "one track of the group");
        require(continuation.set(album, std::nullopt).has_value(), "ended");
    }

    std::filesystem::remove_all(directory);
    return EXIT_SUCCESS;
}
