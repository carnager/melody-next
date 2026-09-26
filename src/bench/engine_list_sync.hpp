// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/persistence/list_repository.hpp"

#include <QObject>
#include <QPointer>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace trackknife::bench {

class EnginePlayback;

// ADR-0233: every list this window has open, on the engine that owns its
// files -- a remote tab's on the remote engine, the rest on this computer's --
// so the phone, the CLI and other windows see them.
//
// Fed from the window's own save: after each, whatever changed is sent. A
// working list (a scratch tab) is written as it changes, and deleted from its
// engine when its tab is gone. A saved list is written as saved -- but not
// while it has unsaved edits, which stay in this window until Save, as they
// always have. An engine that is away, or connects anew, is given its lists
// the next time the window saves.
//
// This is the engine's copy, not yet its authority: nothing here reads a list
// back from an engine.
class EngineListSync final : public QObject {
    Q_OBJECT

  public:
    explicit EngineListSync(QObject* parent = nullptr);

    // The connections lists go to. Either may be null: no remote engine, or
    // none yet.
    void setEngines(EnginePlayback* local, EnginePlayback* remote);
    // After every save of the workspace.
    void update(const std::vector<persistence::ListDocument>& documents);
    // An engine connected -- a restart, perhaps with nothing of this window's
    // -- so its lists are all sent again on the next update.
    void forget(const EnginePlayback* engine);

    // For tests: whether anything is on its way to an engine.
    [[nodiscard]] bool busy() const noexcept { return in_flight_ > 0; }

  private:
    struct Known {
        // What was last sent, or is being sent; empty, nothing yet.
        std::optional<std::size_t> fingerprint;
        bool working{true};
        bool remote{false};
        bool in_flight{false};
        // Changed again while being sent: sent again when the answer comes.
        bool again{false};
        // The revision the engine gave the last write.
        std::optional<std::uint64_t> revision;
    };

    [[nodiscard]] EnginePlayback* engineFor(bool remote) const;
    void send(const persistence::ListDocument& document, std::size_t fingerprint);
    void remove(const std::string& id, bool remote);

    QPointer<EnginePlayback> local_;
    QPointer<EnginePlayback> remote_;
    std::unordered_map<std::string, Known> known_;
    // The newest of each document asked to be sent while one was in flight.
    std::unordered_map<std::string, persistence::ListDocument> waiting_;
    int in_flight_{0};
};

} // namespace trackknife::bench
