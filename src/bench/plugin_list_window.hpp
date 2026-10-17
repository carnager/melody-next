// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "workspace/plugins.hpp"

#include <QDialog>

class QListWidget;

namespace trackknife::bench {

// ADR-0288: what a `list` action answered, in a window of its own -- a
// heading, a row for each item with its detail beneath, opened in the
// browser by a double click where it has a link. It does not hold the
// window up.
class PluginListWindow final : public QDialog {
    Q_OBJECT

  public:
    PluginListWindow(const QString& plugin, const PluginList& list, QWidget* parent = nullptr);

    // The rows' text, a line each, as Copy puts it on the clipboard.
    [[nodiscard]] QString text() const;

  private:
    QListWidget* rows_{nullptr};
    PluginList list_;
};

} // namespace trackknife::bench
