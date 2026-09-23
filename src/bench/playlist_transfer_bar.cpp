// SPDX-License-Identifier: GPL-3.0-only
#include "bench/playlist_transfer_bar.hpp"
#include "bench/bench_main_window_helpers.hpp"
#include "trackknife/core/unicode.hpp"
#include "trackknife/metadata/local_reader.hpp"

#include <QAction>
#include <QElapsedTimer>
#include <QLabel>
#include <QTimer>
#include <QtConcurrentRun>

#include <filesystem>

namespace trackknife::bench {
namespace {
constexpr std::size_t snapshot_limit = 64U * 1024U * 1024U;
constexpr std::size_t imported_metadata_limit = 128U * 1024U * 1024U;
core::Error limitError() {
    return {.code = core::ErrorCode::limit_exceeded,
            .message = "Playlist exceeds the in-memory transfer limit",
            .context = {}};
}
std::size_t rowBytes(const LocalTrackRow& row) {
    auto size = sizeof(LocalTrackRow) + row.raw_path.size() + row.title.size() + row.artist.size() +
                row.album.size() + row.album_artist.size() + row.date.size() +
                row.track_number.size();
    for (const auto& field : row.metadata.fields) {
        size += sizeof(field) + field.canonical_name.size() + field.native_name.size();
        if (field.qualifier.language)
            size += field.qualifier.language->size();
        if (field.qualifier.description)
            size += field.qualifier.description->size();
        for (const auto& value : field.values)
            size += sizeof(value) + value.size();
    }
    for (const auto& object : row.metadata.unsupported_native_objects)
        size += sizeof(object) + object.identity.size();
    return size;
}
} // namespace
PlaylistTransferBar::PlaylistTransferBar(QWidget* parent)
    : QToolBar(tr("Portable playlist"), parent) {
    setObjectName(QStringLiteral("bench-playlist-transfer"));
    setMovable(false);
    setFloatable(false);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("bench-playlist-transfer-status"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    addWidget(status_);
    cancel_action_ = addAction(tr("Cancel"));
    cancel_action_->setObjectName(QStringLiteral("action-cancel-playlist-transfer"));
    connect(cancel_action_, &QAction::triggered, this, [this] {
        if (active_)
            cancel();
        else
            hide();
    });
    pool_.setMaxThreadCount(1);
    capture_timer_ = new QTimer(this);
    capture_timer_->setInterval(0);
    connect(capture_timer_, &QTimer::timeout, this, &PlaylistTransferBar::capture);
    progress_timer_ = new QTimer(this);
    progress_timer_->setInterval(100);
    connect(progress_timer_, &QTimer::timeout, this, [this] {
        if (active_)
            status_->setText(tr("%1… %2 entries").arg(operation_).arg(progress_->load()));
    });
    connect(&watcher_, &QFutureWatcherBase::finished, this, &PlaylistTransferBar::finish);
    hide();
}
PlaylistTransferBar::~PlaylistTransferBar() { stop(); }
void PlaylistTransferBar::clearCapture() {
    capture_timer_->stop();
    for (const auto& connection : connections_)
        disconnect(connection);
    connections_.clear();
    snapshot_.reset();
    model_ = nullptr;
}
void PlaylistTransferBar::report(bool success, const QString& message) {
    active_ = false;
    clearCapture();
    progress_timer_->stop();
    cancel_action_->setText(tr("Close"));
    status_->setText(message);
    emit completed(success, message);
}
void PlaylistTransferBar::cancel() {
    if (!active_)
        return;
    cancellation_.request_cancellation();
    clearCapture();
    if (worker_busy_) {
        operation_ = tr("Cancelling playlist transfer");
        status_->setText(operation_);
    } else
        report(false, tr("Playlist transfer cancelled"));
}
void PlaylistTransferBar::stop() {
    cancellation_.request_cancellation();
    active_ = false;
    clearCapture();
    progress_timer_->stop();
    pool_.waitForDone();
}
bool PlaylistTransferBar::begin(std::string raw_path) {
    if (busy()) {
        show();
        return false;
    }
    cancellation_ = core::CancellationSource{};
    path_ = std::move(raw_path);
    progress_ = std::make_shared<std::atomic<std::size_t>>(0);
    active_ = true;
    cancel_action_->setText(tr("Cancel"));
    show();
    progress_timer_->start();
    return true;
}
void PlaylistTransferBar::importFile(std::string raw_path) {
    if (!begin(std::move(raw_path)))
        return;
    operation_ = tr("Importing playlist");
    status_->setText(operation_);
    worker_busy_ = true;
    watcher_.setFuture(QtConcurrent::run(
        &pool_, [path = path_, token = cancellation_.token(), progress = progress_] {
            Outcome result;
            auto entries = lists::read_m3u8(path, token, progress.get());
            if (!entries) {
                result.error = entries.error();
                return result;
            }
            result.name = std::filesystem::path{path}.stem().native();
            if (!core::unicodeCodePointCount(result.name))
                result.name = core::display_raw_path(result.name);
            result.rows = std::make_shared<std::vector<LocalTrackRow>>();
            result.rows->reserve(entries->size());
            progress->store(0);
            std::size_t bytes = 0;
            for (auto& entry : *entries) {
                if (token.is_cancellation_requested()) {
                    result.error = core::Error{.code = core::ErrorCode::cancelled,
                                               .message = "Playlist import cancelled",
                                               .context = {}};
                    return result;
                }
                LocalTrackRow row;
                row.raw_path = std::move(entry.raw_path);
                row.duration_ms = entry.duration_ms;
                // This is one physical playlist occurrence. Never expand its source
                // into chapters/subsongs or recurse into nested playlists.
                if (core::observe_local_source_revision(row.raw_path)) {
                    if (auto read = metadata::read_local_metadata(row.raw_path, token)) {
                        row.metadata = std::move(read->document);
                        row.source_revision = read->source_revision;
                    }
                }
                project_display_metadata(row);
                if (row.title.empty()) {
                    row.title = std::move(entry.title);
                    if (row.title.empty()) {
                        row.title = std::filesystem::path{row.raw_path}.filename().native();
                        if (!core::unicodeCodePointCount(row.title))
                            row.title = core::display_raw_path(row.title);
                    }
                    row.metadata.fields.push_back(
                        {.canonical_name = "title",
                         .native_name = "TITLE",
                         .values = {row.title},
                         .qualifier = {},
                         .provenance = metadata::FieldProvenance::cached_snapshot});
                }
                row.probed = true;
                bytes += rowBytes(row);
                if (bytes > imported_metadata_limit) {
                    result.error = limitError();
                    return result;
                }
                result.rows->push_back(std::move(row));
                progress->store(result.rows->size());
            }
            if (token.is_cancellation_requested())
                result.error = core::Error{.code = core::ErrorCode::cancelled,
                                           .message = "Playlist import cancelled",
                                           .context = {}};
            return result;
        }));
}
void PlaylistTransferBar::exportFile(std::string raw_path, LocalListModel* model) {
    if (!model || !begin(std::move(raw_path)))
        return;
    operation_ = tr("Exporting playlist");
    status_->setText(operation_);
    model_ = model;
    total_ = model->rowCount();
    bytes_ = 0;
    if (static_cast<std::size_t>(total_) > lists::m3u8_max_entries) {
        report(false, tr("Playlist exceeds 100,000 entries"));
        return;
    }
    snapshot_ = std::make_shared<std::vector<lists::PlaylistEntry>>();
    snapshot_->reserve(static_cast<std::size_t>(total_));
    const auto changed = [this] { report(false, tr("List changed during capture; export again")); };
    connections_.push_back(connect(model, &QAbstractItemModel::modelReset, this, changed));
    connections_.push_back(connect(model, &QAbstractItemModel::rowsInserted, this, changed));
    connections_.push_back(connect(model, &QAbstractItemModel::rowsRemoved, this, changed));
    connections_.push_back(connect(model, &QAbstractItemModel::rowsMoved, this, changed));
    connections_.push_back(connect(model, &QAbstractItemModel::layoutChanged, this, changed));
    connections_.push_back(connect(model, &QObject::destroyed, this, changed));
    connections_.push_back(
        connect(model, &QAbstractItemModel::dataChanged, this,
                [changed](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
                    if (roles.empty() || roles.contains(Qt::DisplayRole))
                        changed();
                }));
    capture_timer_->start();
}
void PlaylistTransferBar::capture() {
    if (!active_ || !model_ || !snapshot_)
        return;
    QElapsedTimer timer;
    timer.start();
    std::size_t copied = 0, copied_bytes = 0;
    while (snapshot_->size() < static_cast<std::size_t>(total_) && copied < 128 &&
           copied_bytes < 256U * 1024U && timer.elapsed() < 4) {
        const auto& row = model_->rows()[snapshot_->size()];
        const auto size = sizeof(lists::PlaylistEntry) + row.raw_path.size() + row.title.size() +
                          (row.logical_reference ? row.logical_reference->size() : 0);
        if (size > lists::m3u8_max_line || bytes_ + size > snapshot_limit) {
            report(false, tr("Playlist exceeds the in-memory transfer limit"));
            return;
        }
        snapshot_->push_back({.raw_path = row.raw_path,
                              .title = row.title,
                              .duration_ms = row.duration_ms,
                              .logical_reference = row.logical_reference,
                              .selection = row.selection,
                              .segment = row.segment});
        ++copied;
        copied_bytes += size;
        bytes_ += size;
    }
    progress_->store(snapshot_->size());
    if (snapshot_->size() != static_cast<std::size_t>(total_))
        return;
    auto snapshot = std::move(snapshot_);
    clearCapture();
    worker_busy_ = true;
    watcher_.setFuture(
        QtConcurrent::run(&pool_, [snapshot = std::move(snapshot), path = path_,
                                   token = cancellation_.token(), progress = progress_] {
            Outcome result;
            auto bytes = lists::serialize_m3u8(*snapshot, path, token, progress.get());
            if (!bytes) {
                result.error = bytes.error();
                return result;
            }
            if (auto written = lists::write_m3u8_new(path, *bytes, token); !written)
                result.error = written.error();
            return result;
        }));
}
void PlaylistTransferBar::finish() {
    worker_busy_ = false;
    if (!active_)
        return;
    auto result = watcher_.result();
    if (result.error) {
        auto message = QString::fromStdString(result.error->message);
        for (const auto& context : result.error->context)
            message += tr(" (%1: %2)")
                           .arg(QString::fromStdString(context.key),
                                QString::fromStdString(context.value));
        report(false, message);
        return;
    }
    // Import cancellation can arrive after the worker returned but before this
    // callback. Export publication is already final and must report success.
    if (result.rows && cancellation_.is_cancellation_requested()) {
        report(false, tr("Playlist import cancelled"));
        return;
    }
    const auto count = result.rows ? result.rows->size() : progress_->load();
    if (result.rows)
        emit imported(result.rows, QString::fromStdString(result.name));
    report(true, tr("%1 %2 entries").arg(result.rows ? tr("Imported") : tr("Exported")).arg(count));
}
} // namespace trackknife::bench
