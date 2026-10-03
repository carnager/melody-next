// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/engine_key.hpp"
#include "trackknife/persistence/list_edits.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "trackknife/protocol/message.hpp"

#include <QObject>
#include <QPointer>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
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
// ADR-0256: a list is sent whole once, when it is made, and after that as
// edits. For each list the sync keeps what the engine acknowledged -- its
// revision and its entries, each with a fingerprint -- and sends the edits
// that turn that into what the window shows, in batches well under the
// engine's line limit. A reconnect sends nothing by itself: the engine's
// revisions are compared with the acknowledged ones.
//
// Out: fed from the window's own save. A working list (a scratch tab) is
// written as it changes and deleted with its tab. A saved list is written
// when saved; a write the engine refuses because someone else saved
// meanwhile is a conflict for the window to settle, and its unsaved edits
// stay in the window.
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

    // ADR-0234: the connection an engine's lists go to; null, it has none
    // (no longer, or not yet).
    void setEngine(const EngineKey& key, EnginePlayback* playback);
    // An engine known by one key is known by another from now: the remote
    // once it has said who it is. Its lists and pending removals go along.
    void rekey(const EngineKey& from, const EngineKey& to);
    // A list's entries as its engine stores them, read from the tab when the
    // sync needs them; none, it is gone.
    using ItemsOf = std::function<std::optional<std::vector<persistence::EngineListItem>>(
        const core::StableId& id)>;
    // After every save of the workspace: every list, with whether its items
    // changed since the last save. A list not among them was closed.
    void update(const std::vector<persistence::ListDocumentWrite>& documents,
                const ItemsOf& items_of);
    // ADR-0256: one list, sent now rather than at the next save, and `then`
    // told once the engine holds it as the window shows it -- true -- or that
    // it will not -- false: no engine, an engine too old for edits, unsaved
    // edits of a saved list, a conflict. What plays the list waits for this.
    void sendNow(const persistence::ListDocumentWrite& document, const ItemsOf& items_of,
                 std::function<void(bool)> then);
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
    documentFromAnswer(const protocol::Json& answer, const EngineKey& engine);
    // A list the window has just opened from its engine, as list.get gave
    // it: not sent back, since that is what it already is.
    void opened(const protocol::Json& answer, const EngineKey& engine);

    // A list entry as the engine stores it, and back: the wire form of
    // ADR-0233's item with ADR-0256's additions.
    [[nodiscard]] static protocol::Json itemJson(const persistence::EngineListItem& item);
    [[nodiscard]] static std::optional<persistence::EngineListItem>
    itemFromJson(const protocol::Json& value);
    // What a fingerprint of an entry covers: everything the engine stores.
    [[nodiscard]] static std::size_t fingerprint(const persistence::EngineListItem& item);

    // For tests: whether anything is on its way to or from an engine.
    [[nodiscard]] bool busy() const noexcept { return in_flight_ > 0; }

    // The most a single list write carries, in bytes of JSON: a quarter of
    // the engine's line limit, so nothing the window sends comes near it.
    static constexpr std::size_t batch_bytes = 256U * 1024U;

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
    // What the engine holds of a list, as far as this window knows.
    struct Acked {
        std::uint64_t revision{0};
        std::string name;
        bool working{true};
        std::vector<persistence::ListEntryPrint> entries;
    };
    struct Known {
        EngineKey engine;
        bool working{true};
        bool dirty{false};
        std::string name;
        // The window's entries at its last save; empty, not read yet.
        std::optional<std::vector<persistence::ListEntryPrint>> local;
        // Those entries whole, read with `local`, kept while there is
        // something to send: a long list goes in many batches, and the tab is
        // not read again for each.
        std::shared_ptr<const std::vector<persistence::EngineListItem>> items;
        std::optional<Acked> acked;
        // Compared: the engine has no such list, so it is made.
        bool missing{false};
        bool in_flight{false};
        bool fetching{false};
        // Waiting for the window to settle it -- and whether it has been asked.
        bool conflict{false};
        bool asked{false};
        // "Keep mine": written over whatever the engine has.
        bool overwrite{false};
        // Told once the engine holds the list as the window shows it.
        std::vector<std::function<void(bool)>> waiting;
    };
    struct Engine {
        QPointer<EnginePlayback> playback;
        bool compared{false};
        bool comparing{false};
        int outstanding{0};
        // Too old for list.edit: lists go whole, as before ADR-0256.
        bool whole_only{false};
    };
    // Why a fetch was made, which decides what its answer does.
    enum class Fetch : std::uint8_t {
        // Comparing on connecting: the engine's revision is not the one known.
        compare,
        // Another client changed it.
        elsewhere,
        // This window's write met a newer revision, and it writes again.
        rewrite,
    };

    [[nodiscard]] EnginePlayback* engineFor(const EngineKey& key) const;
    [[nodiscard]] Engine& stateFor(const EngineKey& key);
    [[nodiscard]] EngineKey keyOf(const EnginePlayback* playback) const;
    // Takes in one written document's header and, when it changed, its items.
    void take(const persistence::ListDocumentWrite& write, const ItemsOf& items_of);
    // Sends whatever the engine still lacks of one list.
    void sync(const std::string& id);
    // Tells those waiting on a list whether it is in step.
    static void settle(Known& known, bool ready);
    [[nodiscard]] static bool inStep(const Known& known);
    void compare(const EngineKey& key);
    void compared(const EngineKey& key);
    void fetch(const std::string& id, const EngineKey& key, Fetch why);
    void create(const std::string& id, Known& known,
                const std::vector<persistence::EngineListItem>& items);
    void edit(const std::string& id, Known& known,
              const std::vector<persistence::EngineListItem>& items);
    void sendWhole(const std::string& id, Known& known,
                   const std::vector<persistence::EngineListItem>& items);
    // A write's answer: `applied` takes a success into what is acknowledged.
    void written(const std::string& id, const core::Result<protocol::Json>& answer,
                 const std::function<void(Known&, const protocol::Json&)>& applied);
    void remove(const std::string& id, const EngineKey& key);
    void flushRemovals(const EngineKey& key);
    void storeRemovals() const;

    std::map<EngineKey, Engine> engines_;
    std::unordered_map<std::string, Known> known_;
    // Read from the tab when a list has to be sent, between saves.
    ItemsOf items_of_;
    int in_flight_{0};
    // Working lists closed here, by engine and id, until
    // their engine has deleted them; kept in Settings under this key.
    static constexpr auto removals_key = "lists/pending-removals";
    std::set<std::pair<EngineKey, std::string>> removals_;
    std::set<std::pair<EngineKey, std::string>> removing_;
};

} // namespace trackknife::bench
