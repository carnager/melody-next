// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/error.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/job_registry.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/operations/file_publication.hpp"
#include "trackknife/operations/metadata_commit.hpp"
#include "trackknife/operations/undo.hpp"
#include "trackknife/protocol/dispatch.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

namespace trackknife::engine {

class Player;
class Workspace;

// ADR-0237: what the engine reads for the file tools, answered at once --
// the long work (measuring, writing) is jobs.
//
// metadata.read {paths: [encoded]} answers {files: [...]}, one per path in
// order: {read} with the file's tags, revision, adapter and capabilities, or
// {error, revision} when it cannot be read -- the revision present when the
// file exists but has no tags to read, which can still take gains in a
// sidecar. At most metadata_read_limit paths per call.
inline constexpr std::size_t metadata_read_limit = 256U;
// How many files of one metadata.read are read at once.
inline constexpr std::size_t metadata_read_threads = 8U;
//
// media.probe {paths: [encoded]} answers {files: [...]}, one per path: its
// best audio stream as {codec, sample_rate, bits, channels, bit_rate,
// duration_ms} -- what the tagger's technical panel shows -- or {error}.
// At most metadata_read_limit paths per call.

// What the engine did at startup about file work a crash interrupted: how
// many operations it finished or rolled back, or why it could not look.
struct FileWorkRecovery {
    std::size_t recovered{0U};
    std::optional<core::Error> error;
};

// Stage 5: what follows a file the engine moved or renamed, as part of the
// move's commit -- and again, harmlessly, when recovery replays it: the
// workspace's record of the file and its library row (so ratings and history
// stay with it; `published` is the document of a file whose tags were
// written on the way), the library index at both paths, every list naming it
// (announced as list.changed on `sink`), and the player's queue.
using MoveFollower =
    std::function<core::Result<void>(const operations::FilePublicationCommitResult& result,
                                     const metadata::MetadataDocument* published)>;
[[nodiscard]] MoveFollower follow_moves(Workspace& workspace, LocalCatalogue& catalogue,
                                        Player* player, EventSink sink);

// Recovers the engine's journals, as Trackknife recovers its own: tag writes
// and moves finished or rolled back where that is safe, left for the user
// otherwise. A move finished here is followed by `follow`; an undo a crash
// interrupted is finished. Run once, at startup, before clients connect.
[[nodiscard]] FileWorkRecovery recover_file_work(const std::filesystem::path& database,
                                                 LocalCatalogue& catalogue,
                                                 const MoveFollower& follow);

// ADR-0289: the writes whose undo copies are kept -- a tagger's, until it
// closes, else left by one that never said it had.
[[nodiscard]] std::vector<operations::UndoRequest>
kept_undo_copies(const std::filesystem::path& database);

struct UndoCopyRelease {
    std::size_t released{0U};
    // The first that could not be, if any.
    std::optional<core::Error> error;
};
// Their undo copies deleted, each checked to be the copy the write kept.
// A write with none kept -- undone, let go of already, a plain rename -- is
// passed over.
UndoCopyRelease release_undo_copies(const std::filesystem::path& database,
                                    const std::vector<operations::UndoRequest>& requests,
                                    const core::CancellationToken& cancellation);

// At startup, off the way of clients: the copies `left` from before let go
// of, and any a crash left half moved put where they are kept (ADR-0266).
void tidy_undo_copies(const std::filesystem::path& database,
                      const std::vector<operations::UndoRequest>& left,
                      const core::CancellationToken& cancellation);

// ADR-0266: where this engine keeps the files writes replace -- the
// engine_state document "backups.location": its own folder (`undo` beside
// its database, the default), a folder given, or beside each file (empty).
[[nodiscard]] std::filesystem::path undo_copy_folder(const Workspace& workspace,
                                                     const std::filesystem::path& database);

// backups.release {operations: [{kind, id}]} -> {released, error}: the
// undo copies of those writes let go of (ADR-0289).
// backups.location -> {place: "engine" | "folder" | "beside", folder?, kept_in};
// backups.set_location {place, folder?} -> the same: kept, and every undo
// copy there is moved there at once. Paths are encoded raw paths.
void register_backup_methods(protocol::Dispatcher& dispatcher, std::filesystem::path database,
                             Workspace& workspace);

// metadata.interrupted answers {recovered, error, interrupted: [{id, path,
// message, target?}]}: what startup recovery did, and each operation it could
// neither finish nor roll back -- what Trackknife's "Interrupted file work"
// lists. `target` is where an interrupted move was going.
void register_file_work_methods(protocol::Dispatcher& dispatcher,
                                std::filesystem::path database = {},
                                FileWorkRecovery recovery = {});

// Stage 4, artwork:
//   artwork.inventory {paths, policy?} -> {files: [{inventory} | {error}]}:
//     embedded pictures and the configured sibling images of each file.
//   artwork.image_file {path, maximum_bytes?} -> {image}: an image file
//     inspected -- a PNG or JPEG, as a plan names it.
//   artwork.image_bytes {image, maximum_bytes?} -> {bytes}: its encoded
//     bytes, re-read under its revision and fingerprint, for showing it.
//   artwork.destination {path} -> {image | null}: what sits where a folder
//     image would go -- nothing, or a regular file with one link and its
//     image; anything else is an error.
//   artwork.stage {bytes} -> {image}: a client's image -- one picked on its
//     own disk, downloaded, or resized -- kept where the engine can write it
//     from, in `staging`, and inspected there. Named by content, so staging
//     the same image twice keeps one copy.
//   artwork.stage_part {upload, offset, bytes, last?} -> {received} | {image}:
//     a large image in parts, each under the engine's line limit, appended in
//     order to the upload a client named (hex); the last stages it as
//     artwork.stage would. A part out of place is refused as a conflict.
void register_artwork_methods(protocol::Dispatcher& dispatcher, std::filesystem::path staging);

// Staged images outlive the plan that used them by a week, then go; run at
// startup.
void clean_artwork_staging(const std::filesystem::path& staging);

} // namespace trackknife::engine
