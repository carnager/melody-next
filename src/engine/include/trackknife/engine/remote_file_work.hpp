// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/engine/metadata_services.hpp"
#include "trackknife/loudness/scan.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/write_plan.hpp"
#include "trackknife/operations/metadata_apply.hpp"
#include "trackknife/protocol/client.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>

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

    // metadata.interrupted: what the engine recovered at startup and what it
    // could not. Absent (not_found) when the engine is older than file work.
    [[nodiscard]] core::Result<protocol::Json> interrupted();

    // Whether the engine does file work at all -- false for one older than
    // ADR-0237, which the tools then do themselves, as before. Asked once.
    [[nodiscard]] bool supported();

  private:
    [[nodiscard]] core::Result<std::shared_ptr<protocol::Client>> client();
    [[nodiscard]] core::Result<protocol::Json>
    read_one(const std::string& raw_path, const core::CancellationToken& cancellation);

    protocol::Endpoint endpoint_;
    std::mutex mutex_;
    std::shared_ptr<protocol::Client> client_;
    std::optional<bool> supported_;
};

} // namespace trackknife::engine
