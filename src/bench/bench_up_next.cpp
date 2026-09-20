// SPDX-License-Identifier: GPL-3.0-only
#include "bench/bench_main_window.hpp"
#include "bench/bench_main_window_helpers.hpp"
#include "quick/mpd_probe_controller.hpp"
#include "quick/mpd_queue_model.hpp"
#include "uicommon/list_persistence_service.hpp"
#include "uicommon/queue_table_view.hpp"
#include <QDockWidget>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace trackknife::bench {
void BenchMainWindow::buildUpNext() {
    up_next_dock_ = new QDockWidget(QStringLiteral("Up Next"), this);
    up_next_dock_->setObjectName(QStringLiteral("bench-up-next"));
    up_next_dock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    auto* content = new QWidget(up_next_dock_);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);
    up_next_status_ = new QLabel(content);
    up_next_status_->setWordWrap(true);
    layout->addWidget(up_next_status_);
    up_next_view_ = new ui::QueueTableView(content);
    up_next_view_->setObjectName(QStringLiteral("up-next-tracks"));
    up_next_local_model_ = new LocalListModel(this);
    up_next_mpd_model_ = new quick::MpdQueueModel(this);
    up_next_view_->setModel(up_next_local_model_);
    auto flat = defaultTrackViewLayout(ui::TrackViewPresentation::plain_columns);
    applyTrackViewLayout(up_next_view_, flat, flat);
    up_next_view_->setAlbumGroupingEnabled(false);
    up_next_view_->verticalHeader()->setDefaultSectionSize(
        std::max(28, up_next_view_->fontMetrics().height() + 10));
    for (int col = 0; col < up_next_local_model_->columnCount(); ++col)
        up_next_view_->setColumnHidden(col, col != ui::track_artist_column &&
                                                col != ui::track_title_column &&
                                                col != ui::track_length_column);
    up_next_view_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    up_next_view_->setSelectionMode(QAbstractItemView::SingleSelection);
    up_next_view_->horizontalHeader()->setSectionResizeMode(ui::track_length_column,
                                                            QHeaderView::ResizeToContents);
    up_next_view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    up_next_view_->setReorderCallback([this](const QVariantList& rows, int destination) {
        if (rows.size() == 1) {
            int from = rows.front().toInt();
            editUpNext(2, from, destination > from ? destination - 1 : destination);
        }
    });
    up_next_view_->setExternalDropCallback(
        [this](QAbstractItemView* source, const QVariantList&, int position, Qt::DropAction) {
            auto* table = qobject_cast<QTableView*>(source);
            if (!table)
                return false;
            const bool local = qobject_cast<LocalListModel*>(table->model()) != nullptr;
            if (local == isMpdContext())
                return false;
            enqueueUpNext(table, false, position);
            return true;
        });
    up_next_view_->setActivateCallback([this](const QModelIndex& index) {
        if (!index.isValid())
            return;
        if (isMpdContext()) {
            if (const auto* track = up_next_mpd_model_->trackAt(index.row());
                track && track->queue_id) {
                mpd::RequestQueueCommand command;
                command.operation = mpd::RequestQueueOperation::play;
                command.id = *track->queue_id;
                mpd_controller_->editRequestQueue(std::move(command));
            }
        } else if (index.row() < static_cast<int>(local_requests_.pending().size())) {
            local_requests_.move(
                local_requests_.pending()[static_cast<std::size_t>(index.row())].id, 0);
            refreshUpNext();
            static_cast<void>(playLocalRequest());
        }
    });
    layout->addWidget(up_next_view_, 1);
    auto* actions = new QHBoxLayout;
    auto button = [&](const QString& text, auto callback) {
        auto* b = new QPushButton(text, content);
        connect(b, &QPushButton::clicked, this, callback);
        actions->addWidget(b);
        return b;
    };
    button(QStringLiteral("Remove"),
           [this] { editUpNext(1, up_next_view_->currentIndex().row()); });
    button(QStringLiteral("↑"), [this] {
        int row = up_next_view_->currentIndex().row();
        editUpNext(2, row, row - 1);
    });
    button(QStringLiteral("↓"), [this] {
        int row = up_next_view_->currentIndex().row();
        editUpNext(2, row, row + 1);
    });
    button(QStringLiteral("Clear"), [this] { editUpNext(0); });
    auto* undo = button(QStringLiteral("Undo"), [this] {
        if (isMpdContext()) {
            mpd::RequestQueueCommand command;
            command.operation = mpd::RequestQueueOperation::undo;
            mpd_controller_->editRequestQueue(std::move(command));
        } else {
            local_requests_.undo();
            last_requested_next_.reset();
            persistUpNext();
            refreshUpNext();
        }
    });
    undo->setObjectName(QStringLiteral("up-next-undo"));
    layout->addLayout(actions);
    auto* resume = new QPushButton(QStringLiteral("Return to playlist now"), content);
    resume->setObjectName(QStringLiteral("up-next-return"));
    layout->addWidget(resume);
    connect(resume, &QPushButton::clicked, this, [this] {
        if (isMpdContext()) {
            mpd::RequestQueueCommand command;
            command.operation = mpd::RequestQueueOperation::resume;
            mpd_controller_->editRequestQueue(std::move(command));
        } else {
            local_requests_.clear();
            if (local_requests_.active())
                playAdjacent(1);
            persistUpNext();
            refreshUpNext();
        }
    });
    auto* remove = new QAction(QStringLiteral("Remove from Up Next"), up_next_view_);
    remove->setShortcut(Qt::Key_Delete);
    remove->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    up_next_view_->addAction(remove);
    connect(remove, &QAction::triggered, this,
            [this] { editUpNext(1, up_next_view_->currentIndex().row()); });
    up_next_dock_->setWidget(content);
    addDockWidget(Qt::RightDockWidgetArea, up_next_dock_);
    up_next_dock_->hide();
    up_next_dock_->resize(QSettings{}.value(QStringLiteral("up-next/width"), 420).toInt(), 400);
    up_next_dock_->setVisible(QSettings{}.value(QStringLiteral("up-next/visible"), false).toBool());
    connect(up_next_dock_, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (!isVisible())
            return;
        QSettings{}.setValue(QStringLiteral("up-next/visible"), visible);
        if (!visible && up_next_dock_->width() > 0)
            QSettings{}.setValue(QStringLiteral("up-next/width"), up_next_dock_->width());
    });
    auto* toggle = up_next_dock_->toggleViewAction();
    toggle->setObjectName(QStringLiteral("action-show-up-next"));
    toggle->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+U")));
    addAction(toggle);
    connect(mpd_controller_, &quick::MpdProbeController::stateChanged, this,
            &BenchMainWindow::refreshUpNext);
}

