// SPDX-License-Identifier: GPL-3.0-only
#include "bench/local_list_edit_bar.hpp"
#include "bench/local_list_model.hpp"

#include <QAction>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QTableView>

namespace trackknife::bench {
LocalListEditBar::LocalListEditBar(QWidget* parent) : QToolBar(tr("Edit local list"), parent) {
    setObjectName(QStringLiteral("bench-list-edit-bar"));
    setMovable(false);
    setFloatable(false);
    expression_ = new QLineEdit(QStringLiteral("%title%"), this);
    expression_->setObjectName(QStringLiteral("bench-list-sort-expression"));
    expression_->setAccessibleName(tr("tkfmt-1 sorting expression"));
    expression_->setToolTip(
        tr("Sort the entire list using tkfmt-1. Example: %album%|%tracknumber%|%title%"));
    expression_->setMaxLength(4096);
    expression_->setMinimumWidth(240);
    expression_action_ = addWidget(expression_);
    direction_ = new QComboBox(this);
    direction_->setObjectName(QStringLiteral("bench-list-sort-direction"));
    direction_->addItems({tr("Ascending"), tr("Descending")});
    direction_->setAccessibleName(tr("Sort direction"));
    direction_action_ = addWidget(direction_);
    apply_ = addAction(tr("Sort list"));
    apply_->setObjectName(QStringLiteral("action-apply-list-sort"));
    const auto sort = [this] {
        start({.kind = lists::EditKind::sort,
               .expression = expression_->text().toStdString(),
               .descending = direction_->currentIndex() == 1});
    };
    connect(apply_, &QAction::triggered, this, sort);
    connect(expression_, &QLineEdit::returnPressed, this, sort);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("bench-list-edit-status"));
    status_->setContentsMargins(8, 0, 8, 0);
    addWidget(status_);
    close_ = addAction(tr("Close"));
    close_->setObjectName(QStringLiteral("action-cancel-list-edit"));
    connect(close_, &QAction::triggered, this, [this] {
        cancel();
        hide();
    });
    connect(&job_, &ListEditJob::changed, this, &LocalListEditBar::refresh);
    connect(&job_, &ListEditJob::shown, this, [this] {
        const auto sorting = job_.sorting();
        expression_action_->setVisible(sorting);
        direction_action_->setVisible(sorting);
        apply_->setVisible(sorting);
        if (sorting) {
            expression_->setText(QString::fromStdString(job_.request().expression));
            direction_->setCurrentIndex(job_.request().descending ? 1 : 0);
        }
        show();
    });
    connect(&job_, &ListEditJob::edited, this, &LocalListEditBar::edited);
    hide();
}
void LocalListEditBar::refresh() {
    const auto active = job_.active();
    close_->setText(active ? tr("Cancel") : tr("Close"));
    expression_->setEnabled(!active);
    direction_->setEnabled(!active);
    apply_->setEnabled(!active);
    status_->setText(job_.status());
}
void LocalListEditBar::cancel() { job_.cancel(); }
void LocalListEditBar::setView(QTableView* view) {
    if (view_ == view)
        return;
    view_ = view;
    job_.setModel(view ? qobject_cast<LocalListModel*>(view->model()) : nullptr);
    hide();
    disconnect(model_gone_);
    if (job_.model() != nullptr)
        model_gone_ = connect(job_.model(), &QObject::destroyed, this, [this] { hide(); });
}
void LocalListEditBar::openSort() {
    if (!job_.model())
        return;
    show();
    expression_action_->setVisible(true);
    direction_action_->setVisible(true);
    apply_->setVisible(true);
    expression_->setFocus();
    expression_->selectAll();
}
void LocalListEditBar::start(lists::EditRequest request) { job_.start(std::move(request)); }
} // namespace trackknife::bench
