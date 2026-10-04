// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/engine/metadata_services.hpp"
#include "trackknife/loudness/scan.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/write_plan.hpp"
#include "trackknife/operations/artwork_apply.hpp"
#include "trackknife/operations/file_publication_apply.hpp"
#include "trackknife/operations/metadata_apply.hpp"
#include "trackknife/operations/undo.hpp"
#include "trackknife/operations/preparation_plan.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "trackknife/protocol/client.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace trackknife::engine {

// A file's best audio stream, as the tagger's technical panel shows it.
struct FileTechnicals {
    std::string codec;
    int sample_rate{0};
    int bits{0};
    int channels{0};
    std::int64_t bit_rate{0};
    std::int64_t duration_ms{-1};

    friend bool operator==(const FileTechnicals&, const FileTechnicals&) = default;
};

// This process's reading of it: formats::probe_local_media.
[[nodiscard]] core::Result<FileTechnicals>
probe_local_technicals(const std::string& raw_path, const core::CancellationToken& cancellation);

// ADR-0237: the file tools' reads, measurements and writes, done by the
// engine that holds the files. The tools keep their own code and their own
// screens; this gives them the same three things they use locally -- a way
// to read files, a loudness scanner, a plan applier -- that go to the engine
// instead. Paths are the engine's.
//
// Its own connection, made on first use and again after it drops, so a scan
// never waits behind playback. Safe to use from several worker threads; every
// call blocks, so none may run on a UI thread.
// The most one loudness.scan or metadata.apply job carries, in bytes of JSON:
// half the engine's line limit. A larger one goes as several jobs -- a scan
// with each album whole in one.
inline constexpr std::size_t remote_job_bytes = 512U * 1024U;

class RemoteFileWork final {
  public:
    explicit RemoteFileWork(protocol::Endpoint endpoint);
    RemoteFileWork(const RemoteFileWork&) = delete;
    RemoteFileWork& operator=(const RemoteFileWork&) = delete;
    ~RemoteFileWork();

    // read: the file's tags as the engine reads them. revision: a file's
    // revision there -- one with no tags to read, a CUE sheet.
    [[nodiscard]] metadata::MetadataFileAccess access();

    [[nodiscard]] core::Result<loudness::LoudnessScanResult>
    scan(std::span<const loudness::LoudnessScanItem> items,
         const loudness::LoudnessScanOptions& options,
         const loudness::LoudnessScanProgressCallback& progress,
         const core::CancellationToken& cancellation);

    // media.probe: the file's technicals as the engine's decoder sees them.
    [[nodiscard]] core::Result<FileTechnicals> probe(const std::string& raw_path,
                                                     const core::CancellationToken& cancellation);

    [[nodiscard]] core::Result<operations::MetadataApplyResult>
    apply(const metadata::MetadataWritePlan& plan,
          const operations::MetadataApplyProgressCallback& progress,
          const core::CancellationToken& cancellation);

    // Stage 4, artwork: the planner's reads (artwork.inventory, image_file,
    // image_bytes, destination), an image handed over to be written from
    // (artwork.stage), and a reviewed artwork plan written (artwork.apply).
    [[nodiscard]] operations::ArtworkFileAccess artwork_access();
    [[nodiscard]] core::Result<metadata::ArtworkImageFile>
    stage(std::span<const unsigned char> bytes);
    [[nodiscard]] core::Result<operations::ArtworkApplyResult>
    artwork_apply(const metadata::ArtworkWritePlan& plan,
                  const operations::ArtworkApplyProgressCallback& progress,
                  const core::CancellationToken& cancellation);

    // Stage 5, moves and renames: a path plan built here, looked at on the
    // engine's filesystem (paths.preflight), and a reviewed preparation --
    // tags, paths, or both -- published there (preparation.apply).
    [[nodiscard]] core::Result<operations::OutputPathPreflight>
    preflight(const operations::OutputPathPlan& plan, const core::CancellationToken& cancellation);
    [[nodiscard]] core::Result<operations::FilePublicationApplyResult>
    publish(const operations::PreparationPlan& plan,
            const operations::FilePublicationApplyProgressCallback& progress,
            const core::CancellationToken& cancellation);

