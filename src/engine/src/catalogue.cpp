// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/catalogue.hpp"

#include "track_format.hpp"

#include "trackknife/formats/artwork.hpp"

#include <algorithm>

namespace trackknife::engine {

core::Result<std::vector<Catalogue::FoundTrack>>
Catalogue::find(const query::CompiledTkq& compiled, const std::string& format,
                const std::size_t limit, const core::CancellationToken& cancellation) const {
    auto program = compile_client_format(format, titleformat::FormatContextKind::track_display);
    if (!program) {
        return std::unexpected(std::move(program.error()));
    }
    auto paths = filter_paths(compiled, cancellation);
    if (!paths) {
        return std::unexpected(std::move(paths.error()));
    }
    if (limit > 0U && paths->size() > limit) {
        paths->resize(limit);
    }
    auto snapshots = cached_tracks(*paths, cancellation);
    if (!snapshots) {
        return std::unexpected(std::move(snapshots.error()));
    }
    std::vector<FoundTrack> found;
    found.reserve(snapshots->size());
    for (auto& snapshot : *snapshots) {
        name_by_file(snapshot.facts, snapshot.raw_path);
        auto text = persistence::tkq_format(*program, snapshot.facts,
                                            track_fields(snapshot.facts, snapshot.raw_path),
                                            cancellation);
        if (!text) {
            return std::unexpected(std::move(text.error()));
        }
        found.push_back({.raw_path = std::move(snapshot.raw_path), .text = std::move(*text)});
    }
    return found;
}


core::Result<DynamicSelected> Catalogue::select(const DynamicSelection& selection,
                                                const std::set<std::string>& exclude,
                                                const core::CancellationToken& cancellation) const {
    std::mt19937 random{std::random_device{}()};
    return select_dynamic(*this, selection, exclude, random, cancellation);
}

core::Result<persistence::LocalLibrary> LocalCatalogue::open() const {
    return persistence::LocalLibrary::open(database_);
}

core::Result<void> LocalCatalogue::prepare() const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return {};
}

core::Result<std::vector<std::string>>
LocalCatalogue::filter_paths(const query::CompiledTkq& compiled,
                             const core::CancellationToken& cancellation) const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->filter_paths(compiled, cancellation);
}

core::Result<std::optional<std::string>>
LocalCatalogue::artwork_source(const std::string& album_key,
                               const core::CancellationToken& cancellation) const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->artwork_source(album_key, cancellation);
}

core::Result<persistence::LibraryInventoryPage>
LocalCatalogue::inventory(const std::string& folder, const std::string& after,
                          const std::size_t limit) const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->inventory(folder, after, limit);
}

core::Result<persistence::LibraryFolder>
LocalCatalogue::folder(const std::string& raw_path, const core::CancellationToken& cancellation) const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->folder(raw_path, cancellation);
}

core::Result<std::size_t> LocalCatalogue::refresh(const std::vector<std::string>& raw_paths,
                                                  const core::CancellationToken& cancellation) {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    // The albums before -- one a write moved to another key, or a folder
    // deleted -- and after.
    auto before = observer_ ? library->albums_touching(raw_paths)
                            : core::Result<std::vector<std::string>>{};
    auto refreshed = library->refresh(raw_paths, cancellation);
    if (refreshed && observer_) {
        // After, for what is there: a track that moved to another album, a
        // cover written. What is gone said all it had to before.
        std::vector<std::string> present;
        for (const auto& path : raw_paths) {
            std::error_code ignored;
            if (std::filesystem::exists(std::filesystem::path{path}, ignored)) {
                present.push_back(path);
            }
        }
        auto after = library->albums_touching(present);
        Change change{.paths = raw_paths, .albums = {}, .everything = false};
        if (before) {
            change.albums = std::move(*before);
        }
        if (after) {
            change.albums.insert(change.albums.end(), after->begin(), after->end());
        }
        std::ranges::sort(change.albums);
        change.albums.erase(std::ranges::unique(change.albums).begin(), change.albums.end());
        changed(std::move(change));
    }
    return refreshed;
}

