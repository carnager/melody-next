// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/engine/catalogue.hpp"
#include "trackknife/protocol/dispatch.hpp"

namespace trackknife::engine {

// ADR-0222: binds the catalogue's read and rating operations onto a
// dispatcher. Long operations are deliberately absent -- `catalogue.scan` is a
// job, and a job is not a slow handler.
//
// The catalogue must outlive the dispatcher.
void register_catalogue_methods(protocol::Dispatcher& dispatcher, Catalogue& catalogue);

} // namespace trackknife::engine
