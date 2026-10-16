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

// ADR-0286: the window's tabs -- which are open and how, in its settings;
// and each tab's rows, kept here. A tab that is no saved playlist is kept
// nowhere else: this is its only copy, so it lives with the window's data,
// not in a cache, a file that cannot be read is set aside and said rather
// than forgotten, and the workspace backup takes the tabs too. A saved
// playlist's rows here open it at once; its engine has it as well.
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

    // Rows are kept under `directory`; empty, the window's own
    // (defaultDirectory, where those kept in its cache before are moved).
    explicit TabStore(QString directory = {});
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
    // Any thread; nothing for a tab with no rows kept. One whose file cannot
    // be read is set aside beside it (".unreadable") and named by
    // takeUnreadable, for the window to say.
    [[nodiscard]] std::optional<persistence::ListDocument> readRows(const core::StableId& id) const;
    [[nodiscard]] std::vector<QString> takeUnreadable();
    // Every cache but those of `keep`.
    void dropAllBut(const std::vector<core::StableId>& keep);
    // Until what was asked to be written is.
    void flush();

    [[nodiscard]] QString directory() const { return directory_; }
    // AppDataLocation/tabs: with the window's data.
    [[nodiscard]] static QString defaultDirectory();
    // ADR-0286: the rows of every tab copied into `destination`, a folder
    // made for them -- the workspace backup's.
    [[nodiscard]] bool copyTo(const QString& destination);
    // The tabs of a backup put in place of these, which are kept beside them
    // ("tabs-before-restore-…"). Before any TabStore is made.
    [[nodiscard]] static bool restoreFrom(const QString& source);

  private:
    [[nodiscard]] QString pathOf(const core::StableId& id) const;
    void setAside(const QString& path) const;
    void work();

    QString directory_;
    mutable std::mutex mutex_;
    // Files set aside by readRows, not said yet.
    mutable std::mutex unreadable_mutex_;
    mutable std::vector<QString> unreadable_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::map<std::string, persistence::ListDocument> pending_;
    bool writing_{false};
    bool stopping_{false};
    std::thread writer_;
};

} // namespace trackknife::bench
