// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/local_reader.hpp"

#include <tiostream.h>

#include <string>

namespace trackknife::metadata {

// The metadata of whatever `stream` holds, read as read_local_metadata reads
// a file -- the same document, adapter and capabilities -- with no source
// revision: the caller says what the stream is. ADR-0281 reads the tags a
// write would leave from an overlay over the file, before the file is touched.
[[nodiscard]] core::Result<LocalMetadataRead>
read_metadata_from_stream(const std::string& raw_path, TagLib::IOStream& stream,
                          const core::CancellationToken& cancellation = {});

} // namespace trackknife::metadata
