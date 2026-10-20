// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "workspace/plugins.hpp"

#include <QDialog>
#include <QVariantMap>

namespace trackknife::bench {

// ADR-0300: what a plugin asks to be set, in a dialog of its own, opened
// from its Settings button on the Plugins page. A setting that needs an
// optional feature is greyed until that feature's toggle is ticked. What is
// set is the dialog's until OK; Settings' Save then writes it with the rest.
class PluginSettingsDialog final : public QDialog {
    Q_OBJECT

  public:
    // `current`: each setting's value now, by its key in the manifest.
    PluginSettingsDialog(const Plugin& plugin, QVariantMap current, QWidget* parent = nullptr);

    // Each setting's value as edited, by its key in the manifest.
    [[nodiscard]] const QVariantMap& values() const { return values_; }

  private:
    QVariantMap values_;
};

} // namespace trackknife::bench
