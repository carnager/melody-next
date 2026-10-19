// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/engine/job_registry.hpp"
#include "trackknife/protocol/dispatch.hpp"

#include <cstddef>

namespace trackknife::engine {

class LocalCatalogue;

class Workspace;

// ADR-0237: how files the engine moves are named and where they go.
//
// Naming layouts are global presets, made in Trackknife and handed to every
// engine; the engine keeps its copy so any client that moves files through
// it can name them.
//   layouts.list                    -> {layouts: [{id, profile}]}
//   layouts.put {layouts, removed?} -> {layouts}: each layout added or
//       updated, and only the ids in `removed` taken away -- never one it
//       was not told of, whatever set a client holds
//
// Move destinations are this engine's -- folders on its machine.
//   destinations.list               -> {destinations: [{id, profile}]}
//   destinations.save {destination} -> {destinations}
//   destinations.remove {id}        -> {destinations}
//   event destinations.changed      after either, for every client
//
// Choosing one browses this machine's folders:
//   folders.list {path?, links?}    -> {path, parent, folders: [name],
//                                       linked: [name], home, roots: [path]}
// The folders directly in `path` (absolute; the engine's home when absent),
// by encoded name and in name order. A symlink to a folder is left out
// unless `links`, and then listed in `linked` too. `parent` is null at the
// root. At most folder_list_limit. `home` and the library's music folders,
// `roots`, are where browsing starts (level 20).
//   folders.make {path, name}       -> {path}
// A new folder `name` in `path`; one already there is a conflict.
inline constexpr std::size_t folder_list_limit = 10'000U;
void register_naming_methods(protocol::Dispatcher& dispatcher, Workspace& workspace, EventSink sink,
                             const LocalCatalogue* catalogue = nullptr);

} // namespace trackknife::engine
