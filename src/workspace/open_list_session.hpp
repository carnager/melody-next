// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/engine_key.hpp"

#include <QObject>
#include <QString>

#include <vector>

namespace trackknife::bench {

class Workspace;

// "Open list": every engine's lists under its name, saved ones first --
// they are the ones kept for a reason -- as each engine answers; one opened
// into a tab. The window's Open list dialog draws it.
class OpenListSession final : public QObject {
    Q_OBJECT

  public:
    struct List {
        QString id;
        QString label;
        int tracks{0};
    };
    struct Group {
        QString name;
        EngineKey engine;
        std::vector<List> lists;
        // Said instead of lists, when there are none.
        QString note;
        bool answered{false};
    };

    explicit OpenListSession(Workspace& work, QObject* parent = nullptr);

    [[nodiscard]] const std::vector<Group>& groups() const { return groups_; }
    void open(int group, int row);

  signals:
    void changed();
    // Opened: the dialog is done.
    void opened();

  private:
    Workspace& work_;
    std::vector<Group> groups_;
};

} // namespace trackknife::bench