    // ADR-0263: writes undone there (operations.undo), newest first as given.
    [[nodiscard]] core::Result<std::vector<operations::UndoOutcome>>
    undo(std::span<const operations::UndoRequest> requests,
         const operations::UndoProgressCallback& progress,
         const core::CancellationToken& cancellation);

    // Stage 6: a library file as it is, fetched into `to` on this computer
    // (streams.ticket, then the stream port) -- what the converter reads
    // when it cannot reach the engine's files itself.
    [[nodiscard]] core::Result<void> download_original(const std::string& raw_path,
                                                       const std::filesystem::path& to,
                                                       const core::CancellationToken& cancellation);

    // The tagger's lookups, made by the engine (musicbrainz.fetch,
    // acoustid.fingerprint, acoustid.lookup): the bytes a service answered.
    [[nodiscard]] core::Result<std::string> fetch(const std::string& url,
                                                  const core::CancellationToken& cancellation);
    [[nodiscard]] core::Result<MetadataServices::Fingerprint>
    fingerprint(const std::string& raw_path, const core::CancellationToken& cancellation);
    [[nodiscard]] core::Result<std::string>
    acoustid_lookup(const MetadataServices::Fingerprint& fingerprint,
                    const core::CancellationToken& cancellation);
    // Hands the engine the AcoustID key it looks up with; empty forgets it.
    [[nodiscard]] core::Result<void> set_acoustid_key(const std::string& key);
    // Stage 2: whether the engine also writes track ratings into the files,
    // and the scale other players' plain RATING tags are read on ("off",
    // "5", "10", "100").
    [[nodiscard]] core::Result<void> set_rating_tags(bool write_tags);
    [[nodiscard]] core::Result<void> set_rating_scale(const std::string& scale);
    // ADR-0245: the tag ratings are copied into; empty for none.
    [[nodiscard]] core::Result<void> set_rating_backup_tag(const std::string& tag);
    // ADR-0263: how long the files writes replace are kept there for undo
    // (backups.set_retention); 0 keeps none.
    [[nodiscard]] core::Result<void> set_backup_retention(int days, int writes, int gigabytes);

    // Naming layouts handed over (layouts.put): each added or updated, and
    // only `removed` taken away. This engine's own move destinations, and
    // folders.list for choosing one on its machine.
    [[nodiscard]] core::Result<void>
    put_layouts(const std::vector<persistence::SavedOutputLayoutProfile>& layouts,
                const std::vector<core::StableId>& removed = {});
    [[nodiscard]] core::Result<std::vector<persistence::SavedDestinationProfile>> destinations();
    [[nodiscard]] core::Result<void>
    save_destination(const persistence::SavedDestinationProfile& destination);
    [[nodiscard]] core::Result<void> remove_destination(const core::StableId& id);
    struct FolderListing {
        std::string path;
        std::optional<std::string> parent;
        std::vector<std::string> folders;
    };
    // `path` empty: where the engine starts, its home.
    [[nodiscard]] core::Result<FolderListing> folders(const std::string& path);

    // metadata.interrupted: what the engine recovered at startup and what it
    // could not. Absent (not_found) when the engine is older than file work.
    [[nodiscard]] core::Result<protocol::Json> interrupted();

    // Whether the engine does file work at all -- false for one older than
    // ADR-0237, which the tools then do themselves, as before. Asked once.
    [[nodiscard]] bool supported();

    // The most one scan or apply job carries (remote_job_bytes unless told).
    void limitJobs(std::size_t bytes) noexcept { job_bytes_ = bytes; }

  private:
    [[nodiscard]] core::Result<std::shared_ptr<protocol::Client>> client();
    // ADR-0237: files read a batch to a request, in order, one result each.
    [[nodiscard]] core::Result<std::vector<core::Result<metadata::LocalMetadataRead>>>
    read_many(const std::vector<std::string>& raw_paths,
              const core::CancellationToken& cancellation);
    [[nodiscard]] core::Result<protocol::Json>
    read_one(const std::string& raw_path, const core::CancellationToken& cancellation);

    protocol::Endpoint endpoint_;
    std::size_t job_bytes_{remote_job_bytes};
    std::mutex mutex_;
    std::shared_ptr<protocol::Client> client_;
    std::optional<bool> supported_;
};

} // namespace trackknife::engine