void BenchMainWindow::refreshUpNext() {
    if (!up_next_dock_)
        return;
    const bool server = isMpdContext();
    if (auto* undo = up_next_dock_->findChild<QPushButton*>(QStringLiteral("up-next-undo")))
        undo->setEnabled(server
                             ? (mpd_controller_->connected() && mpd_controller_->requestQueue() &&
                                mpd_controller_->requestQueue()->can_undo)
                             : local_requests_.canUndo());
    if (auto* resume = up_next_dock_->findChild<QPushButton*>(QStringLiteral("up-next-return")))
        resume->setEnabled(server
                               ? (mpd_controller_->connected() && mpd_controller_->requestQueue() &&
                                  mpd_controller_->requestQueue()->active_id != 0)
                               : local_requests_.active().has_value());
    auto* model = server ? static_cast<QAbstractItemModel*>(up_next_mpd_model_)
                         : static_cast<QAbstractItemModel*>(up_next_local_model_);
    if (up_next_view_->model() != model) {
        up_next_view_->setModel(model);
        auto flat = defaultTrackViewLayout(ui::TrackViewPresentation::plain_columns);
        applyTrackViewLayout(up_next_view_, flat, flat);
        up_next_view_->setAlbumGroupingEnabled(false);
        up_next_view_->verticalHeader()->setDefaultSectionSize(
            std::max(28, up_next_view_->fontMetrics().height() + 10));
        for (int col = 0; col < model->columnCount(); ++col)
            up_next_view_->setColumnHidden(col, col != ui::track_artist_column &&
                                                    col != ui::track_title_column &&
                                                    col != ui::track_length_column);
        up_next_view_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        up_next_view_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    }
    if (server) {
        const auto& state = mpd_controller_->requestQueue();
        bool available = mpd_controller_->connected() &&
                         mpd_controller_->supportsCommand(QStringLiteral("melody_upnext")) &&
                         state.has_value();
        up_next_view_->setEnabled(available);
        if (available) {
            if (up_next_remote_revision_ != state->revision ||
                up_next_remote_profile_ != mpd_controller_->profileId()) {
                up_next_mpd_model_->replaceTracks(state->pending);
                up_next_remote_revision_ = state->revision;
                up_next_remote_profile_ = mpd_controller_->profileId();
            }
            up_next_status_->setText(QStringLiteral("Melody · %1 pending\n%2\nThen resume: %3")
                                         .arg(state->pending.size())
                                         .arg(mpd_controller_->nowPlaying())
                                         .arg(state->context.empty()
                                                  ? QStringLiteral("Queue")
                                                  : QString::fromStdString(state->context)));
        } else {
            if (up_next_mpd_model_->rowCount() != 0)
                up_next_mpd_model_->replaceTracks({});
            up_next_remote_revision_ = 0;
            up_next_status_->setText(QStringLiteral(
                "Up Next requires a connected Melody server with request-queue support. Stock MPD "
                "still supports inserting tracks into its normal list."));
        }
    } else {
        up_next_view_->setEnabled(true);
        if (up_next_local_revision_ != local_requests_.revision()) {
            std::vector<LocalTrackRow> rows;
            for (const auto& entry : local_requests_.pending())
                rows.push_back(entry.source);
            up_next_local_model_->replaceRows(std::move(rows));
            up_next_local_revision_ = local_requests_.revision();
        }
        auto* tab = tabForDocument(playback_document_id_);
        QString playing;
        if (local_requests_.active())
            playing = QString::fromStdString(local_requests_.active()->source.artist + " — " +
                                             local_requests_.active()->source.title);
        up_next_status_->setText(
            QStringLiteral("Local · %1 pending%2\nThen resume: %3")
                .arg(local_requests_.pending().size())
                .arg(playing.isEmpty() ? QString{} : QStringLiteral("\nPlaying: ") + playing)
                .arg(tab ? QString::fromStdString(tab->document.name)
                         : QStringLiteral("No normal playback")));
    }
    if (up_next_button_)
        up_next_button_->setText(QStringLiteral("Up Next · %1").arg(model->rowCount()));
}

