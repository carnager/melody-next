// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace trackknife::musicbrainz {

// ADR-0261: what grouping needs of one file -- its raw path and the tags it
// carries; any of them may be empty.
struct AlbumGroupInput {
    std::string raw_path;
    std::string release_id;
    std::string album_artist;
    std::string artist;
    std::string album;
    std::string date;

    friend bool operator==(const AlbumGroupInput&, const AlbumGroupInput&) = default;
};

enum class AlbumGroupBasis : std::uint8_t {
    // The MusicBrainz release id the files carry.
    release_id,
    // Album artist (else artist), album without a disc designator, and year.
    tags,
    // No album tag: the folder, with a disc folder ("CD 2") taken as its
    // parent's.
    folder,
};

struct AlbumGroup {
    AlbumGroupBasis basis{AlbumGroupBasis::folder};
    // What the files share, as grouped: "mbid:<id>", the tag key, or the
    // folder's raw path.
    std::string key;
    // Indexes into the input, in input order.
    std::vector<std::size_t> items;
    // The folders the files are in, each once, in the order first met.
    std::vector<std::string> folders;

    friend bool operator==(const AlbumGroup&, const AlbumGroup&) = default;
};

// The albums a selection holds, in the order each is first met: by release id
// where a file carries one, else by its tags, else by its folder. Pure and
// deterministic. A selection mixing tagged and untagged files of one folder
// gives two groups, as it should be looked at twice.
[[nodiscard]] std::vector<AlbumGroup> group_albums(std::span<const AlbumGroupInput> files);

// Whether a folder's name is a disc's of a release split into folders:
// "CD 2", "Disc 1", "disk3", case-insensitively.
[[nodiscard]] bool is_disc_folder(std::string_view name);

} // namespace trackknife::musicbrainz
