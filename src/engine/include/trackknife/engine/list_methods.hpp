// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/engine/job_registry.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "trackknife/protocol/dispatch.hpp"

namespace trackknife::engine {

class LocalCatalogue;
class Player;
class Workspace;

// ADR-0233: the engine's lists, working and saved, for every client.
//
//   list.all                                   -> {"lists": [summary]}
//   list.get {id}                              -> summary + {"items": [item]}
//   list.save {id?, name, kind?, items, revision?} -> summary
//   list.edit {id, revision, edits, name?, kind?} -> summary
//   list.rename {id, name, revision?}          -> summary
//   list.delete {id, revision?}                -> {"deleted": bool}
//   list.draft {of}                            -> summary (ADR-0259)
//   list.commit {id, force?}                   -> summary of the saved list
//   list.from_query {query, name, words?}      -> summary of a new working list
//   list.describe {id, entries}                -> {revision, items: [{entry,
//                                                 library|null, missing?}]}
//   list.play {id, entry?}                     -> {"playing": entry}
//   list.relocate {moves: [{from, to}]}        -> {"changed": [id]}
//   event list.changed {id, revision?, deleted}
//
// A summary is {id, name, kind: "working"|"saved", revision, tracks,
// modified_ms, draft_of, draft_base}: a draft is a working list holding the
// unsaved edits of the saved list `draft_of`, begun at its revision
// `draft_base`; one to a saved list, deleted with it. An item is a queue entry's fields -- entry, path, segment,
// selection, duration_ms -- plus logical, title, artist and album. A write
// with `revision` is refused as a conflict when the list has moved on since;
// for a new list, `revision` 0 refuses if the id is taken.
//
// ADR-0256: `edits` are applied in order, all or none, and the queue played
// from the list follows them: {"remove": [entry]}, {"insert": [item],
// "after": entry|null}, {"move": [entry], "after": entry|null},
// {"update": [item]}. An item also carries album_artist, date and an optional
// replay_gain {track_gain_db, track_peak, album_gain_db, album_peak}.
//
// ADR-0259: `list.describe` gives each entry the library indexes a "library"
// description (track_description.hpp), from `catalogue`; an item's own
// title, artist and album are only for a file it does not.
void register_list_methods(protocol::Dispatcher& dispatcher, Workspace& workspace, EventSink sink,
                           Player& player, const LocalCatalogue* catalogue = nullptr);

// list.changed, as every change to a list is announced: `summary` null when
// it was deleted.
void announce_list_change(const EventSink& sink, const persistence::EngineListSummary* summary,
                          const core::StableId& id);

} // namespace trackknife::engine
