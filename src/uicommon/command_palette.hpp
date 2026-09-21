// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QDialog>
#include <QPointer>

class QAction;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace trackknife::ui {

class CommandPalette final : public QDialog {
    Q_OBJECT

  public:
    explicit CommandPalette(QList<QAction*> actions, QWidget* parent = nullptr);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void rebuild();
    void updateSelection();
    void runCurrent();
    [[nodiscard]] QAction* currentAction() const;

    QList<QPointer<QAction>> actions_;
    QLineEdit* filter_{nullptr};
    QListWidget* results_{nullptr};
    QPushButton* run_{nullptr};
    QLabel* status_{nullptr};
    QDialogButtonBox* buttons_{nullptr};
};

} // namespace trackknife::ui
