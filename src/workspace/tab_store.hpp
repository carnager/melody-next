// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "trackknife/core/stable_id.hpp"
#include "trackknife/persistence/list_repository.hpp"

#include <QByteArray>
#include <QString>

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace trackknife::bench {

// ADR-0259: what of its tabs a window keeps -- not the lists, which are their
// engines'. Which lists are open and how, in the window's settings; and a
// cache of each tab's rows, so it opens at once and shows what it held while
// its engine is away. The cache is never sent anywhere, and losing it costs
// a tab that is blank until its engine answers.
class TabStore {
  public:
    struct Tab {
        core::StableId id;
        // EngineKey::stored(): the engine whose list it is.
        std::string engine;
        // As last known, for a tab whose cache is gone.
        std::string name;
        persistence::ListKind kind{persistence::ListKind::scratch};
        bool pinned{false};
        // A saved list with edits not saved yet.
        bool dirty{false};
        QByteArray layout;

        friend bool operator==(const Tab&, const Tab&) = default;
    };
    struct State {
        std::vector<Tab> tabs;
        std::optional<core::StableId> active;

        friend bool operator==(const State&, const State&) = default;
    };

    // Rows are cached under `cache_directory`; empty, the window's own.
    explicit TabStore(QString cache_directory = {});
    ~TabStore();
    TabStore(const TabStore&) = delete;
    TabStore& operator=(const TabStore&) = delete;

    // Whether this window has kept its tabs here yet; before, they are in
    // the database it kept lists in (the migration).
    [[nodiscard]] static bool hasState();
    [[nodiscard]] static State loadState();
    static void saveState(const State& state);

    // Written on a thread of its own, the latest of a tab's rows only.
    void writeRows(persistence::ListDocument document);
    // Any thread; nothing for a tab with no cache, or one unreadable.
    [[nodiscard]] std::optional<persistence::ListDocument> readRows(const core::StableId& id) const;
    // Every cache but those of `keep`.
    void dropAllBut(const std::vector<core::StableId>& keep);
    // Until what was asked to be written is.
    void flush();

    [[nodiscard]] QString directory() const { return directory_; }

  private:
    [[nodiscard]] QString pathOf(const core::StableId& id) const;
    void work();

    QString directory_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::map<std::string, persistence::ListDocument> pending_;
    bool writing_{false};
    bool stopping_{false};
    std::thread writer_;
};

} // namespace trackknife::bench
