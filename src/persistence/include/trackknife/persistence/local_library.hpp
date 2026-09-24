// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "trackknife/persistence/tkq_row.hpp"
#include "trackknife/query/tkq.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace trackknife::persistence {

struct LibraryRoot {
    std::string raw_path;
    bool available{false};
    std::string error;
};

struct LibraryHistorySource {
    ListItem source;
    // Empty for unnamed albums, which remain singleton source groups.
    std::string album_hash;
};

enum class LibraryEntryKind { artist, album, track };

struct LibraryQuery {
    LibraryEntryKind kind{LibraryEntryKind::artist};
    std::string text;
    std::optional<std::string> artist;
    std::optional<std::string> album_key;
    std::optional<std::string> raw_path;
    std::size_t offset{0};
    std::size_t limit{200};
};

struct LibraryEntry {
    LibraryEntryKind kind{LibraryEntryKind::track};
    std::string key;
    std::string label;
    std::string artist;
    std::string album;
    std::size_t tracks{0};
    std::size_t available{0};
    int track_number{0};
    std::size_t albums{0};
    // ADR-0179: the entry's content-identity rating key (track or album hash)
    // and its stored 0-10 rating; empty/0 for artists and unrated entries.
    std::string rating_hash{};
    unsigned rating{0};
    // An album's or a track's date, as tagged; empty for artists.
    std::string date{};
};

struct LibraryPage {
    std::vector<LibraryEntry> entries;
    bool more{false};
};

// Presentation snapshot only; never a complete native metadata write baseline.
struct LibraryTrackSnapshot {
    std::string raw_path;
    TkqRowFacts facts;
};

struct LibraryScanProgress {
    std::atomic<std::size_t> visited{0};
    std::atomic<std::size_t> indexed{0};
    std::atomic<std::size_t> failed{0};
};

struct LibraryScanResult {
    bool cancelled{false};
    bool incomplete{false};
};

// Synchronous, Qt-free service. Each worker owns its own connection; all
// traversal, probing, SQL, and query formatting must run off the UI thread.
class LocalLibrary final {
  public:
    static core::Result<LocalLibrary> open(const std::filesystem::path& database_path);
    LocalLibrary(LocalLibrary&&) noexcept;
    LocalLibrary& operator=(LocalLibrary&&) noexcept;
    ~LocalLibrary();
    LocalLibrary(const LocalLibrary&) = delete;
    LocalLibrary& operator=(const LocalLibrary&) = delete;

    core::Result<std::vector<LibraryRoot>> roots() const;
    core::Result<void> add_root(const std::string& raw_path);
    core::Result<void> remove_root(const std::string& raw_path);
    core::Result<LibraryPage> query(const LibraryQuery& query,
                                    const core::CancellationToken& cancellation = {}) const;
    core::Result<std::vector<std::string>>
    paths(const LibraryQuery& query, const core::CancellationToken& cancellation = {}) const;
    // ADR-0150: structured tkq evaluation over the cached index only.
    // Indexable predicates push down into SQL; tkfmt expression predicates
    // evaluate per candidate row. A sort clause materializes the match set
    // (bounded like paths) before paging.
    core::Result<LibraryPage> filter(const query::CompiledTkq& compiled, std::size_t offset,
                                     std::size_t limit,
                                     const core::CancellationToken& cancellation = {}) const;
    core::Result<std::vector<std::string>>
    filter_paths(const query::CompiledTkq& compiled,
                 const core::CancellationToken& cancellation = {}) const;
    core::Result<std::vector<std::array<std::int64_t, 6>>>
    history_facts(const std::vector<LibraryHistorySource>& sources,
                  const core::CancellationToken& cancellation = {}) const;
    // Reads cached fields/technicals in input order, preserving raw paths and duplicates.
    // No filesystem access; missing index records fail rather than trigger discovery.
    core::Result<std::vector<LibraryTrackSnapshot>>
    cached_tracks(const std::vector<std::string>& raw_paths,
                  const core::CancellationToken& cancellation = {}) const;
    core::Result<std::optional<std::string>>
    artwork_source(const std::string& album_key,
                   const core::CancellationToken& cancellation = {}) const;
    // ADR-0179: 0-10 content-identity ratings shared with Melody's scale.
    // Rating 0 deletes the stored row; hashes come from rating_identity() or
    // library entries, so files outside the library rate identically.
    core::Result<void> set_rating(const std::string& hash, bool album, unsigned rating);
    // Stored ratings for each hash in input order; 0 means unrated.
    core::Result<std::vector<unsigned>>
    ratings(const std::vector<std::string>& hashes,
            const core::CancellationToken& cancellation = {}) const;
    core::Result<LibraryScanResult> scan(const core::CancellationToken& cancellation,
                                         LibraryScanProgress& progress);
    // Re-reads the named files now, without a walk: after they were tagged
    // or moved by someone else. A file inside a library folder is indexed
    // as a scan would; one that is gone is dropped from the index, as a
    // moved file's old path is. Paths outside every folder are ignored.
    // Returns how many were re-read or dropped.
    core::Result<std::size_t> refresh(const std::vector<std::string>& raw_paths,
                                      const core::CancellationToken& cancellation = {});

  private:
    struct Impl;
    explicit LocalLibrary(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> implementation_;
};

// Called inside ListRepository's existing metadata/relocation transaction.
// No filesystem I/O or nested transaction; only already indexed rows change.
core::Result<void> refresh_library_source(sqlite3* database, const std::string& source,
                                          const std::string& target,
                                          const metadata::MetadataDocument* document);

} // namespace trackknife::persistence
