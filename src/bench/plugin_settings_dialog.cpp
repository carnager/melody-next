// SPDX-License-Identifier: GPL-3.0-only

#include "bench/plugin_settings_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>
#include <vector>

namespace trackknife::bench {
namespace {

// What is said of a setting beneath it: its help, and its page as a link.
[[nodiscard]] QLabel* caption(const PluginSetting& setting, QWidget* parent) {
    QStringList lines;
    if (!setting.help.isEmpty()) {
        lines << setting.help.toHtmlEscaped();
    }
    if (!setting.link.isEmpty()) {
        const QUrl url{setting.link};
        auto shown = url.host() + url.path();
        if (shown.endsWith(QLatin1Char('/'))) {
            shown.chop(1);
        }
        lines << QStringLiteral("<a href=\"%1\">%2</a>")
                     .arg(setting.link.toHtmlEscaped(), shown.toHtmlEscaped());
    }
    if (lines.isEmpty()) {
        return nullptr;
    }
    auto* label = new QLabel(lines.join(QStringLiteral("<br>")), parent);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setForegroundRole(QPalette::PlaceholderText);
    label->setOpenExternalLinks(true);
    label->setTextInteractionFlags(Qt::TextBrowserInteraction);
    return label;
}

} // namespace

PluginSettingsDialog::PluginSettingsDialog(const Plugin& plugin, QVariantMap current,
                                           QWidget* parent)
    : QDialog(parent), values_(std::move(current)) {
    setObjectName(QStringLiteral("bench-plugin-settings-%1").arg(plugin.name));
    setWindowTitle(QStringLiteral("%1 Settings").arg(plugin.title));
    auto* layout = new QVBoxLayout(this);
    if (!plugin.description.isEmpty()) {
        auto* about = new QLabel(plugin.description, this);
        about->setTextFormat(Qt::PlainText);
        about->setWordWrap(true);
        about->setForegroundRole(QPalette::PlaceholderText);
        layout->addWidget(about);
    }

    // Settings in their order, except that what needs an optional feature
    // is beneath that feature's toggle, indented, and greyed with it off.
    // Beneath a toggle is beneath its text.
    const auto toggle_indent = style()->pixelMetric(QStyle::PM_IndicatorWidth) +
                               style()->pixelMetric(QStyle::PM_CheckBoxLabelSpacing);
    // Each field's label, and how far in its form is: the fields line up.
    std::vector<std::pair<QLabel*, int>> labels;
    QFormLayout* form = nullptr;
    const auto formIn = [&form, layout]() {
        if (form == nullptr) {
            form = new QFormLayout;
            layout->addLayout(form);
        }
        return form;
    };
    const auto addField = [this, &plugin, &labels, toggle_indent](const PluginSetting& setting,
                                                                  QFormLayout* into, QWidget* owner,
                                                                  const int indent) -> QCheckBox* {
        const auto name =
            QStringLiteral("bench-settings-plugin-%1-%2").arg(plugin.name, setting.key);
        const auto value = values_.value(setting.key, setting.fallback);
        QWidget* field = nullptr;
        QCheckBox* toggle = nullptr;
        switch (setting.kind) {
        case PluginSetting::Kind::text:
        case PluginSetting::Kind::secret: {
            auto* edit = new QLineEdit(value.toString(), owner);
            if (setting.kind == PluginSetting::Kind::secret) {
                edit->setEchoMode(QLineEdit::Password);
            } else {
                edit->setPlaceholderText(setting.fallback.toString());
            }
            connect(edit, &QLineEdit::textEdited, this,
                    [this, key = setting.key](const QString& text) { values_[key] = text; });
            field = edit;
            break;
        }
        case PluginSetting::Kind::toggle: {
            toggle = new QCheckBox(setting.label, owner);
            toggle->setChecked(value.toBool());
            connect(toggle, &QCheckBox::toggled, this,
                    [this, key = setting.key](const bool on) { values_[key] = on; });
            field = toggle;
            break;
        }
        case PluginSetting::Kind::choice: {
            auto* combo = new QComboBox(owner);
            combo->addItems(setting.choices);
            combo->setCurrentText(value.toString());
            connect(combo, &QComboBox::currentTextChanged, this,
                    [this, key = setting.key](const QString& text) { values_[key] = text; });
            field = combo;
            break;
        }
        }
        field->setObjectName(name);
        field->setToolTip(setting.help);
        // A toggle is its own label.
        if (toggle != nullptr) {
            into->addRow(toggle);
        } else {
            auto* label = new QLabel(setting.label + QLatin1Char(':'), owner);
            label->setBuddy(field);
            labels.emplace_back(label, indent);
            into->addRow(label, field);
        }
        if (auto* said = caption(setting, owner); said != nullptr) {
            said->setObjectName(name + QStringLiteral("-caption"));
            if (toggle != nullptr) {
                said->setContentsMargins(toggle_indent, 0, 0, 0);
                into->addRow(said);
            } else {
                into->addRow(QString{}, said);
            }
        }
        return toggle;
    };
    for (const auto& setting : plugin.settings) {
        if (!setting.needs.isEmpty()) {
            continue;
        }
        auto* toggle = addField(setting, formIn(), this, 0);
        const auto needing = std::ranges::any_of(
            plugin.settings, [&setting](const auto& other) { return other.needs == setting.key; });
        if (toggle == nullptr || !needing) {
            continue;
        }
        auto* options = new QWidget(this);
        options->setObjectName(
            QStringLiteral("bench-settings-plugin-%1-%2-options").arg(plugin.name, setting.key));
        auto* options_form = new QFormLayout(options);
        options_form->setContentsMargins(toggle_indent, 0, 0, 0);
        for (const auto& dependent : plugin.settings) {
            if (dependent.needs == setting.key) {
                addField(dependent, options_form, options, toggle_indent);
            }
        }
        layout->addWidget(options);
        options->setEnabled(toggle->isChecked());
        connect(toggle, &QCheckBox::toggled, options, &QWidget::setEnabled);
        // What follows is in a form of its own, beneath these.
        form = nullptr;
    }
    int widest = 0;
    for (const auto& [label, indent] : labels) {
        widest = std::max(widest, label->sizeHint().width() + indent);
    }
    for (const auto& [label, indent] : labels) {
        label->setMinimumWidth(widest - indent);
    }
    layout->addStretch(1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("bench-plugin-settings-buttons"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    resize(std::max(sizeHint().width(), 440), sizeHint().height());
}

} // namespace trackknife::bench
