// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QList>
#include <QWidget>
class QAction;
class QKeySequenceEdit;
class QLabel;
namespace trackknife::bench {
class ShortcutSettings final : public QWidget {
  public:
    explicit ShortcutSettings(const QList<QAction*>& actions, QWidget* parent = nullptr);
    bool apply();

  private:
    struct Binding {
        QAction* action;
        QKeySequenceEdit* edit;
    };
    QList<Binding> bindings_;
    QLabel* error_{};
};
} // namespace trackknife::bench
