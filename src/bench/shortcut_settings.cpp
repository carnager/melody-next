// SPDX-License-Identifier: GPL-3.0-only
#include "bench/shortcut_settings.hpp"
#include <QAction>
#include <QFormLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace trackknife::bench {
ShortcutSettings::ShortcutSettings(const QList<QAction*>& actions, QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    auto* note = new QLabel(
        tr("Click a shortcut and press the new keys. Clear it to disable it. Changes take effect "
           "on Save. Shortcuts operate within Trackknife, not across the desktop."),
        this);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(note);
    auto* form = new QFormLayout;
    for (auto* action : actions) {
        auto* edit = new QKeySequenceEdit(action->shortcut(), this);
        edit->setObjectName(QStringLiteral("shortcut-") + action->objectName());
        auto label = action->text();
        label.remove('&');
        edit->setAccessibleName(label);
        form->addRow(label, edit);
        bindings_.append({action, edit});
    }
    layout->addLayout(form);
    error_ = new QLabel(this);
    error_->setObjectName(QStringLiteral("shortcut-conflict"));
    error_->setWordWrap(true);
    layout->addWidget(error_);
    auto* reset = new QPushButton(tr("Restore defaults"), this);
    reset->setObjectName(QStringLiteral("shortcut-restore-defaults"));
    connect(reset, &QPushButton::clicked, this, [this] {
        for (const auto& binding : bindings_)
            binding.edit->setKeySequence(
                QKeySequence(binding.action->property("shortcut-default").toString(),
                             QKeySequence::PortableText));
        error_->clear();
    });
    layout->addWidget(reset);
    layout->addStretch();
}

bool ShortcutSettings::apply() {
    const auto overlaps = [](const QKeySequence& a, const QKeySequence& b) {
        return !a.isEmpty() && !b.isEmpty() &&
               (a.matches(b) != QKeySequence::NoMatch || b.matches(a) != QKeySequence::NoMatch);
    };
    for (qsizetype i = 0; i < bindings_.size(); ++i) {
        const auto key = bindings_[i].edit->keySequence();
        for (qsizetype j = 0; j < i; ++j) {
            if (overlaps(key, bindings_[j].edit->keySequence())) {
                error_->setText(tr("Conflicting shortcuts: %1 and %2")
                                    .arg(bindings_[i].action->text(), bindings_[j].action->text()));
                return false;
            }
        }
    }
    QSettings settings;
    for (const auto& binding : bindings_) {
        const auto key = binding.edit->keySequence();
        binding.action->setShortcut(key);
        settings.setValue(QStringLiteral("shortcuts/") + binding.action->objectName(),
                          key.toString(QKeySequence::PortableText));
    }
    error_->clear();
    return true;
}
} // namespace trackknife::bench