core::Result<std::vector<std::string>>
LocalCatalogue::rated_paths(const std::string& track_hash) const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->rated_paths(track_hash);
}

core::Result<std::size_t> LocalCatalogue::import_indexed_tag_ratings() {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->import_indexed_tag_ratings();
}

core::Result<std::vector<std::pair<std::string, unsigned>>>
LocalCatalogue::rated_tracks(const core::CancellationToken& cancellation) const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->rated_tracks(cancellation);
}

core::Result<std::vector<unsigned char>>
LocalCatalogue::artwork(const std::string& raw_path,
                        const core::CancellationToken& cancellation) const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    // In the library, or not read: a path missing from the index is an
    // error there, which is the answer here too.
    if (auto known = library->cached_tracks({raw_path}, cancellation); !known) {
        return std::unexpected(std::move(known.error()));
    }
    return formats::load_track_artwork(raw_path, cancellation);
}

core::Result<std::vector<persistence::LibraryTrackSnapshot>>
LocalCatalogue::cached_tracks(const std::vector<std::string>& raw_paths,
                              const core::CancellationToken& cancellation) const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->cached_tracks(raw_paths, cancellation);
}

core::Result<std::vector<std::optional<persistence::LibraryTrackSnapshot>>>
LocalCatalogue::described_tracks(const std::vector<std::string>& raw_paths,
                                 const core::CancellationToken& cancellation) const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->described_tracks(raw_paths, cancellation);
}

core::Result<std::vector<std::array<std::int64_t, 6>>>
LocalCatalogue::history_facts(const std::vector<persistence::LibraryHistorySource>& sources,
                              const core::CancellationToken& cancellation) const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->history_facts(sources, cancellation);
}

} // namespace trackknife::engine

namespace trackknife::engine {

core::Result<persistence::LibraryScanResult>
LocalCatalogue::scan(const core::CancellationToken& cancellation,
                     persistence::LibraryScanProgress& progress) {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    auto scanned = library->scan(cancellation, progress);
    if (scanned) {
        changed({.paths = {}, .albums = {}, .everything = true});
    }
    return scanned;
}

} // namespace trackknife::engine

namespace trackknife::engine {

core::Result<std::vector<persistence::LibraryRoot>> LocalCatalogue::roots() const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->roots();
}

core::Result<void> LocalCatalogue::add_root(const std::string& raw_path) {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    auto added = library->add_root(raw_path);
    if (added) {
        changed({.paths = {raw_path}, .albums = {}, .everything = true});
    }
    return added;
}

core::Result<void> LocalCatalogue::remove_root(const std::string& raw_path) {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    auto removed = library->remove_root(raw_path);
    if (removed) {
        changed({.paths = {raw_path}, .albums = {}, .everything = true});
    }
    return removed;
}

core::Result<persistence::LibraryPage>
LocalCatalogue::query(const persistence::LibraryQuery& request,
                      const core::CancellationToken& cancellation) const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    if (!request.view.empty()) {
        return views_->query(*library, request, cancellation);
    }
    return library->query(request, cancellation);
}

core::Result<std::vector<std::string>>
LocalCatalogue::paths(const persistence::LibraryQuery& request,
                      const core::CancellationToken& cancellation) const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    if (!request.view.empty()) {
        return views_->paths(*library, request, cancellation);
    }
    return library->paths(request, cancellation);
}

core::Result<persistence::LibraryPage>
LocalCatalogue::filter(const query::CompiledTkq& compiled, const std::size_t offset,
                       const std::size_t limit, const core::CancellationToken& cancellation) const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->filter(compiled, offset, limit, cancellation);
}

core::Result<std::string> LocalCatalogue::revision() const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->revision();
}

core::Result<std::vector<unsigned>>
LocalCatalogue::ratings(const std::vector<std::string>& hashes,
                        const core::CancellationToken& cancellation) const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->ratings(hashes, cancellation);
}

core::Result<void> LocalCatalogue::set_rating(const std::string& hash, const bool album,
                                              const unsigned rating) {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->set_rating(hash, album, rating);
}

} // namespace trackknife::engine