void BenchMainWindow::addUpNextActions(QMenu* menu, QTableView* source) {
    const bool remote = qobject_cast<LocalListModel*>(source->model()) == nullptr;
    for (bool prepend : {true, false}) {
        auto* action = findChild<QAction*>(prepend ? QStringLiteral("action-queue-next")
                                                   : QStringLiteral("action-queue-end"));
        if (!action)
            continue;
        menu->addAction(action);
        action->setEnabled(
            !source->selectionModel()->selectedRows().isEmpty() &&
            (!remote || (mpd_controller_->connected() &&
                         mpd_controller_->supportsCommand(QStringLiteral("melody_upnext")))));
    }
}

void BenchMainWindow::enqueueUpNext(QTableView* source, bool prepend, int position) {
    if (auto* local = qobject_cast<LocalListModel*>(source->model())) {
        std::vector<LocalTrackRow> rows;
        auto indices = source->selectionModel()->selectedRows();
        std::sort(indices.begin(), indices.end(),
                  [](const auto& a, const auto& b) { return a.row() < b.row(); });
        for (const auto& index : indices)
            rows.push_back(local->rows().at(static_cast<std::size_t>(index.row())));
        enqueueLocalRequests(std::move(rows), position >= 0 ? position : (prepend ? 0 : -1));
    } else {
        mpd::RequestQueueCommand command;
        command.operation = position >= 0 ? mpd::RequestQueueOperation::insert
                                          : (prepend ? mpd::RequestQueueOperation::prepend
                                                     : mpd::RequestQueueOperation::append);
        if (position >= 0)
            command.position = static_cast<unsigned>(position);
        for (const auto& track : selectedMpdViewTracks(source))
            command.uris.push_back(track.uri);
        if (!command.uris.empty())
            mpd_controller_->editRequestQueue(std::move(command));
    }
    refreshUpNext();
}

void BenchMainWindow::enqueueLocalRequests(std::vector<LocalTrackRow> rows, int position) {
    const auto count = rows.size();
    if (!local_requests_.insert(std::move(rows), position < 0
                                                     ? local_requests_.pending().size()
                                                     : static_cast<std::size_t>(position))) {
        statusBar()->showMessage(QStringLiteral("Up Next holds at most 500 tracks."), 5000);
        return;
    }
    last_requested_next_.reset();
    persistUpNext();
    refreshUpNext();
    statusBar()->showMessage(QStringLiteral("Added %1 to Up Next").arg(count), 3000);
}

