// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/catalogue.hpp"

namespace trackknife::engine {

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

core::Result<std::vector<persistence::LibraryTrackSnapshot>>
LocalCatalogue::cached_tracks(const std::vector<std::string>& raw_paths,
                              const core::CancellationToken& cancellation) const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->cached_tracks(raw_paths, cancellation);
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
    return library->scan(cancellation, progress);
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
    return library->add_root(raw_path);
}

core::Result<void> LocalCatalogue::remove_root(const std::string& raw_path) {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
    }
    return library->remove_root(raw_path);
}

core::Result<persistence::LibraryPage>
LocalCatalogue::query(const persistence::LibraryQuery& request,
                      const core::CancellationToken& cancellation) const {
    auto library = open();
    if (!library) {
        return std::unexpected(std::move(library.error()));
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
