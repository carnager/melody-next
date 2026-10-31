// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QDialog>
#include <QList>
#include <QPointer>
#include <QStringList>

class QAction;
class QMainWindow;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace trackknife::ui {

// ADR-0345: a command as the palette offers it -- an action and the menus it
// is found in, from the menu bar's down. A menu greyed out or hidden takes
// its commands with it.
struct PaletteCommand {
    QPointer<QAction> action;
    QStringList path;
    QList<QPointer<QAction>> menus;
};

// Every command of a window: its menu bar's, in every submenu, then the ones
// it holds itself (keys with no menu entry). Each once, where first found.
[[nodiscard]] QList<PaletteCommand> windowCommands(const QMainWindow& window);

class CommandPalette final : public QDialog {
    Q_OBJECT

  public:
    explicit CommandPalette(QList<PaletteCommand> commands, QWidget* parent = nullptr);
    explicit CommandPalette(const QList<QAction*>& actions, QWidget* parent = nullptr);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void rebuild();
    void updateSelection();
    void runCurrent();
    [[nodiscard]] QAction* currentAction() const;
    [[nodiscard]] bool available(const PaletteCommand& command) const;

    QList<PaletteCommand> commands_;
    QLineEdit* filter_{nullptr};
    QListWidget* results_{nullptr};
    QPushButton* run_{nullptr};
    QLabel* status_{nullptr};
    QDialogButtonBox* buttons_{nullptr};
};

} // namespace trackknife::ui