void BenchMainWindow::editUpNext(int operation, int row, int destination) {
    if (isMpdContext()) {
        const auto& state = mpd_controller_->requestQueue();
        if (!state)
            return;
        mpd::RequestQueueCommand command;
        if (operation == 0)
            command.operation = mpd::RequestQueueOperation::clear;
        else {
            if (row < 0 || row >= static_cast<int>(state->pending.size()))
                return;
            command.id = state->pending[static_cast<std::size_t>(row)].queue_id.value_or(0);
            if (operation == 1)
                command.operation = mpd::RequestQueueOperation::remove;
            else {
                if (destination < 0 || destination >= static_cast<int>(state->pending.size()))
                    return;
                command.operation = mpd::RequestQueueOperation::move;
                command.position = static_cast<unsigned>(destination);
            }
        }
        mpd_controller_->editRequestQueue(std::move(command));
    } else {
        if (operation == 0)
            local_requests_.clear();
        else {
            if (row < 0 || row >= static_cast<int>(local_requests_.pending().size()))
                return;
            auto id = local_requests_.pending()[static_cast<std::size_t>(row)].id;
            if (operation == 1)
                local_requests_.remove(id);
            else {
                if (destination < 0)
                    return;
                local_requests_.move(id, static_cast<std::size_t>(destination));
            }
        }
        last_requested_next_.reset();
        persistUpNext();
        refreshUpNext();
    }
}

void BenchMainWindow::persistUpNext() {
    if (!persistence_ || !up_next_restored_)
        return;
    QJsonArray rows;
    const auto append = [&](const LocalTrackRow& row) {
        QJsonObject item{{QStringLiteral("path"),
                          QString::fromLatin1(QByteArray::fromStdString(row.raw_path).toBase64())},
                         {QStringLiteral("title"), QString::fromStdString(row.title)},
                         {QStringLiteral("artist"), QString::fromStdString(row.artist)}};
        if (row.selection.stream_index)
            item[QStringLiteral("stream")] = *row.selection.stream_index;
        if (row.selection.subsong_index)
            item[QStringLiteral("subsong")] = static_cast<int>(*row.selection.subsong_index);
        if (row.segment) {
            item[QStringLiteral("start")] = QString::number(row.segment->start_sample);
            if (row.segment->end_sample)
                item[QStringLiteral("end")] = QString::number(*row.segment->end_sample);
        }
        item[QStringLiteral("album")] = QString::fromStdString(row.album);
        item[QStringLiteral("albumArtist")] = QString::fromStdString(row.album_artist);
        item[QStringLiteral("date")] = QString::fromStdString(row.date);
        if (row.logical_reference)
            item[QStringLiteral("logical")] = QString::fromStdString(*row.logical_reference);
        if (row.duration_ms)
            item[QStringLiteral("duration")] = QString::number(*row.duration_ms);
        QJsonArray fields;
        for (const auto& field : row.metadata.fields) {
            QJsonArray values;
            for (const auto& value : field.values)
                values.push_back(QString::fromStdString(value));
            QJsonObject fieldJson{
                {QStringLiteral("name"), QString::fromStdString(field.canonical_name)},
                {QStringLiteral("native"), QString::fromStdString(field.native_name)},
                {QStringLiteral("values"), values},
                {QStringLiteral("provenance"), static_cast<int>(field.provenance)}};
            if (field.qualifier.language)
                fieldJson[QStringLiteral("language")] =
                    QString::fromStdString(*field.qualifier.language);
            if (field.qualifier.description)
                fieldJson[QStringLiteral("description")] =
                    QString::fromStdString(*field.qualifier.description);
            fields.push_back(fieldJson);
        }
        item[QStringLiteral("fields")] = fields;
        rows.push_back(item);
    };
    if (local_requests_.active())
        append(local_requests_.active()->source);
    for (const auto& entry : local_requests_.pending())
        append(entry.source);
    QJsonObject state{
        {QStringLiteral("version"), 1},
        {QStringLiteral("rows"), rows},
        {QStringLiteral("document"), playback_document_id_},
        {QStringLiteral("row"), playback_index_.isValid() ? playback_index_.row() : -1}};
    state[QStringLiteral("anchor")] =
        QString::fromLatin1(QByteArray::fromStdString(playback_source_.raw_path).toBase64());
    persistence_->saveUiState(
        QStringLiteral("playback/up-next/v1"), QJsonDocument(state).toJson(QJsonDocument::Compact),
        [this](QString error) {
            if (!error.isEmpty())
                statusBar()->showMessage(
                    QStringLiteral("Up Next could not be saved: %1").arg(error), 8000);
        });
}

