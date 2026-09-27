// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/engine/job_registry.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "trackknife/protocol/dispatch.hpp"

namespace trackknife::engine {

class Player;
class Workspace;

// ADR-0233: the engine's lists, working and saved, for every client.
//
//   list.all                                   -> {"lists": [summary]}
//   list.get {id}                              -> summary + {"items": [item]}
//   list.save {id?, name, kind?, items, revision?} -> summary
//   list.rename {id, name, revision?}          -> summary
//   list.delete {id, revision?}                -> {"deleted": bool}
//   list.play {id, entry?}                     -> {"playing": entry}
//   list.relocate {moves: [{from, to}]}        -> {"changed": [id]}
//   event list.changed {id, revision?, deleted}
//
// A summary is {id, name, kind: "working"|"saved", revision, tracks,
// modified_ms}. An item is a queue entry's fields -- entry, path, segment,
// selection, duration_ms -- plus logical, title, artist and album. A write
// with `revision` is refused as a conflict when the list has moved on since;
// for a new list, `revision` 0 refuses if the id is taken.
void register_list_methods(protocol::Dispatcher& dispatcher, Workspace& workspace, EventSink sink,
                           Player& player);

// list.changed, as every change to a list is announced: `summary` null when
// it was deleted.
void announce_list_change(const EventSink& sink, const persistence::EngineListSummary* summary,
                          const core::StableId& id);

} // namespace trackknife::engine
