// SPDX-License-Identifier: GPL-3.0-only

#include "help.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <string_view>

namespace trackknife::cli {
namespace {

struct Entry final {
    // The words `help` finds it by.
    std::initializer_list<std::string_view> names;
    std::string_view group;
    // How it is written, for the overview's left column.
    std::string_view synopsis;
    std::string_view summary;
    // `help COMMAND`: what it does, in full, with examples.
    std::string_view detail;
};

constexpr std::array<std::string_view, 7> groups{
    "Playing", "The queue",        "The library", "Playlists", "Your music's files",
    "Sharing", "Engines and setup"};

const std::array<Entry, 27> entries{{
    {{"status"},
     "Playing",
     "status",
     "what plays, where, and on which engine",
     "  The playing track, its position, the volume, the queue and Up Next, and\n"
     "  which engine answered and which speakers it plays on.\n"
     "    melody-cli status\n"
     "    melody-cli --json status        the engine's own answer, for scripts\n"},
    {{"current"},
     "Playing",
     "current [--format EXPR]",
     "the playing track on one line",
     "  The playing track on one line; nothing when nothing plays -- for a status\n"
     "  bar. EXPR is Melody's formatting language (tkfmt-1), default\n"
     "  \"$if(%artist%,%artist% - )%title%\": every tag as %name%\n"
     "  (%replaygain_track_gain%...), $info(samplerate) $info(bitspersample)\n"
     "  $info(codec) $info(channels), %rating% %albumrating% (1-10), %length%\n"
     "  %path% %playback_state% %playback_time% %playback_remaining%. A field not\n"
     "  known is empty; literal ( ) , $ % are escaped with a backslash.\n"
     "    melody-cli current --format '%artist% - %title% [%playback_time%]'\n"},
    {{"play", "pause", "toggle", "stop", "next", "prev"},
     "Playing",
     "play | pause | toggle | stop | next | prev",
     "the transport: play resumes, next skips",
     "  play resumes (or starts the queue), pause pauses, toggle switches between\n"
     "  the two, stop stops, next and prev skip.\n"
     "  With words after them, play and next take something to play instead:\n"
     "  melody-cli help queue.\n"},
    {{"seek"},
     "Playing",
     "seek [+|-]SECONDS",
     "jump to a place, or forward and back",
     "    melody-cli seek 90               to 1:30\n"
     "    melody-cli seek +30              30 seconds forward\n"},
    {{"volume"},
     "Playing",
     "volume [[+|-]PERCENT]",
     "say or set the volume",
     "    melody-cli volume                say how loud\n"
     "    melody-cli volume 40\n"
     "    melody-cli volume -5\n"},
    {{"outputs", "output", "speakers"},
     "Playing",
     "outputs | output NAME",
     "list the speakers; play on others",
     "  outputs lists the speakers this engine can play on: its own, agents on\n"
     "  other computers, phones, UPnP devices. output NAME moves playback there.\n"
     "    melody-cli output livingroom\n"},
    {{"queue", "play", "add", "next", "random"},
     "The queue",
     "play | add | next | queue  album|track WORDS...",
     "find something and play it, or add it",
     "  play replaces the queue with it and plays it; add appends it to the\n"
     "  queue; next puts it first in Up Next; queue puts it last in Up Next.\n"
     "  Every word must appear in its artist, title, album or year:\n"
     "    melody-cli play album doors 1967\n"
     "    melody-cli next track riders on the storm\n"
     "  Or name one exactly by the key `albums --keys` or `tracks --keys` give:\n"
     "    melody-cli add album --key KEY\n"
     "  Or let the engine choose -- an album of a random album artist, or COUNT\n"
     "  random tracks (default 25):\n"
     "    melody-cli play random album\n"
     "    melody-cli play random tracks 50\n"},
    {{"list", "play"},
     "The queue",
     "play list NAME...",
     "play one of the engine's playlists",
     "  By its name, or every word found in one name.\n"
     "    melody-cli play list late night\n"},
    {{"albums", "tracks"},
     "The library",
     "albums | tracks [WORDS...]",
     "list albums or tracks, or those words find",
     "  One a line. --fields artist,album,title,date chooses what is shown,\n"
     "  --keys adds each one's key for `add --key`, --json the engine's answer.\n"
     "    melody-cli albums doors\n"
     "    melody-cli --keys --fields artist,title tracks blue\n"},
    {{"find", "query", "search"},
     "The library",
     "find QUERY [--format EXPR]",
     "tracks a query finds, formatted",
     "  QUERY is a Melody query; each track is formatted by the engine from its\n"
     "  whole library row, with the fields `current` has, bar playback_*.\n"
     "    melody-cli find 'genre IS jazz SORT BY %date%' --format '%date% %artist% - %title%'\n"},
    {{"latest"},
     "The library",
     "latest [COUNT] | latest --since WHEN [--folders]",
     "the albums added most recently",
     "  The COUNT albums added last (default 20), or every album added after\n"
     "  WHEN: Unix seconds, 2026-10-17, 2026-10-17T18:30, or a time ago -- 90m,\n"
     "  12h, 3d, 2w. Nothing new is no error. --folders prints each album's\n"
     "  folder instead of its name (with --json, a \"folder\" each).\n"
     "    melody-cli latest --since 1d --folders\n"},
    {{"update", "rescan"},
     "The library",
     "update [PATH...]",
     "rescan the library, or these folders and files",
     "  The engine rescans its music folders, or only these folders and files --\n"
     "  its own paths, relative ones taken from here: new files read, changed\n"
     "  ones re-read, gone ones dropped.\n"
     "    melody-cli update /srv/music/Incoming\n"},
    {{"revision"},
     "The library",
     "revision",
     "changes whenever the library does, for caches",
     "  A number that changes whenever the library does: a script keeps it and\n"
     "  asks again only when it changed.\n"},
    {{"lists", "playlists"},
     "Playlists",
     "lists | playlists [folder DIR|default [--style S]]",
     "saved playlists, and where they are kept",
     "  lists names the engine's saved playlists. playlists says where they are\n"
     "  kept: a folder of the engine's, each playlist file there one list.\n"
     "  playlists folder DIR keeps them in DIR (MPD's playlist_directory, say),\n"
     "  default in Playlists in the music folder; --style root|playlist|absolute\n"
     "  is how new files name their tracks.\n"},
    {{"rating", "rate", "albumrating"},
     "Your music's files",
     "rating | albumrating [0-10]",
     "say or set the playing track's rating",
     "  1-10 as Melody keeps it; 0 clears it, and is what an unrated one says.\n"
     "    melody-cli rating 8\n"
     "    melody-cli albumrating\n"},
    {{"replaygain"},
     "Your music's files",
     "replaygain album|track WORDS... [--track-gain] [--true-peak] [--sidecar]",
     "measure loudness and write ReplayGain",
     "  As Melody does: an album gets track and album gain, a track or\n"
     "  --track-gain only track gain. Found as play finds it (also --key KEY).\n"
     "  The engine holding the files measures and writes; --sidecar keeps the\n"
     "  values out of the files.\n"
     "    melody-cli replaygain album doors 1967 --true-peak\n"},
    {{"convert"},
     "Your music's files",
     "convert --to FOLDER [...] (--since WHEN | PATH...)",
     "convert tracks with Melody's converter",
     "  Tags and cover carried and checked, named by a naming layout or kept\n"
     "  below the music folder, ReplayGain measured afresh; what is there\n"
     "  already is passed over. Every option: melody-cli convert --help.\n"
     "    melody-cli convert --to ~/Opus --since 1d\n"},
    {{"cover"},
     "Your music's files",
     "cover FILE [SIZE]",
     "write the playing track's cover to FILE",
     "  SIZE pixels across at most, if given.\n"
     "    melody-cli cover /tmp/cover.jpg 300\n"},
    {{"love", "unlove", "fav", "unfav"},
     "Sharing",
     "love | unlove | fav | unfav",
     "Last.fm love, or Melody's Favorites",
     "  love and unlove mark the playing track on Last.fm; fav and unfav put it\n"
     "  into Melody's Favorites playlist or take it out.\n"},
    {{"together"},
     "Sharing",
     "together host|join|leave|status|party|suggest|vote|remove",
     "listen along with friends",
     "  together host SERVER [ROOM [PASSWORD [NAME]]]\n"
     "      a room on a melody-together server, this engine its host; prints the\n"
     "      invite to send to friends. ROOM and PASSWORD are made when - or left\n"
     "      out. A server that asks a password of its own is given\n"
     "      $MELODY_TOGETHER_PASSWORD, else the first line of\n"
     "      ~/.config/melody/together-password.\n"
     "  together join INVITE [NAME]           listen along, as NAME ($USER)\n"
     "  together join SERVER ROOM PASSWORD [NAME]\n"
     "  together leave                        out of the room; its host closes it\n"
     "  together status                       who listens, what plays, suggestions\n"
     "  together party on|off                 as host: friends suggest and vote\n"
     "  together suggest FILE...              at a party: tracks of your library\n"
     "  together vote N                       for suggestion N, or taken back\n"
     "  together remove N                     as host: suggestion N, gone\n"},
    {{"watch"},
     "Engines and setup",
     "watch [--all] | watch albums [--folders | --run CMD]",
     "follow playback, or react to new albums",
     "  watch prints the state each time it changes; --all follows whichever\n"
     "  engine here or nearby plays (only those given with --server, if any).\n"
     "  watch albums prints each album new to the library once it is whole: its\n"
     "  name, its folder (--folders), or runs CMD for it -- {} its folder,\n"
     "  quoted; MELODY_ALBUM_FOLDER, _ARTIST, _TITLE and _DATE set. Runs until\n"
     "  the engine goes, then fails.\n"
     "    melody-cli watch albums --run 'notify-send \"New: $MELODY_ALBUM_TITLE\"'\n"},
    {{"engines"},
     "Engines and setup",
     "engines",
     "the engines nearby, and which one is in use",
     "  Every engine announcing itself on the network, with its address and\n"
     "  whether it wants a password; \"in use\" marks the one melody-cli talks to.\n"},
    {{"config"},
     "Engines and setup",
     "config [KEY [VALUE...]] | config --edit",
     "this computer's melodyd.conf",
     "  This computer's engine settings -- not those of the engine --server\n"
     "  names: all of them, one, or one changed, checked as melodyd reads them,\n"
     "  comments kept. --engine NAME for an [engine NAME] section, --unset KEY,\n"
     "  --config FILE (default $MELODYD_CONFIG, else\n"
     "  ~/.config/melody/melodyd.conf). A new file starts from the commented\n"
     "  skeleton. --edit opens it in $EDITOR and takes it only when melodyd\n"
     "  would.\n"
     "    melody-cli config password 'a long sentence'\n"},
    {{"help"},
     "Engines and setup",
     "help [COMMAND]",
     "this overview, or one command in full",
     "  Also COMMAND --help.\n"
     "    melody-cli help queue\n"},
}};

constexpr std::string_view engine_choice =
    "Which engine: --local for this computer's, --server HOST:PORT (or\n"
    "$MELODY_SERVER) for one by address, --engine NAME for one found nearby.\n"
    "Without any, $MELODY_SERVER, else this computer's engine, else the one\n"
    "nearby. status says which one answered. --password PASS (or\n"
    "$MELODY_PASSWORD) or --password-file FILE ($MELODY_PASSWORD_FILE) for an\n"
    "engine on the network; --json prints the engine's own answers.\n";

} // namespace

void print_overview(std::ostream& out) {
    out << "usage: melody-cli [--local | --server HOST:PORT | --engine NAME] [--json]\n"
           "                  COMMAND [ARGUMENTS]\n"
           "       melody-cli help COMMAND      one command in full, with examples\n";
    constexpr std::size_t column = 34U;
    for (const auto group : groups) {
        out << "\n" << group << "\n";
        for (const auto& entry : entries) {
            if (entry.group != group) {
                continue;
            }
            out << "  " << entry.synopsis;
            if (entry.synopsis.size() + 2U < column) {
                out << std::string(column - entry.synopsis.size() - 2U, ' ');
            } else {
                out << "\n" << std::string(column, ' ');
            }
            out << entry.summary << "\n";
        }
    }
    out << "\n" << engine_choice;
}

bool print_command_help(std::ostream& out, const std::string_view command) {
    bool found = false;
    for (const auto& entry : entries) {
        if (!std::ranges::contains(entry.names, command)) {
            continue;
        }
        out << (found ? "\n" : "") << "melody-cli " << entry.synopsis << "\n" << entry.detail;
        found = true;
    }
    return found;
}

} // namespace trackknife::cli
