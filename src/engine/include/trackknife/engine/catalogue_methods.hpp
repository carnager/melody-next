// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/job_registry.hpp"
#include "trackknife/protocol/dispatch.hpp"

#include <functional>
#include <string>

namespace trackknife::engine {

// What changed in the library, as told to every client:
//   catalogue.changed {paths: [raw path], albums: [album key], everything}
// paths re-read or dropped, the albums they touched, or after a scan or a
// change of folders, everything. Keys and paths encoded as raw paths.
[[nodiscard]] protocol::Event catalogue_change_event(const LocalCatalogue::Change& change);

// ADR-0222: binds the catalogue's read and rating operations onto a
// dispatcher. Long operations are deliberately absent -- `catalogue.scan` is a
// job, and a job is not a slow handler.
//
// A rating that is set is told to every client as `catalogue.rating_changed`
// {hash, album, rating} through `events`, so each shows it without asking.
//
// `holds` says whether the engine holds a file to play -- the files it may
// stream. `catalogue.artwork` gives their covers too, read from the file,
// indexed or not.
//
// The catalogue must outlive the dispatcher.
using HeldPath = std::function<bool(const std::string& raw_path)>;
// Told of each rating stored, after it is: what else follows one (ADR-0237
// stage 2, ratings in tags).
using RatingObserver = std::function<void(const std::string& hash, bool album, unsigned rating)>;
void register_catalogue_methods(protocol::Dispatcher& dispatcher, Catalogue& catalogue,
                                EventSink events = {}, HeldPath holds = {},
                                RatingObserver rated = {});

} // namespace trackknife::engine
