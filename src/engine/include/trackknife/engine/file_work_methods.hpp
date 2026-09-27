// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/error.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "trackknife/protocol/dispatch.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>

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

// Recovers the engine's metadata journal, as Trackknife recovers its own:
// finished or rolled back where that is safe, left for the user otherwise.
// Backups are released, as there is no undo to keep them for. Run once, at
// startup, before clients connect.
[[nodiscard]] FileWorkRecovery recover_file_work(const std::filesystem::path& database,
                                                 LocalCatalogue& catalogue);

// metadata.interrupted answers {recovered, error, interrupted: [{id, path,
// message}]}: what startup recovery did, and each operation it could neither
// finish nor roll back -- what Trackknife's "Interrupted file work" lists.
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
void register_artwork_methods(protocol::Dispatcher& dispatcher, std::filesystem::path staging);

// Staged images outlive the plan that used them by a week, then go; run at
// startup.
void clean_artwork_staging(const std::filesystem::path& staging);

} // namespace trackknife::engine
