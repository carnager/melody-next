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
