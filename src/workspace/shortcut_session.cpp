// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/shortcut_session.hpp"

#include <QSettings>

#include <utility>

namespace trackknife::bench {
namespace {

[[nodiscard]] QString settingsKey(const QString& id) { return QStringLiteral("shortcuts/") + id; }

} // namespace

ShortcutSession::ShortcutSession(std::vector<Command> commands, QObject* parent)
    : QObject(parent), commands_(std::move(commands)) {}

QKeySequence ShortcutSession::saved(const QString& id, const QKeySequence& default_key) {
    const QSettings settings;
    return settings.contains(settingsKey(id))
               ? QKeySequence(settings.value(settingsKey(id)).toString(),
                              QKeySequence::PortableText)
               : default_key;
}

void ShortcutSession::setKey(const int row, const QKeySequence& key) {
    if (row < 0 || row >= static_cast<int>(commands_.size())) {
        return;
    }
    commands_[static_cast<std::size_t>(row)].key = key;
    emit changed();
}

void ShortcutSession::restoreDefaults() {
    for (auto& command : commands_) {
        command.key = command.default_key;
    }
    error_.clear();
    emit changed();
}

bool ShortcutSession::apply() {
    const auto overlaps = [](const QKeySequence& a, const QKeySequence& b) {
        return !a.isEmpty() && !b.isEmpty() &&
               (a.matches(b) != QKeySequence::NoMatch || b.matches(a) != QKeySequence::NoMatch);
    };
    for (std::size_t i = 0; i < commands_.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (overlaps(commands_[i].key, commands_[j].key)) {
                error_ = tr("Conflicting shortcuts: %1 and %2")
                             .arg(commands_[i].label, commands_[j].label);
                emit changed();
                return false;
            }
        }
    }
    QSettings settings;
    for (const auto& command : commands_) {
        settings.setValue(settingsKey(command.id),
                          command.key.toString(QKeySequence::PortableText));
    }
    error_.clear();
    emit changed();
    return true;
}

} // namespace trackknife::bench
