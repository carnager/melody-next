// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/protocol/dispatch.hpp"

namespace trackknife::engine {

class Catalogue;
class Player;

// What plays, formatted by the engine, which has the library's whole row
// for it -- every tag, ReplayGain included, and the technicals -- where a
// client has only what catalogue.query summarises.
//
//   playback.format {format} -> {"text": string | null}
//
// `format` is tkfmt-1 in the now-playing context (docs/title-formatting.md):
// the track's tags as fields; $info(codec), $info(samplerate),
// $info(bitspersample), $info(channels), $info(lengthms); and the host
// fields playback_state, playback_time, playback_remaining, length, path and
// rating (1-10, half stars; absent when unrated). A file the library does not
// index is formatted from what its queue entry says. `text` is null when
// nothing plays; a format that does not compile is refused as
// invalid_argument, with the reason.
void register_now_playing_methods(protocol::Dispatcher& dispatcher, const Catalogue& catalogue,
                                  Player& player);

} // namespace trackknife::engine
