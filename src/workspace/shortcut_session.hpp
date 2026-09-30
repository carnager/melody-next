// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QKeySequence>
#include <QObject>
#include <QString>

#include <vector>

namespace trackknife::bench {

// The keyboard shortcuts page: every command with a key by default, and
// every workspace command, given a key of the user's or none; saved by the
// command's object name, refused while two overlap. Both windows' pages
// draw it, and both windows' commands take their keys from saved().
class ShortcutSession final : public QObject {
    Q_OBJECT

  public:
    struct Command {
        QString id;
        QString label;
        QKeySequence key;
        QKeySequence default_key;
    };

    // The commands, in the order shown.
    explicit ShortcutSession(std::vector<Command> commands, QObject* parent = nullptr);

    // A command's key: the saved one, or its default.
    [[nodiscard]] static QKeySequence saved(const QString& id, const QKeySequence& default_key);

    [[nodiscard]] const std::vector<Command>& commands() const { return commands_; }
    [[nodiscard]] QString error() const { return error_; }

    void setKey(int row, const QKeySequence& key);
    void restoreDefaults();
    // Saved, unless two keys overlap (said by error()).
    [[nodiscard]] bool apply();

  signals:
    void changed();

  private:
    std::vector<Command> commands_;
    QString error_;
};

} // namespace trackknife::bench
