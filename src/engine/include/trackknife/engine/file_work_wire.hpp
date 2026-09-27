// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/error.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/loudness/scan.hpp"
#include "trackknife/protocol/message.hpp"

namespace trackknife::engine::wire {

// ADR-0237: file work runs in the engine, and what it reads, measures and
// writes crosses protocol v1 unchanged. Each encoding here is exact -- a value
// decoded is equal to the value encoded -- because the preview a client shows
// is the contract the engine later holds a write to. Raw paths travel as
// encoded bytes, never as text; a double that is not finite (a track too
// short to have a loudness) travels as the string "inf", "-inf" or "nan".
//
// Decoding refuses a malformed document with invalid_argument rather than
// guessing: a client and an engine that disagree about a plan must not write.

using protocol::Json;

[[nodiscard]] Json encode(const core::Error& error);
[[nodiscard]] core::Result<core::Error> decode_error(const Json& value);

[[nodiscard]] Json encode(const core::LocalSourceRevision& revision);
[[nodiscard]] core::Result<core::LocalSourceRevision> decode_revision(const Json& value);

[[nodiscard]] Json encode(const formats::AudioSourceSelection& selection);
[[nodiscard]] core::Result<formats::AudioSourceSelection> decode_selection(const Json& value);

[[nodiscard]] Json encode(const formats::SampleRange& range);
[[nodiscard]] core::Result<formats::SampleRange> decode_range(const Json& value);

[[nodiscard]] Json encode(const loudness::LoudnessScanItem& item);
[[nodiscard]] core::Result<loudness::LoudnessScanItem> decode_scan_item(const Json& value);

[[nodiscard]] Json encode(const loudness::LoudnessScanOptions& options);
[[nodiscard]] core::Result<loudness::LoudnessScanOptions> decode_scan_options(const Json& value);

[[nodiscard]] Json encode(const loudness::LoudnessScanResult& result);
[[nodiscard]] core::Result<loudness::LoudnessScanResult> decode_scan_result(const Json& value);

} // namespace trackknife::engine::wire
