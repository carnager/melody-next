// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/musicbrainz/proposal_bridge.hpp"

#include <QString>
#include <QWidget>

#include <functional>
#include <vector>

namespace trackknife::bench {

[[nodiscard]] QWidget* createMusicBrainzTrackMatchWidget(
    musicbrainz::Release release, std::vector<musicbrainz::LocalTrackDescriptor> local_tracks,
    std::vector<QString> local_paths, std::vector<std::size_t> item_indexes,
    std::function<void(metadata::MetadataProposalSet)> accepted, std::function<void()> back,
    QWidget* parent);

} // namespace trackknife::bench
