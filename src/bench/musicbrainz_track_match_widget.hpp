// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/musicbrainz/proposal_bridge.hpp"

#include <QString>
#include <QWidget>

#include <functional>
#include <vector>

namespace trackknife::bench {

class TrackMatchSession;

// Pairing files with a release's tracks, over a session of its own.
[[nodiscard]] QWidget* createMusicBrainzTrackMatchWidget(
    musicbrainz::Release release, std::vector<musicbrainz::LocalTrackDescriptor> local_tracks,
    std::vector<QString> local_paths, std::vector<std::size_t> item_indexes,
    std::function<void(metadata::MetadataProposalSet)> accepted, std::function<void()> back,
    QWidget* parent);

// The same, over a session someone else keeps -- the Identify window's.
[[nodiscard]] QWidget* createMusicBrainzTrackMatchView(TrackMatchSession* session,
                                                       std::function<void()> back, QWidget* parent);

} // namespace trackknife::bench
