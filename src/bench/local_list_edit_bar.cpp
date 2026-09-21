// SPDX-License-Identifier: GPL-3.0-only
#include "bench/local_list_edit_bar.hpp"
#include "bench/local_list_model.hpp"

#include <QAction>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QTableView>
#include <QTimer>
#include <QtConcurrentRun>

namespace trackknife::bench {
namespace {
// Account before copying; no tag parsing or file reads occur here.
std::size_t snapshotBytes(const LocalTrackRow& row, bool metadata) {
    constexpr std::size_t limit = 64U * 1024U;
    auto size = sizeof(lists::Entry) + row.raw_path.size() +
                (row.logical_reference ? row.logical_reference->size() : 0);
    if (!metadata)
        return size;
    for (const auto* field :
         {&row.title, &row.artist, &row.album, &row.album_artist, &row.date, &row.track_number})
        size += field->size();
    size += row.metadata.fields.size() * sizeof(metadata::MetadataField);
    if (size > limit)
        return size;
    for (const auto& field : row.metadata.fields) {
        size += field.canonical_name.size() + field.native_name.size() +
                field.values.size() * sizeof(std::string);
        if (field.qualifier.language)
            size += field.qualifier.language->size();
        if (field.qualifier.description)
            size += field.qualifier.description->size();
        if (size > limit)
            return size;
        for (const auto& value : field.values) {
            size += value.size();
            if (size > limit)
                return size;
        }
    }
    return size;
}
QString editLabel(lists::EditKind kind) {
    switch (kind) {
    case lists::EditKind::sort:
        return QObject::tr("Sort list");
    case lists::EditKind::reverse:
        return QObject::tr("Reverse list");
    case lists::EditKind::shuffle_albums:
        return QObject::tr("Shuffle albums");
    case lists::EditKind::remove_duplicates:
        return QObject::tr("Remove duplicate entries");
    }
    return {};
}
} // namespace
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
    pool_.setMaxThreadCount(1);
    capture_timer_ = new QTimer(this);
    capture_timer_->setSingleShot(true);
    connect(capture_timer_, &QTimer::timeout, this, &LocalListEditBar::capture);
    progress_timer_ = new QTimer(this);
    progress_timer_->setInterval(100);
    connect(progress_timer_, &QTimer::timeout, this, [this] {
        if (active_ && progress_)
            status_->setText(tr("Preparing edit… %1 / %2 rows").arg(progress_->load()).arg(total_));
    });
    connect(&watcher_, &QFutureWatcherBase::finished, this, &LocalListEditBar::finish);
    hide();
}
LocalListEditBar::~LocalListEditBar() {
    cancel();
    pool_.waitForDone();
}
void LocalListEditBar::cancel() {
    ++generation_;
    active_ = false;
    close_->setText(tr("Close"));
    cancellation_.request_cancellation();
    capture_timer_->stop();
    progress_timer_->stop();
    snapshot_.reset();
    expression_->setEnabled(true);
    direction_->setEnabled(true);
    apply_->setEnabled(true);
}
void LocalListEditBar::stopWithMessage(const QString& message) {
    cancel();
    status_->setText(message);
}
void LocalListEditBar::invalidate() {
    if (active_)
        stopWithMessage(tr("List changed — run the command again"));
}
void LocalListEditBar::setView(QTableView* view) {
    if (view_ == view)
        return;
    cancel();
    hide();
    for (const auto& connection : connections_)
        disconnect(connection);
    connections_.clear();
    view_ = view;
    model_ = view ? qobject_cast<LocalListModel*>(view->model()) : nullptr;
    if (!model_)
        return;
    const auto changed = [this] { invalidate(); };
    connections_.push_back(connect(model_, &QAbstractItemModel::rowsInserted, this, changed));
    connections_.push_back(connect(model_, &QAbstractItemModel::rowsRemoved, this, changed));
    connections_.push_back(connect(model_, &QAbstractItemModel::rowsMoved, this, changed));
    connections_.push_back(connect(model_, &QAbstractItemModel::modelReset, this, changed));
    connections_.push_back(connect(model_, &QAbstractItemModel::layoutChanged, this, changed));
    connections_.push_back(
        connect(model_, &QAbstractItemModel::dataChanged, this,
                [this](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
                    if (roles.empty() || roles.contains(Qt::DisplayRole))
                        invalidate();
                }));
    connections_.push_back(connect(model_, &QObject::destroyed, this, [this] {
        cancel();
        hide();
    }));
}
void LocalListEditBar::openSort() {
    if (!model_)
        return;
    show();
    expression_action_->setVisible(true);
    direction_action_->setVisible(true);
    apply_->setVisible(true);
    expression_->setFocus();
    expression_->selectAll();
}
void LocalListEditBar::start(lists::EditRequest request) {
    cancel();
    if (!model_ || model_->rowCount() < 2)
        return;
    show();
    if (worker_busy_) {
        status_->setText(tr("Previous edit is stopping — run the command again"));
        return;
    }
    total_ = model_->rowCount();
    if (total_ > 1'000'000) {
        status_->setText(tr("List edits support at most one million rows"));
        return;
    }
    request_ = std::move(request);
    const auto sorting = request_.kind == lists::EditKind::sort;
    expression_action_->setVisible(sorting);
    direction_action_->setVisible(sorting);
    apply_->setVisible(sorting);
    if (request_.kind == lists::EditKind::sort) {
        expression_->setText(QString::fromStdString(request_.expression));
        direction_->setCurrentIndex(request_.descending ? 1 : 0);
    }
    active_ = true;
    close_->setText(tr("Cancel"));
    cancellation_ = core::CancellationSource{};
    snapshot_ = std::make_shared<std::vector<lists::Entry>>();
    progress_ = std::make_shared<std::atomic<std::size_t>>(0);
    bytes_ = 0;
    expression_->setEnabled(false);
    direction_->setEnabled(false);
    apply_->setEnabled(false);
    status_->setText(tr("Preparing list edit…"));
    capture_timer_->start(0);
}
void LocalListEditBar::capture() {
    if (!active_ || !model_)
        return;
    QElapsedTimer clock;
    clock.start();
    std::size_t batch_bytes = 0, batch_rows = 0;
    while (request_.kind != lists::EditKind::reverse &&
           snapshot_->size() < static_cast<std::size_t>(total_) && batch_rows < 128 &&
           batch_bytes < 256U * 1024U && clock.elapsed() < 4) {
        const auto& row = model_->rows()[snapshot_->size()];
        const auto sorting = request_.kind == lists::EditKind::sort;
        const auto size = snapshotBytes(row, sorting);
        if (size > 64U * 1024U || bytes_ + size > 128U * 1024U * 1024U) {
            stopWithMessage(tr("List edit exceeds the 64 KiB per row or 128 MiB snapshot limit"));
            return;
        }
        lists::Entry entry;
        if (request_.kind != lists::EditKind::reverse) {
            entry.raw_path = row.raw_path;
            entry.logical_reference = row.logical_reference;
            entry.selection = row.selection;
            entry.segment = row.segment;
        }
        if (sorting) {
            entry.metadata.fields = row.metadata.fields;
            entry.display = {row.title,        row.artist, row.album,
                             row.album_artist, row.date,   row.track_number};
        }
        snapshot_->push_back(std::move(entry));
        bytes_ += size;
        batch_bytes += size;
        ++batch_rows;
    }
    status_->setText(tr("Reading cached rows… %1 / %2").arg(snapshot_->size()).arg(total_));
    if (request_.kind != lists::EditKind::reverse &&
        snapshot_->size() < static_cast<std::size_t>(total_)) {
        capture_timer_->start(0);
        return;
    }
    worker_busy_ = true;
    job_generation_ = generation_;
    progress_timer_->start();
    watcher_.setFuture(QtConcurrent::run(&pool_, [snapshot = std::move(snapshot_),
                                                  request = request_, token = cancellation_.token(),
                                                  progress = progress_, total = total_]() mutable {
        // Release the potentially large detached snapshot on this worker.
        const auto entries = std::move(snapshot);
        if (request.kind == lists::EditKind::reverse)
            return lists::plan_reverse(static_cast<std::size_t>(total), token);
        return lists::plan_edit(*entries, request, token, progress.get());
    }));
}
void LocalListEditBar::finish() {
    worker_busy_ = false;
    if (!active_ || job_generation_ != generation_ || !model_)
        return;
    auto outcome = watcher_.result();
    if (!outcome) {
        stopWithMessage(QString::fromStdString(outcome.error().message));
        return;
    }
    const auto label = editLabel(request_.kind);
    cancel(); // Own model notifications must not invalidate this completed plan.
    bool changed = false;
    if (outcome->removal) {
        changed = !outcome->positions.empty();
        model_->removeRowIndexes(std::move(outcome->positions), true, label);
    } else {
        changed = model_->applyPermutation(outcome->positions, label);
    }
    status_->setText(!changed ? tr("No changes needed")
                     : model_->canUndo() && model_->undoLabel() == label
                         ? tr("%1 completed — Undo is available").arg(label)
                         : tr("%1 completed").arg(label));
    if (changed)
        emit edited(model_);
}
} // namespace trackknife::bench
