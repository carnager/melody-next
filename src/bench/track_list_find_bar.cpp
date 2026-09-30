// SPDX-License-Identifier: GPL-3.0-only

#include "bench/track_list_find_bar.hpp"

#include "bench/local_list_model.hpp"
#include "uicommon/track_row_roles.hpp"

#include <QAction>
#include <QEvent>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QTableView>

namespace trackknife::bench {

TrackListFindBar::TrackListFindBar(QWidget* parent)
    : QToolBar(tr("Find in list"), parent), find_(new ListFind(this)) {
    setObjectName(QStringLiteral("bench-list-find-bar"));
    setMovable(false);
    setFloatable(false);
    query_ = new QLineEdit(this);
    query_->setObjectName(QStringLiteral("bench-list-find-query"));
    query_->setPlaceholderText(tr("Find in current list"));
    query_->setAccessibleName(tr("Find in current list"));
    query_->setToolTip(
        tr("Search any metadata value, the duration, the audio format, or the source path/URI"));
    query_->setClearButtonEnabled(true);
    query_->setMaxLength(1024);
    query_->setMinimumWidth(180);
    query_->installEventFilter(this);
    addWidget(query_);
    auto* previous = addAction(tr("Previous"));
    previous->setObjectName(QStringLiteral("action-list-find-previous-button"));
    previous->setToolTip(tr("Previous match (Shift+Enter or Shift+F3)"));
    connect(previous, &QAction::triggered, this, [this] { findNext(true); });
    auto* next = addAction(tr("Next"));
    next->setObjectName(QStringLiteral("action-list-find-next-button"));
    next->setToolTip(tr("Next match (Enter or F3)"));
    connect(next, &QAction::triggered, this, [this] { findNext(); });
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("bench-list-find-status"));
    status_->setContentsMargins(8, 0, 8, 0);
    addWidget(status_);
    auto* close = addAction(tr("Close"));
    close->setObjectName(QStringLiteral("action-list-find-close"));
    close->setToolTip(tr("Close find (Escape)"));
    close->setShortcut(QKeySequence(Qt::Key_Escape));
    close->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(close, &QAction::triggered, this, &TrackListFindBar::dismiss);
    connect(query_, &QLineEdit::textChanged, find_, &ListFind::setQuery);
    connect(find_, &ListFind::changed, this, &TrackListFindBar::sync);
    connect(find_, &ListFind::opened, this, [this] {
        query_->setFocus(Qt::ShortcutFocusReason);
        query_->selectAll();
    });
    connect(find_, &ListFind::dismissed, this, [this] {
        if (view_)
            view_->setFocus(Qt::ShortcutFocusReason);
    });
    connect(find_, &ListFind::found, this, [this](const int row) {
        if (!view_ || view_->model() == nullptr)
            return;
        const auto index = view_->model()->index(row, ui::track_title_column);
        view_->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect |
                                                            QItemSelectionModel::Rows);
        view_->scrollTo(index, QAbstractItemView::EnsureVisible);
    });
    hide();
}

TrackListFindBar::~TrackListFindBar() = default;

void TrackListFindBar::sync() {
    if (query_->text() != find_->query()) {
        const QSignalBlocker blocker{query_};
        query_->setText(find_->query());
    }
    status_->setText(find_->status());
    setVisible(find_->shown());
}

void TrackListFindBar::setView(QTableView* view) {
    if (view_ == view)
        return;
    for (const auto& connection : connections_)
        disconnect(connection);
    connections_.clear();
    if (view_)
        view_->removeEventFilter(this);
    view_ = view;
    auto* model = view == nullptr ? nullptr : qobject_cast<LocalListModel*>(view->model());
    find_->setList(model, [guarded = QPointer<QTableView>{view}] {
        return guarded ? guarded->currentIndex().row() : -1;
    });
    if (model == nullptr)
        return;
    view_->installEventFilter(this);
    connections_.push_back(connect(view_->selectionModel(), &QItemSelectionModel::currentChanged,
                                   find_, &ListFind::selectionChanged));
    connections_.push_back(connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged,
                                   find_, &ListFind::selectionChanged));
}

bool TrackListFindBar::eventFilter(QObject* watched, QEvent* event) {
    // Destruction-time events (Hide, FocusOut, ChildRemoved) reach this filter
    // while the watched view is mid-teardown; comparing against the typed
    // QPointer then downcasts a partially destroyed object. Only key presses
    // matter here, so gate on the event type before touching the pointers.
    if (event->type() != QEvent::KeyPress)
        return QToolBar::eventFilter(watched, event);
    if (watched == view_ && !isHidden() &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        dismiss();
        return true;
    }
    if (watched == query_) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            dismiss();
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            findNext(key->modifiers().testFlag(Qt::ShiftModifier));
            return true;
        }
    }
    return QToolBar::eventFilter(watched, event);
}

} // namespace trackknife::bench
