// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/musicbrainz_lookup.hpp"
#include "workspace/tagger_session.hpp"

#include <vector>

class QDialog;
class QWidget;

namespace trackknife::bench {

// The in-app identification surface (ADR-0090): text search needing no
// MusicBrainz id, every release version presented as a distinct candidate,
// and the chosen version staged into the tagger's draft as ADR-0086
// proposals. The dialog never writes anything itself. The release the files
// are tagged with is looked up as it opens (ADR-0310). Several albums are
// identified one at a time, beside a list of them (ADR-0311).
[[nodiscard]] QDialog* createMusicBrainzIdentifyDialog(TaggerSession& tagger,
                                                       MusicBrainzLookupService service,
                                                       std::vector<TaggerSession::Identify> albums,
                                                       QWidget* parent);

} // namespace trackknife::bench
