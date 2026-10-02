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
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

namespace trackknife::persistence {

// ADR-0237 stage 2: the engine_state key naming the scale plain RATING tags
// are read on ("off", "5", "10", "100"), which indexing reads.
inline constexpr std::string_view plain_rating_scale_state_key = "ratings.rating-scale";

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

// ADR-0254: `group` is a level of a library view.
enum class LibraryEntryKind { artist, album, track, group };

// ADR-0254: one level of a library view -- a tkfmt-1 expression in the
// tree-level host, which may use $each, and how its nodes are ordered: by
// `sort` when given, else by label.
struct LibraryViewLevel {
    std::string format;
    std::string sort;
    bool descending{false};

    friend bool operator==(const LibraryViewLevel&, const LibraryViewLevel&) = default;
};

struct LibraryQuery {
    LibraryEntryKind kind{LibraryEntryKind::artist};
    std::string text;
    std::optional<std::string> artist;
    std::optional<std::string> album_key;
    std::optional<std::string> raw_path;
    std::size_t offset{0};
    std::size_t limit{200};
    // Albums and tracks by when they came into the library, newest first,
    // rather than by artist and name.
    bool newest_first{false};
    // In no order at all: a random artist, album or tracks, as many as the
    // limit asks for. Outranks newest_first.
    bool random{false};
    // ADR-0254: browsing a view. Its levels, the labels opened so far -- as
    // many as there are levels lists that node's tracks -- and a tkq-1 query
    // narrowing it first. `kind` and the fields above are ignored then.
    std::vector<LibraryViewLevel> view;
    std::vector<std::string> view_path;
    std::string view_filter;
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
    // A track's album's rating key and rating, so its album can be rated from
    // the track; an album's own, repeated; empty/0 for artists.
    std::string album_rating_hash{};
    unsigned album_rating{0};
    // An album's or a track's date, as tagged; empty for artists.
    std::string date{};
    // A track's own title, as tagged -- `label` is how the tree shows it.
    std::string title{};
    // When it came into the library (an album: its newest track), in Unix
    // seconds; 0 for artists.
    std::int64_t added{0};
    // A track's length, an album's total; -1 when any of it is unknown, as
    // for an artist. What a client queues needs it to show a duration.
    std::int64_t duration_ms{-1};
    // ADR-0254: a view's node -- a group, or an album standing for one -- by
    // the label its level gave it, which is what opening it adds to the path.
    std::string view_value{};
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

// ADR-0254: an indexed track as a view groups it -- the entry the tree shows,
// its album, and every fact its levels can read.
struct LibraryViewTrack {
    LibraryEntry entry;
    std::string album_key;
    TkqRowFacts facts;
};

struct LibraryScanProgress {
    std::atomic<std::size_t> visited{0};
    std::atomic<std::size_t> indexed{0};
    std::atomic<std::size_t> failed{0};
};

// ADR-0232: one indexed file as the index last saw it, for a watcher on the
// files' own machine to compare with what is there now.
struct LibraryInventoryEntry {
    std::string raw_path;
    // As indexed; 0 and 0 when the row predates recording them, which
    // compares as changed.
    std::uint64_t size{0};
    std::int64_t modified_seconds{0};
    bool available{true};
};

struct LibraryInventoryPage {
    std::vector<LibraryInventoryEntry> entries;
    bool more{false};
};

// A folder as the library holds it: the names of the folders below it that
// hold indexed tracks, in byte order, and the tracks directly in it, in disc
// and track order. Read from the index only, so it is the same answer for a
// client with no access to the files.
struct LibraryFolder {
    std::vector<std::string> folders;
    std::vector<LibraryEntry> tracks;
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
    // ADR-0254: every indexed track, available or not, in library order, with
    // all its fields -- and its listening history when `history` -- handed
    // to `visit` one at a time, so a whole library is never held at once.
    core::Result<void> each_track(const std::function<void(LibraryViewTrack&&)>& visit,
                                  bool history,
                                  const core::CancellationToken& cancellation = {}) const;
    // ADR-0254: what a view grouped from each_track depends on, as one
    // string: the tracks' revision, the ratings, and the listening history
    // when `history`. Equal stamps, equal views.
    core::Result<std::string> view_stamp(bool history) const;
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
    // Whether the library changed since a listing: its id and a count that
    // goes up when a track comes, goes, or changes what a listing shows, as
    // "id:count". Equal means a cached listing is still true.
    core::Result<std::string> revision() const;
    // Stored ratings for each hash in input order; 0 means unrated.
    core::Result<std::vector<unsigned>>
    ratings(const std::vector<std::string>& hashes,
            const core::CancellationToken& cancellation = {}) const;
    // ADR-0237 stage 2: the files a track rating belongs to, and every rated
    // track with its rating -- what writing ratings into tags visits.
    core::Result<std::vector<std::string>> rated_paths(const std::string& track_hash) const;
    core::Result<std::vector<std::pair<std::string, unsigned>>>
    rated_tracks(const core::CancellationToken& cancellation = {}) const;
    // For a library indexed before ratings were imported, and again when
    // the plain RATING scale changes: each unrated track takes the rating
    // its files' tags carry, from what was indexed. Returns how many were
    // rated.
    core::Result<std::size_t> import_indexed_tag_ratings();
    core::Result<LibraryScanResult> scan(const core::CancellationToken& cancellation,
                                         LibraryScanProgress& progress);
    // Re-reads the named files now, without a walk: after they were tagged
    // or moved by someone else. A file inside a library folder is indexed
    // as a scan would; one that is gone is dropped from the index, as a
    // moved file's old path is. Paths outside every folder are ignored.
    // Returns how many were re-read or dropped.
    //
    // A path that is gone and was a folder -- moved or deleted as a whole --
    // drops every track indexed under it (ADR-0232), provided the folder it
    // was in is still there: a share that is not mounted must not read as a
    // library that was deleted.
    core::Result<std::size_t> refresh(const std::vector<std::string>& raw_paths,
                                      const core::CancellationToken& cancellation = {});
    // See LibraryFolder. A trailing slash names the same folder.
    [[nodiscard]] core::Result<LibraryFolder>
    folder(const std::string& raw_path, const core::CancellationToken& cancellation = {}) const;
    // ADR-0232: what is indexed under `folder`, in path order, the page after
    // the path `after` (empty for the first). No filesystem access.
    core::Result<LibraryInventoryPage> inventory(const std::string& folder,
                                                 const std::string& after,
                                                 std::size_t limit) const;

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
