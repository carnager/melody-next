// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "workspace/list_find.hpp"

#include <QPointer>
#include <QToolBar>

#include <vector>

class QLabel;
class QLineEdit;
class QTableView;

namespace trackknife::bench {

// Find in the list shown, as a bar under it: a ListFind, drawn.
class TrackListFindBar final : public QToolBar {
    Q_OBJECT

  public:
    explicit TrackListFindBar(QWidget* parent = nullptr);
    ~TrackListFindBar() override;
    void setView(QTableView* view);
    void open() { find_->open(); }
    void findNext(bool backwards = false) { find_->findNext(backwards); }
    void dismiss() { find_->dismiss(); }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void sync();

    ListFind* find_;
    QPointer<QTableView> view_;
    std::vector<QMetaObject::Connection> connections_;
    QLineEdit* query_{nullptr};
    QLabel* status_{nullptr};
};

} // namespace trackknife::bench
