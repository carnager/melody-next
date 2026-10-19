// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/engine/job_registry.hpp"
#include "trackknife/protocol/dispatch.hpp"

#include <string_view>

namespace trackknife::engine {

class LocalCatalogue;
class Player;
class Workspace;

// ADR-0297: the favourite tracks, a saved playlist named Favorites -- made
// with the first, a file in the playlist folder as every saved list is.
//
//   favorites.toggle {path?, favorite?}    -> {favorite, list}
//     the track at `path` (encoded; the one playing when absent) made a
//     favourite or not -- `favorite` says which, else it turns over.
//   favorites.state {paths}                -> {favorites: [bool], list}
//     whether each is one; `list` null while there is no Favorites list.
//
// Its change is announced through `sink`, as every list's is.
inline constexpr std::string_view favorites_name = "Favorites";
void register_favorite_methods(protocol::Dispatcher& dispatcher, Workspace& workspace,
                               EventSink sink, Player& player, const LocalCatalogue* catalogue);

} // namespace trackknife::engine
