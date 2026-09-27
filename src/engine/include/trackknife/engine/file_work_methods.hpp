// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/protocol/dispatch.hpp"

#include <cstddef>

namespace trackknife::engine {

// ADR-0237: what the engine reads for the file tools, answered at once --
// the long work (measuring, writing) is jobs.
//
// metadata.read {paths: [encoded]} answers {files: [...]}, one per path in
// order: {read} with the file's tags, revision, adapter and capabilities, or
// {error, revision} when it cannot be read -- the revision present when the
// file exists but has no tags to read, which can still take gains in a
// sidecar. At most metadata_read_limit paths per call.
inline constexpr std::size_t metadata_read_limit = 256U;

void register_file_work_methods(protocol::Dispatcher& dispatcher);

} // namespace trackknife::engine
