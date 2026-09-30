// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/list_edit_job.hpp"

#include "bench/local_list_model.hpp"

#include <QElapsedTimer>
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

} // namespace

QString ListEditJob::label(const lists::EditKind kind) {
    switch (kind) {
    case lists::EditKind::sort:
        return tr("Sort list");
    case lists::EditKind::reverse:
        return tr("Reverse list");
    case lists::EditKind::shuffle_albums:
        return tr("Shuffle albums");
    case lists::EditKind::remove_duplicates:
        return tr("Remove duplicate entries");
    }
    return {};
}

ListEditJob::ListEditJob(QObject* parent) : QObject(parent) {
    pool_.setMaxThreadCount(1);
    capture_timer_.setSingleShot(true);
    connect(&capture_timer_, &QTimer::timeout, this, &ListEditJob::capture);
    progress_timer_.setInterval(100);
    connect(&progress_timer_, &QTimer::timeout, this, [this] {
        if (active_ && progress_)
            setStatus(tr("Preparing edit… %1 / %2 rows").arg(progress_->load()).arg(total_));
    });
    connect(&watcher_, &QFutureWatcherBase::finished, this, &ListEditJob::finish);
}

ListEditJob::~ListEditJob() {
    cancel();
    pool_.waitForDone();
}

void ListEditJob::setStatus(const QString& status) {
    status_ = status;
    emit changed();
}

void ListEditJob::cancel() {
    ++generation_;
    active_ = false;
    cancellation_.request_cancellation();
    capture_timer_.stop();
    progress_timer_.stop();
    snapshot_.reset();
    emit changed();
}

void ListEditJob::stopWithMessage(const QString& message) {
    cancel();
    setStatus(message);
}

void ListEditJob::invalidate() {
    if (active_)
        stopWithMessage(tr("List changed — run the command again"));
}

void ListEditJob::setModel(LocalListModel* model) {
    if (model_ == model)
        return;
    cancel();
    for (const auto& connection : connections_)
        disconnect(connection);
    connections_.clear();
    model_ = model;
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
    connections_.push_back(connect(model_, &QObject::destroyed, this, [this] { cancel(); }));
}

void ListEditJob::start(lists::EditRequest request) {
    cancel();
    if (!model_ || model_->rowCount() < 2)
        return;
    request_ = std::move(request);
    emit shown();
    if (worker_busy_) {
        setStatus(tr("Previous edit is stopping — run the command again"));
        return;
    }
    total_ = model_->rowCount();
    if (total_ > 1'000'000) {
        setStatus(tr("List edits support at most one million rows"));
        return;
    }
    active_ = true;
    cancellation_ = core::CancellationSource{};
    snapshot_ = std::make_shared<std::vector<lists::Entry>>();
    progress_ = std::make_shared<std::atomic<std::size_t>>(0);
    bytes_ = 0;
    setStatus(tr("Preparing list edit…"));
    capture_timer_.start(0);
}

void ListEditJob::capture() {
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
    setStatus(tr("Reading cached rows… %1 / %2").arg(snapshot_->size()).arg(total_));
    if (request_.kind != lists::EditKind::reverse &&
        snapshot_->size() < static_cast<std::size_t>(total_)) {
        capture_timer_.start(0);
        return;
    }
    worker_busy_ = true;
    job_generation_ = generation_;
    progress_timer_.start();
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

void ListEditJob::finish() {
    worker_busy_ = false;
    if (!active_ || job_generation_ != generation_ || !model_)
        return;
    auto outcome = watcher_.result();
    if (!outcome) {
        stopWithMessage(QString::fromStdString(outcome.error().message));
        return;
    }
    const auto edit_label = label(request_.kind);
    cancel(); // Own model notifications must not invalidate this completed plan.
    bool edited_now = false;
    if (outcome->removal) {
        edited_now = !outcome->positions.empty();
        model_->removeRowIndexes(std::move(outcome->positions), true, edit_label);
    } else {
        edited_now = model_->applyPermutation(outcome->positions, edit_label);
    }
    setStatus(!edited_now ? tr("No changes needed")
              : model_->canUndo() && model_->undoLabel() == edit_label
                  ? tr("%1 completed — Undo is available").arg(edit_label)
                  : tr("%1 completed").arg(edit_label));
    if (edited_now)
        emit edited(model_);
}

} // namespace trackknife::bench
