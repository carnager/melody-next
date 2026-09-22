// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/catalogue.hpp"

namespace trackknife::engine {

core::Result<persistence::LocalLibrary> Catalogue::open() const {
    return persistence::LocalLibrary::open(database_);
}

core::Result<std::vector<std::string>>
Catalogue::filter_paths(const query::CompiledTkq& compiled,
                        const core::CancellationToken& cancellation) const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->filter_paths(compiled, cancellation);
}

core::Result<std::optional<std::string>>
Catalogue::artwork_source(const std::string& album_key,
                          const core::CancellationToken& cancellation) const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->artwork_source(album_key, cancellation);
}

core::Result<std::vector<persistence::LibraryTrackSnapshot>>
Catalogue::cached_tracks(const std::vector<std::string>& raw_paths,
                         const core::CancellationToken& cancellation) const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->cached_tracks(raw_paths, cancellation);
}

core::Result<std::vector<std::array<std::int64_t, 6>>>
Catalogue::history_facts(const std::vector<persistence::LibraryHistorySource>& sources,
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
Catalogue::scan(const core::CancellationToken& cancellation,
                persistence::LibraryScanProgress& progress) {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->scan(cancellation, progress);
}

} // namespace trackknife::engine

namespace trackknife::engine {

core::Result<std::vector<persistence::LibraryRoot>> Catalogue::roots() const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->roots();
}

core::Result<void> Catalogue::add_root(const std::string& raw_path) {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->add_root(raw_path);
}

core::Result<void> Catalogue::remove_root(const std::string& raw_path) {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->remove_root(raw_path);
}

core::Result<persistence::LibraryPage>
Catalogue::query(const persistence::LibraryQuery& request,
                 const core::CancellationToken& cancellation) const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->query(request, cancellation);
}

core::Result<std::vector<std::string>>
Catalogue::paths(const persistence::LibraryQuery& request,
                 const core::CancellationToken& cancellation) const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->paths(request, cancellation);
}

core::Result<persistence::LibraryPage>
Catalogue::filter(const query::CompiledTkq& compiled, const std::size_t offset,
                  const std::size_t limit, const core::CancellationToken& cancellation) const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->filter(compiled, offset, limit, cancellation);
}

core::Result<std::vector<unsigned>>
Catalogue::ratings(const std::vector<std::string>& hashes,
                   const core::CancellationToken& cancellation) const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->ratings(hashes, cancellation);
}

core::Result<void> Catalogue::set_rating(const std::string& hash, const bool album,
                                         const unsigned rating) {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->set_rating(hash, album, rating);
}

} // namespace trackknife::engine
