// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/persistence/list_repository.hpp"
#include "trackknife/protocol/message.hpp"

#include <QObject>
#include <QPointer>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace trackknife::bench {

class EnginePlayback;

// ADR-0233: every list this window has open, on the engine that owns its
// files -- a remote tab's on the remote engine, the rest on this computer's --
// and kept in step with it, so the phone, the CLI and other windows share
// them.
//
// Out: fed from the window's own save, whatever changed is sent. A working
// list (a scratch tab) is written as it changes and deleted with its tab. A
// saved list is written when saved, with the revision it was read at, and a
// write the engine refuses because someone else saved meanwhile is a
// conflict for the window to settle; its unsaved edits stay in the window.
//
// In: a list another client changed is taken up -- unless it is a saved one
// with unsaved edits here, which then conflicts when saved. A list another
// client deleted closes. And when an engine connects, the lists are compared
// with it first, so a window's older copy never overwrites what changed
// while it was closed; what the engine does not have is given to it.
class EngineListSync final : public QObject {
    Q_OBJECT

  public:
    explicit EngineListSync(QObject* parent = nullptr);

    // The connections lists go to. Either may be null: no remote engine, or
    // none yet.
    void setEngines(EnginePlayback* local, EnginePlayback* remote);
    // After every save of the workspace.
    void update(const std::vector<persistence::ListDocument>& documents);
    // An engine connected, or connected again: compared anew before anything
    // more is sent to it.
    void reconnected(const EnginePlayback* engine);
    // An engine said a list changed.
    void listChanged(const EnginePlayback* engine, const QString& id, quint64 revision,
                     bool deleted);

    // Settling a conflict: write this window's version regardless, or take
    // the engine's.
    void keepMine(const QString& id);
    void takeTheirs(const QString& id);

    // A list as list.get answers it, in the window's terms; empty for an
    // answer that is not one.
    [[nodiscard]] static std::optional<persistence::ListDocument>
    documentFromAnswer(const protocol::Json& answer, bool remote);
    // A list the window has just opened from its engine, as it is there: not
    // sent back, since that is what it already is.
    void opened(const persistence::ListDocument& document, std::uint64_t revision);

    // For tests: whether anything is on its way to or from an engine.
    [[nodiscard]] bool busy() const noexcept { return in_flight_ > 0; }

  signals:
    // The engine's version of a list open here, to be shown in its place.
    void adopted(const persistence::ListDocument& document);
    // Deleted by another client.
    void removedElsewhere(const QString& id);
    // A saved list changed on its engine since this window read it, and this
    // window has changes of its own to it.
    void conflicted(const QString& id);
    // Something is ready to be sent: the window should save.
    void wantsSave();

  private:
    struct Known {
        // What was last sent or taken up; empty, nothing yet.
        std::optional<std::size_t> fingerprint;
        // The window's version at its last save.
        std::optional<std::size_t> local;
        bool working{true};
        bool remote{false};
        bool dirty{false};
        bool in_flight{false};
        // Changed again while being sent: sent again when the answer comes.
        bool again{false};
        // Waiting for the window to settle it -- and whether it has been asked.
        bool conflict{false};
        bool asked{false};
        // The engine's revision of what this window last read or wrote.
        std::optional<std::uint64_t> revision;
    };
    struct Engine {
        bool compared{false};
        bool comparing{false};
        int outstanding{0};
    };

    [[nodiscard]] EnginePlayback* engineFor(bool remote) const;
    [[nodiscard]] Engine& stateFor(bool remote) { return remote ? remote_state_ : local_state_; }
    void compare(bool remote);
    void compared(bool remote);
    void fetch(const std::string& id, bool remote, bool comparing);
    void send(const persistence::ListDocument& document, std::size_t fingerprint);
    void remove(const std::string& id, bool remote);
    void flushRemovals(bool remote);
    void storeRemovals() const;

    QPointer<EnginePlayback> local_;
    QPointer<EnginePlayback> remote_;
    Engine local_state_;
    Engine remote_state_;
    std::unordered_map<std::string, Known> known_;
    // The newest of each document asked to be sent while one was in flight.
    std::unordered_map<std::string, persistence::ListDocument> waiting_;
    int in_flight_{0};
    // Working lists closed here, by engine (remote or not) and id, until
    // their engine has deleted them; kept in Settings under this key.
    static constexpr auto removals_key = "lists/pending-removals";
    std::set<std::pair<bool, std::string>> removals_;
    std::set<std::pair<bool, std::string>> removing_;
};

} // namespace trackknife::bench
