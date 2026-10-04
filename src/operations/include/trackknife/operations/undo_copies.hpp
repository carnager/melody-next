// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"

#include <filesystem>
#include <string>

namespace trackknife::operations {

// ADR-0266: where this engine keeps what its writes replace -- a tag
// write's backup, a move's retained source -- for undo: a folder of the
// person's choosing (by default the engine's own), or, empty, beside each
// file. One engine, one place; set at start and when the person changes it.
void set_undo_copy_folder(std::filesystem::path folder);
[[nodiscard]] std::filesystem::path undo_copy_folder();

// Where the kept copy named `name` of the file at `source_raw_path` goes:
// in the folder, or beside the file when there is none. The folder is made
// when missing; a path is returned either way, and a folder that cannot be
// made fails the write that tries to use it.
[[nodiscard]] std::string undo_copy_path(const std::string& source_raw_path,
                                         const std::string& name);

// A copy of the file at `from`, made at `to` (which must not exist): its
// permissions and time kept, its bytes read back and compared. Its identity.
[[nodiscard]] core::Result<core::LocalSourceRevision> copy_file_verified(const std::string& from,
                                                                         const std::string& to);

class MetadataOperationJournal;
class FilePublicationJournal;

// ADR-0266: every retained undo copy the journals know -- a tag write's
// backup, a move's retained source -- moved to where undo copies are kept
// now: renamed on one filesystem, else copied, verified, recorded, and only
// then removed. A copy a crash left in both places is found by its name and
// removed from the old one. How many moved.
[[nodiscard]] core::Result<std::size_t> keep_undo_copies_in_place(MetadataOperationJournal& metadata,
                                                                  FilePublicationJournal& files);

} // namespace trackknife::operations
