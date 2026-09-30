// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/musicbrainz_lookup.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/proposal.hpp"
#include "trackknife/musicbrainz/acoustid.hpp"
#include "trackknife/musicbrainz/matching.hpp"

#include <QByteArray>
#include <QString>

#include <cstddef>
#include <functional>
#include <vector>

class QDialog;
class QWidget;

namespace trackknife::bench {

// The in-app identification surface (ADR-0090): text search needing no
// MusicBrainz id, every release version presented as a distinct candidate,
// and the chosen version delivered as ADR-0086 proposals for ordinary
// colored draft staging. The dialog never writes anything itself.
[[nodiscard]] QDialog* createMusicBrainzIdentifyDialog(
    MusicBrainzLookupService service, std::vector<musicbrainz::LocalTrackDescriptor> local_tracks,
    std::vector<QString> local_paths, std::vector<std::size_t> item_indexes, QString initial_artist,
    QString initial_release, std::function<void(metadata::MetadataProposalSet)> accepted,
    QWidget* parent);

} // namespace trackknife::bench
