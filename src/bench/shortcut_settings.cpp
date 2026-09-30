// SPDX-License-Identifier: GPL-3.0-only
#include "bench/shortcut_settings.hpp"
#include "workspace/shortcut_session.hpp"
#include <QAction>
#include <QFormLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace trackknife::bench {
ShortcutSettings::ShortcutSettings(const QList<QAction*>& actions, QWidget* parent)
    : QWidget(parent), actions_(actions) {
    std::vector<ShortcutSession::Command> commands;
    for (auto* action : actions) {
        auto label = action->text();
        label.remove('&');
        commands.push_back({action->objectName(), label, action->shortcut(),
                            QKeySequence(action->property("shortcut-default").toString(),
                                         QKeySequence::PortableText)});
    }
    session_ = new ShortcutSession(std::move(commands), this);
    auto* layout = new QVBoxLayout(this);
    auto* note = new QLabel(
        tr("Click a shortcut and press the new keys. Clear it to disable it. Changes take effect "
           "on Save. Shortcuts operate within Trackknife, not across the desktop."),
        this);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(note);
    auto* form = new QFormLayout;
    for (int row = 0; const auto& command : session_->commands()) {
        auto* edit = new QKeySequenceEdit(command.key, this);
        edit->setObjectName(QStringLiteral("shortcut-") + command.id);
        edit->setAccessibleName(command.label);
        form->addRow(command.label, edit);
        connect(edit, &QKeySequenceEdit::keySequenceChanged, this,
                [this, row](const QKeySequence& key) { session_->setKey(row, key); });
        edits_.append(edit);
        ++row;
    }
    layout->addLayout(form);
    error_ = new QLabel(this);
    error_->setObjectName(QStringLiteral("shortcut-conflict"));
    error_->setWordWrap(true);
    layout->addWidget(error_);
    auto* reset = new QPushButton(tr("Restore defaults"), this);
    reset->setObjectName(QStringLiteral("shortcut-restore-defaults"));
    connect(reset, &QPushButton::clicked, session_, &ShortcutSession::restoreDefaults);
    layout->addWidget(reset);
    layout->addStretch();
    connect(session_, &ShortcutSession::changed, this, &ShortcutSettings::sync);
}

void ShortcutSettings::sync() {
    for (qsizetype row = 0; row < edits_.size(); ++row) {
        const auto& key = session_->commands()[static_cast<std::size_t>(row)].key;
        if (edits_[row]->keySequence() != key) {
            const QSignalBlocker blocker{edits_[row]};
            edits_[row]->setKeySequence(key);
        }
    }
    error_->setText(session_->error());
}

bool ShortcutSettings::apply() {
    if (!session_->apply()) {
        return false;
    }
    for (qsizetype row = 0; row < actions_.size(); ++row) {
        actions_[row]->setShortcut(session_->commands()[static_cast<std::size_t>(row)].key);
    }
    return true;
}
} // namespace trackknife::bench
