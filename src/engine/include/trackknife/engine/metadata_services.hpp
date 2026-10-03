// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/engine/job_methods.hpp"
#include "trackknife/protocol/dispatch.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>

namespace trackknife::engine {

// ADR-0237: the tagger's online lookups, made by the engine that holds the
// files -- MusicBrainz and the Cover Art Archive, and AcoustID with the key
// the engine keeps. The client still reads the answers; the engine fetches,
// paces and caches them, and fingerprints its own files.
//
// Fetches reach only MusicBrainz's web service and the Cover Art Archive,
// over HTTPS: an engine is not a way to fetch anything for anyone.
class MetadataServices final {
  public:
    // `database` holds the response cache; `state` the AcoustID key.
    MetadataServices(std::filesystem::path database, std::filesystem::path state);

    [[nodiscard]] core::Result<std::string> fetch(const std::string& url,
                                                  const core::CancellationToken& cancellation);

    struct Fingerprint {
        std::size_t duration_seconds{0};
        std::string fingerprint;
    };
    // fpcalc, as Picard ships it, on a file of this machine.
    [[nodiscard]] core::Result<Fingerprint>
    fingerprint(const std::string& raw_path, const core::CancellationToken& cancellation);

    [[nodiscard]] core::Result<std::string>
    acoustid_lookup(const Fingerprint& fingerprint, const core::CancellationToken& cancellation);

    [[nodiscard]] core::Result<void> set_acoustid_key(const std::string& key);
    [[nodiscard]] bool has_acoustid_key();

    // Whether a URL is one fetch() will reach.
    [[nodiscard]] static bool fetchable(std::string_view url);

    // ADR-0261: what fetch() does with MusicBrainz's answer `status` to
    // attempt `attempt` (0 the first), which asked to wait
    // `retry_after_seconds` (0: it did not say): take it, wait that many
    // seconds and ask again, or give up -- twice asked again at most, and
    // never after more than 30 s.
    struct Throttle {
        enum class Kind : std::uint8_t { answer, wait, give_up };
        Kind kind{Kind::answer};
        std::int64_t seconds{0};

        friend bool operator==(const Throttle&, const Throttle&) = default;
    };
    [[nodiscard]] static Throttle throttle(long status, std::int64_t retry_after_seconds,
                                           int attempt);

  private:
    // Waits until `interval` has passed since the last dispatch of its kind.
    void pace(std::chrono::steady_clock::time_point& last, std::chrono::milliseconds interval);
    [[nodiscard]] std::string acoustid_key();

    std::filesystem::path database_;
    std::filesystem::path key_file_;
    std::mutex pacing_;
    std::chrono::steady_clock::time_point last_musicbrainz_{};
    std::chrono::steady_clock::time_point last_acoustid_{};
    std::mutex key_mutex_;
};

// musicbrainz.fetch {url} finishes with {result: {body}} -- the body as
// encoded bytes -- or {error}; acoustid.fingerprint {path} with {result:
// {duration_seconds, fingerprint}}; acoustid.lookup {duration_seconds,
// fingerprint} with {result: {body}}. All three are jobs: pacing and the
// network make them slow, and they must not hold a connection.
void register_metadata_service_jobs(JobCatalog& jobs, MetadataServices& services);

// metadata_services.set {acoustid_client_key} keeps the key (an empty one
// forgets it); metadata_services.status answers {acoustid: bool}.
void register_metadata_service_methods(protocol::Dispatcher& dispatcher,
                                       MetadataServices& services);

} // namespace trackknife::engine