void BenchMainWindow::restoreUpNext() {
    if (!persistence_)
        return;
    persistence_->loadUiState(QStringLiteral("playback/up-next/v1"), [this](QByteArray payload,
                                                                            QString error) {
        if (!error.isEmpty()) {
            statusBar()->showMessage(error, 5000);
            return;
        }
        if (!payload.isEmpty()) {
            const auto document = QJsonDocument::fromJson(payload);
            const auto state = document.object();
            if (state.value(QStringLiteral("version")).toInt() != 1) {
                statusBar()->showMessage(
                    QStringLiteral("Unsupported Up Next state; it has been preserved."), 8000);
                return;
            }
            std::vector<LocalTrackRow> rows;
            for (const auto& value : state.value(QStringLiteral("rows")).toArray()) {
                const auto item = value.toObject();
                LocalTrackRow row;
                row.raw_path =
                    QByteArray::fromBase64(item.value(QStringLiteral("path")).toString().toLatin1())
                        .toStdString();
                if (row.raw_path.empty())
                    continue;
                row.title = item.value(QStringLiteral("title")).toString().toStdString();
                row.artist = item.value(QStringLiteral("artist")).toString().toStdString();
                if (item.contains(QStringLiteral("stream")))
                    row.selection.stream_index = item.value(QStringLiteral("stream")).toInt();
                if (item.contains(QStringLiteral("subsong")))
                    row.selection.subsong_index = item.value(QStringLiteral("subsong")).toInt();
                if (item.contains(QStringLiteral("start")))
                    row.segment = formats::SampleRange{
                        item.value(QStringLiteral("start")).toString().toLongLong(),
                        item.contains(QStringLiteral("end"))
                            ? std::optional<std::int64_t>{item.value(QStringLiteral("end"))
                                                              .toString()
                                                              .toLongLong()}
                            : std::nullopt};
                row.album = item.value(QStringLiteral("album")).toString().toStdString();
                row.album_artist =
                    item.value(QStringLiteral("albumArtist")).toString().toStdString();
                row.date = item.value(QStringLiteral("date")).toString().toStdString();
                if (item.contains(QStringLiteral("logical")))
                    row.logical_reference =
                        item.value(QStringLiteral("logical")).toString().toStdString();
                if (item.contains(QStringLiteral("duration")))
                    row.duration_ms =
                        item.value(QStringLiteral("duration")).toString().toLongLong();
                for (const auto& fieldValue : item.value(QStringLiteral("fields")).toArray()) {
                    const auto fieldJson = fieldValue.toObject();
                    metadata::MetadataField field;
                    field.canonical_name =
                        fieldJson.value(QStringLiteral("name")).toString().toStdString();
                    field.native_name =
                        fieldJson.value(QStringLiteral("native")).toString().toStdString();
                    const auto provenance = fieldJson.value(QStringLiteral("provenance")).toInt();
                    if (provenance < 0 ||
                        provenance > static_cast<int>(metadata::FieldProvenance::sidecar))
                        continue;
                    field.provenance = static_cast<metadata::FieldProvenance>(provenance);
                    for (const auto& valueText :
                         fieldJson.value(QStringLiteral("values")).toArray())
                        field.values.push_back(valueText.toString().toStdString());
                    if (fieldJson.contains(QStringLiteral("language")))
                        field.qualifier.language =
                            fieldJson.value(QStringLiteral("language")).toString().toStdString();
                    if (fieldJson.contains(QStringLiteral("description")))
                        field.qualifier.description =
                            fieldJson.value(QStringLiteral("description")).toString().toStdString();
                    row.metadata.fields.push_back(std::move(field));
                }
                rows.push_back(std::move(row));
            }
            if (local_requests_.pending().empty())
                local_requests_.insert(std::move(rows), 0);
            if (playback_document_id_.isEmpty() && !local_requests_.pending().empty()) {
                const auto id = state.value(QStringLiteral("document")).toString();
                const auto anchor = QByteArray::fromBase64(
                                        state.value(QStringLiteral("anchor")).toString().toLatin1())
                                        .toStdString();
                if (auto* tab = tabForDocument(id)) {
                    const auto row = state.value(QStringLiteral("row")).toInt(-1);
                    if (row >= 0 && row < tab->model->rowCount() &&
                        tab->model->rawPath(row) == anchor) {
                        playback_document_id_ = id;
                        playback_index_ = tab->model->index(row, 0);
                        playback_source_ = tab->model->source(row);
                        resetPlaybackOrder();
                    }
                }
            }
        }
        local_requests_.forgetUndo();
        up_next_restored_ = true;
        refreshUpNext();
    });
}
} // namespace trackknife::bench
