// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/engine_key.hpp"

#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <vector>

namespace trackknife::bench {

class Workspace;

// ADR-0233: the lists as a pane instead of a tab bar -- every engine's,
// under its name, whether open in this window or not: saved ones first, by
// name; what is open here as this window has it. Each can be opened,
// closed, saved, renamed or deleted from there. Both windows' lists panes
// draw it.
class ListsCatalog final : public QObject {
    Q_OBJECT

  public:
    struct List {
        QString id;
        QString name;
        bool saved{false};
        bool open{false};
        bool dirty{false};
        bool pinned{false};
        bool playing{false};
        // Unknown: -1.
        int tracks{-1};
    };
    struct Group {
        QString name;
        EngineKey engine;
        std::vector<List> lists;
        // Said in place of lists when there are none: an engine away, say.
        QString note;
    };

    explicit ListsCatalog(Workspace& work, QObject* parent = nullptr);

    [[nodiscard]] const std::vector<Group>& groups() const { return groups_; }
    // The engines are asked for their lists again (shortly, once for many
    // asks); what is open here is gathered again at once.
    void fetch();
    void present();

    void open(const EngineKey& engine, const QString& id);
    // Opened first when it is not open, then: its tab to close, save or
    // rename -- the window asks for a name as it would for its tab.
    void close(const EngineKey& engine, const QString& id);
    void save(const EngineKey& engine, const QString& id);
    // Renamed where it is: the tab open here, or the engine's list.
    void rename(const EngineKey& engine, const QString& id, const QString& name);
    // Deleted from its engine, closed here first; its files untouched.
    void remove(const EngineKey& engine, const QString& id);

  signals:
    void changed();
    // The window's own ways with the list now shown.
    void closeWanted(const QString& id);
    void saveWanted();

  private:
    Workspace& work_;
    std::vector<Group> groups_;
    QTimer fetch_timer_;
};

} // namespace trackknife::bench
