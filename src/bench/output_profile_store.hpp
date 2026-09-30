// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/engine_folder_listing.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/persistence/list_repository.hpp"

#include <QString>

#include <functional>
#include <string>
#include <vector>

namespace trackknife::bench {

// The asynchronous persistence seam shared by the tag editor (profile
// selection) and the Settings screen (profile management). ADR-0185.
// ADR-0237: the move destinations of one engine -- folders on its machine --
// and how a folder there is chosen.
struct DestinationPlace {
    using LoadCompletion =
        std::function<void(std::vector<persistence::SavedDestinationProfile>, QString)>;
    using Completion = std::function<void(QString)>;

    QString key;  // the engine's, as EngineKey spells it
    QString name; // as shown: "this computer", or the engine's name
    std::function<void(LoadCompletion)> load;
    std::function<void(persistence::SavedDestinationProfile, Completion)> save;
    std::function<void(core::StableId, Completion)> remove;
    // Lists that engine's folders; empty for this computer, whose file
    // dialog is used.
    EngineFolderLister folders;
    // This computer's destinations that lie under the engine's mount, as
    // that engine names them, for copying there; empty when there is none.
    std::function<std::vector<persistence::SavedDestinationProfile>()> copyable;
};

struct OutputProfileStore {
    using LoadCompletion =
        std::function<void(std::vector<persistence::SavedOutputLayoutProfile>,
                           std::vector<persistence::SavedDestinationProfile>, QString)>;
    using Completion = std::function<void(QString)>;

    // Naming layouts, and the move destinations of the engine `destinations_on`
    // names (empty: this computer).
    std::function<void(LoadCompletion)> load;
    std::function<void(persistence::SavedOutputLayoutProfile, Completion)> save_layout;
    std::function<void(core::StableId, Completion)> remove_layout;
    std::function<void(persistence::SavedDestinationProfile, Completion)> save_destination;
    std::function<void(core::StableId, Completion)> remove_destination;
    QString destinations_on;
    // That engine's key, as EngineKey spells it: what a destination chosen
    // for it is remembered under.
    QString destinations_key;
    // For the manager: every engine whose destinations can be managed, this
    // computer first. Empty: only the destinations above.
    std::vector<DestinationPlace> places;
};

} // namespace trackknife::bench
