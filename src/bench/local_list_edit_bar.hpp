// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "trackknife/lists/edit_plan.hpp"
#include "workspace/list_edit_job.hpp"

#include <QPointer>
#include <QToolBar>

class QComboBox;
class QLabel;
class QLineEdit;
class QTableView;
namespace trackknife::bench {
class LocalListModel;
// The edit bar under the lists: a ListEditJob's expression, direction and
// progress, as the widgets window shows them.
class LocalListEditBar final : public QToolBar {
    Q_OBJECT
  public:
    explicit LocalListEditBar(QWidget* parent = nullptr);
    void setView(QTableView* view);
    void openSort();
    void start(lists::EditRequest request);
    void cancel();
    [[nodiscard]] bool busy() const noexcept { return job_.active(); }
  signals:
    void edited(LocalListModel* model);

  private:
    void refresh();
    ListEditJob job_{this};
    QPointer<QTableView> view_;
    QMetaObject::Connection model_gone_;
    QLineEdit* expression_{};
    QComboBox* direction_{};
    QLabel* status_{};
    QAction* apply_{};
    QAction* expression_action_{};
    QAction* direction_action_{};
    QAction* close_{};
};
} // namespace trackknife::bench
