// SPDX-License-Identifier: GPL-3.0-only
#include "bench/dynamic_playlist_dialog.hpp"
#include "uicommon/queue_table_view.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDataStream>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

namespace trackknife::bench {
namespace {
// Raw source identity plus duplicate ordinal, never a mutable row number or title.
QList<QByteArray> resultKeys(QAbstractItemModel* model) {
    QList<QByteArray> keys;
    QHash<QByteArray, int> occurrences;
    for (int i = 0; i < model->rowCount(); ++i) {
        QByteArray key;
        QDataStream stream(&key, QIODevice::WriteOnly);
        if (auto* local = qobject_cast<LocalListModel*>(model)) {
            const auto& row = local->rows()[static_cast<std::size_t>(i)];
            stream << QByteArray::fromStdString(row.raw_path)
                   << row.selection.stream_index.value_or(-1)
                   << row.selection.subsong_index.value_or(-1)
                   << static_cast<qint64>(row.segment ? row.segment->start_sample : -1)
                   << static_cast<qint64>(row.segment ? row.segment->end_sample.value_or(-1) : -1);
        }
        const auto ordinal = occurrences[key]++;
        stream << ordinal;
        keys.push_back(std::move(key));
    }
    return keys;
}
} // namespace
DynamicPlaylistDialog::DynamicPlaylistDialog(QString profile, QString authority_label,
                                             DynamicPlaylistService::Search search, QWidget* parent)
    : QDialog(parent), profile_(std::move(profile)),
      service_(new DynamicPlaylistService(std::move(search), this)) {
    setObjectName(QStringLiteral("bench-dynamic-playlists"));
    setWindowTitle(QStringLiteral("Dynamic playlists — %1").arg(authority_label));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(900, 720);
    auto* layout = new QVBoxLayout(this);
    auto* explanation = new QLabel(
        QStringLiteral("Save rules or a Last.fm source, then refresh to see matching tracks in %1. "
                       "Rules update while this window is open. Last.fm refreshes draw a fresh "
                       "selection when requested. "
                       "Opening a snapshot keeps that list stable while you listen.")
            .arg(authority_label),
        this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* catalog_row = new QHBoxLayout;
    catalog_ = new QComboBox(this);
    catalog_->setObjectName(QStringLiteral("dynamic-catalog"));
    catalog_->setAccessibleName(QStringLiteral("Saved dynamic playlists"));
    catalog_row->addWidget(catalog_, 1);
    auto* save = new QPushButton(QStringLiteral("Save definition"), this);
    save->setObjectName(QStringLiteral("dynamic-save"));
    auto* remove = new QPushButton(QStringLiteral("Remove"), this);
    remove->setObjectName(QStringLiteral("dynamic-remove"));
    catalog_row->addWidget(save);
    catalog_row->addWidget(remove);
    layout->addLayout(catalog_row);
    form_ = new QFormLayout;
    form_->setRowWrapPolicy(QFormLayout::WrapLongRows);
    const auto line = [this](const QString& label, const QString& object) {
        auto* edit = new QLineEdit(this);
        edit->setObjectName(object);
        form_->addRow(label, edit);
        return edit;
    };
    name_ = line(QStringLiteral("Name:"), QStringLiteral("dynamic-name"));
    source_ = new QComboBox(this);
    source_->setObjectName(QStringLiteral("dynamic-source"));
    source_->addItem(QStringLiteral("Library rules"), QStringLiteral("rules"));
    source_->addItem(QStringLiteral("Last.fm similar tracks"), QStringLiteral("similar"));
    source_->addItem(QStringLiteral("Last.fm loved tracks"), QStringLiteral("loved"));
    source_->addItem(QStringLiteral("Last.fm top tracks (all time)"), QStringLiteral("top"));
    source_->addItem(QStringLiteral("Last.fm tag tracks"), QStringLiteral("tag"));
    form_->addRow(QStringLiteral("Source:"), source_);
    query_ = line(QStringLiteral("Rules:"), QStringLiteral("dynamic-query"));
    query_->setPlaceholderText(QStringLiteral("genre HAS rock AND rating GREATER 6"));
    query_->setToolTip(QStringLiteral(
        "Ratings use 0–10; 8 means four stars. Example: genre HAS jazz SORT BY %album%"));
    artist_ = line(QStringLiteral("Seed artist:"), QStringLiteral("dynamic-artist"));
    track_ = line(QStringLiteral("Seed track:"), QStringLiteral("dynamic-track"));
    user_ = line(QStringLiteral("Last.fm user:"), QStringLiteral("dynamic-user"));
    tag_ = line(QStringLiteral("Last.fm tag:"), QStringLiteral("dynamic-tag"));
    limit_ = new QSpinBox(this);
    limit_->setRange(1, 500);
    limit_->setValue(100);
    limit_->setObjectName(QStringLiteral("dynamic-limit"));
    form_->addRow(QStringLiteral("Maximum tracks:"), limit_);
    shuffle_ = new QCheckBox(QStringLiteral("Shuffle results on refresh"), this);
    shuffle_->setObjectName(QStringLiteral("dynamic-shuffle"));
    form_->addRow(QString{}, shuffle_);
    layout->addLayout(form_);
    auto* actions = new QHBoxLayout;
    refresh_ = new QPushButton(QStringLiteral("Refresh"), this);
    refresh_->setObjectName(QStringLiteral("dynamic-refresh"));
    auto* stop = new QPushButton(QStringLiteral("Stop"), this);
    stop->setObjectName(QStringLiteral("dynamic-stop"));
    open_ = new QPushButton(QStringLiteral("Open snapshot in new tab"), this);
    open_->setObjectName(QStringLiteral("dynamic-open"));
    actions->addWidget(refresh_);
    actions->addWidget(stop);
    actions->addStretch();
    actions->addWidget(open_);
    layout->addLayout(actions);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("dynamic-status"));
    status_->setWordWrap(true);
    layout->addWidget(status_);
    view_ = new ui::QueueTableView(this);
    view_->setObjectName(QStringLiteral("dynamic-tracks"));
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view_->setProperty("definition-owned", true);
    view_->setDragDropMode(QAbstractItemView::DragOnly);
    view_->setDefaultDropAction(Qt::CopyAction);
    view_->setDragEnabled(true);
    view_->setAcceptDrops(false);
    view_->setActivateCallback([this](const QModelIndex&) { playCurrent(); });
    connect(view_, &QTableView::doubleClicked, this, [this](const QModelIndex&) { playCurrent(); });
    local_model_ = new LocalListModel(this);
    local_model_->setProperty("definition-owned", true);
    view_->setModel(local_model_);
    // Hidden by default; the owning window supplies the authority's history service.
    view_->setColumnHidden(ui::track_play_count_column, true);
    view_->setColumnHidden(ui::track_last_played_column, true);
    layout->addWidget(view_, 1);
    refresh_timer_ = new QTimer(this);
    refresh_timer_->setSingleShot(true);
    refresh_timer_->setInterval(500);
    connect(refresh_timer_, &QTimer::timeout, this, [this] {
        if (source_->currentData() == QStringLiteral("rules"))
            refresh(true);
    });
    auto* poll = new QTimer(this);
    poll->setInterval(30000);
    connect(poll, &QTimer::timeout, this, [this] {
        if (isVisible() && !busy_ && !shuffle_->isChecked())
            libraryChanged();
    });
    poll->start();
    connect(service_, &DynamicPlaylistService::progress, status_, &QLabel::setText);
    connect(
        service_, &DynamicPlaylistService::finished, this,
        [this](const DynamicPlaylistService::Tracks& tracks, int unmatched, const QString& error) {
            busy_ = false;
            refresh_->setEnabled(authority_valid_);
            if (refresh_pending_) {
                refresh_pending_ = false;
                // A database change during this query invalidates its snapshot.
                // Retain the last displayed result until a fresh evaluation finishes.
                libraryChanged();
                return;
            }
            if (!error.isEmpty()) {
                discardResults();
                status_->setText(error);
                return;
            }
            const auto old_keys = resultKeys(view_->model());
            QSet<QByteArray> selected;
            for (const auto& index : view_->selectionModel()->selectedRows())
                selected.insert(old_keys.value(index.row()));
            const auto current_key = old_keys.value(view_->currentIndex().row());
            const auto top = view_->indexAt(QPoint{1, 1});
            const auto top_key = old_keys.value(top.row());
            const auto offset = top.isValid() ? view_->visualRect(top).top() : 0;
            const auto horizontal = view_->horizontalScrollBar()->value();
            tracks_ = tracks;
            {
                const auto& rows = tracks_;
                const auto& previous = local_model_->rows();
                const bool unchanged = rows.size() == previous.size() &&
                                       std::equal(rows.begin(), rows.end(), previous.begin(),
                                                  [](const auto& a, const auto& b) {
                                                      return a == b && a.rating == b.rating &&
                                                             a.album_rating == b.album_rating;
                                                  });
                if (!unchanged)
                    local_model_->replaceRows(rows);
            }
            const auto keys = resultKeys(view_->model());
            view_->selectionModel()->clearSelection();
            for (int i = 0; i < keys.size(); ++i) {
                const auto index = view_->model()->index(i, 0);
                if (selected.contains(keys[i]))
                    view_->selectionModel()->select(index, QItemSelectionModel::Select |
                                                               QItemSelectionModel::Rows);
                if (!current_key.isEmpty() && keys[i] == current_key)
                    view_->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
                if (!top_key.isEmpty() && keys[i] == top_key) {
                    view_->scrollTo(index, QAbstractItemView::PositionAtTop);
                    if (view_->verticalScrollMode() == QAbstractItemView::ScrollPerPixel)
                        view_->verticalScrollBar()->setValue(view_->verticalScrollBar()->value() -
                                                             offset);
                }
            }
            view_->horizontalScrollBar()->setValue(horizontal);
            emit resultsChanged();
            const auto count = tracks_.size();
            open_->setEnabled(count > 0);
            status_->setText(
                source_->currentData() == QStringLiteral("rules")
                    ? QStringLiteral(
                          "%1 tracks · rules update automatically while this window is open")
                          .arg(count)
                    : QStringLiteral("%1 tracks selected from %2 library matches · %3 Last.fm "
                                     "tracks not found%4")
                          .arg(count)
                          .arg(service_->matchedPoolSize())
                          .arg(unmatched)
                          .arg(service_->matchedPoolSize() <=
                                       static_cast<std::size_t>(limit_->value())
                                   ? QStringLiteral(" · all available matches included")
                                   : QString{}));
        });
    connect(refresh_, &QPushButton::clicked, this, &DynamicPlaylistDialog::refresh);
    connect(stop, &QPushButton::clicked, this, [this] {
        auto_refresh_ = false;
        refresh_pending_ = false;
        refresh_timer_->stop();
        service_->cancel();
        busy_ = false;
        refresh_->setEnabled(authority_valid_);
        status_->setText(QStringLiteral("Stopped"));
    });
    connect(open_, &QPushButton::clicked, this,
            [this] { emit snapshotRequested(name_->text().trimmed(), tracks_); });
    connect(catalog_, &QComboBox::activated, this, [this](int) { loadSelection(); });
    connect(source_, &QComboBox::currentIndexChanged, this, [this](int) {
        updateFields();
        discardResults();
    });
    for (auto* edit : {query_, artist_, track_, user_, tag_})
        connect(edit, &QLineEdit::textEdited, this, [this] { discardResults(); });
    connect(limit_, &QSpinBox::valueChanged, this, [this](int) { discardResults(); });
    connect(shuffle_, &QCheckBox::toggled, this, [this](bool) { discardResults(); });
    connect(save, &QPushButton::clicked, this, [this] {
        auto d = definition();
        if (d.name.isEmpty()) {
            status_->setText(QStringLiteral("Give the playlist a name"));
            return;
        }
        if (d.source == QStringLiteral("rules")) {
            const auto compiled = query::compile_tkq(d.query.toStdString());
            if (!compiled) {
                status_->setText(QString::fromStdString(compiled.error().message));
                return;
            }
        }
        auto next = definitions_;
        if (d.id.isEmpty()) {
            d.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            next.push_back(d);
        } else
            for (auto& entry : next)
                if (entry.id == d.id)
                    entry = d;
        const auto saved = saveDynamicPlaylists(profile_, next);
        if (!saved) {
            status_->setText(QString::fromStdString(saved.error().message));
            return;
        }
        definitions_ = std::move(next);
        refill(d.id);
        status_->setText(QStringLiteral("Definition saved"));
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const auto id = catalog_->currentData().toString();
        if (id.isEmpty())
            return;
        auto next = definitions_;
        next.removeIf([&id](const auto& d) { return d.id == id; });
        const auto saved = saveDynamicPlaylists(profile_, next);
        if (!saved) {
            status_->setText(QString::fromStdString(saved.error().message));
            return;
        }
        definitions_ = std::move(next);
        refill();
        loadSelection();
    });
    const auto loaded = loadDynamicPlaylists(profile_);
    if (loaded)
        definitions_ = *loaded;
    else {
        save->setEnabled(false);
        remove->setEnabled(false);
    }
    refill();
    loadSelection();
    if (!loaded)
        status_->setText(QString::fromStdString(loaded.error().message));
}
DynamicPlaylistDialog::~DynamicPlaylistDialog() { service_->cancel(); }
QString DynamicPlaylistDialog::playlistName() const { return name_->text().trimmed(); }
void DynamicPlaylistDialog::playCurrent() {
    if (authority_valid_ && view_->currentIndex().isValid())
        emit playRequested(view_->currentIndex().row());
}
void DynamicPlaylistDialog::refill(const QString& selected) {
    catalog_->clear();
    catalog_->addItem(QStringLiteral("New dynamic playlist…"), QString{});
    for (const auto& d : definitions_)
        catalog_->addItem(d.name, d.id);
    const auto index = catalog_->findData(selected);
    catalog_->setCurrentIndex(index < 0 ? 0 : index);
}
DynamicPlaylistDefinition DynamicPlaylistDialog::definition() const {
    return {.id = catalog_->currentData().toString(),
            .name = name_->text().trimmed(),
            .profile = profile_,
            .source = source_->currentData().toString(),
            .query = query_->text(),
            .artist = artist_->text(),
            .track = track_->text(),
            .user = user_->text(),
            .tag = tag_->text(),
            .limit = limit_->value(),
            .shuffle = shuffle_->isChecked()};
}
void DynamicPlaylistDialog::loadSelection() {
    loading_ = true;
    DynamicPlaylistDefinition d;
    for (const auto& entry : definitions_)
        if (entry.id == catalog_->currentData().toString())
            d = entry;
    name_->setText(d.name);
    source_->setCurrentIndex(source_->findData(d.source));
    query_->setText(d.query);
    artist_->setText(d.artist);
    track_->setText(d.track);
    user_->setText(d.user);
    tag_->setText(d.tag);
    limit_->setValue(d.limit);
    shuffle_->setChecked(d.shuffle);
    loading_ = false;
    updateFields();
    discardResults();
    status_->setText(QStringLiteral("Choose Refresh to evaluate this definition."));
    if (!d.id.isEmpty() && d.source == QStringLiteral("rules"))
        refresh();
}
void DynamicPlaylistDialog::updateFields() {
    const auto source = source_->currentData().toString();
    shuffle_->setText(source == QStringLiteral("rules")
                          ? QStringLiteral("Shuffle results on refresh")
                          : QStringLiteral("Shuffle selected tracks"));
    shuffle_->setToolTip(source == QStringLiteral("rules")
                             ? QString{}
                             : QStringLiteral("Each refresh picks a fresh selection, favouring "
                                              "tracks outside the previous result. "
                                              "This option also randomizes their order; otherwise "
                                              "Last.fm ranking determines the order."));
    form_->setRowVisible(query_, source == QStringLiteral("rules"));
    form_->setRowVisible(artist_, source == QStringLiteral("similar"));
    form_->setRowVisible(track_, source == QStringLiteral("similar"));
    form_->setRowVisible(user_,
                         source == QStringLiteral("loved") || source == QStringLiteral("top"));
    form_->setRowVisible(tag_, source == QStringLiteral("tag"));
}
void DynamicPlaylistDialog::discardResults() {
    if (loading_)
        return;
    auto_refresh_ = false;
    refresh_pending_ = false;
    refresh_timer_->stop();
    service_->cancel();
    busy_ = false;
    refresh_->setEnabled(authority_valid_);
    open_->setEnabled(false);
    local_model_->replaceRows({});
}
void DynamicPlaylistDialog::refresh(const bool) {
    if (!authority_valid_)
        return;
    // Definition edits discard explicitly; refresh retains presentation anchors.
    refresh_timer_->stop();
    service_->cancel();
    open_->setEnabled(false);
    auto_refresh_ = true;
    refresh_pending_ = false;
    busy_ = true;
    refresh_->setEnabled(false);
    service_->refresh(definition(), QSettings{}.value(QStringLiteral("lastfm/api-key")).toString());
}
void DynamicPlaylistDialog::libraryChanged() {
    if (!authority_valid_ || !auto_refresh_ || source_->currentData() != QStringLiteral("rules") ||
        query_->text().isEmpty())
        return;
    if (busy_)
        refresh_pending_ = true;
    else
        refresh_timer_->start();
}
void DynamicPlaylistDialog::invalidateAuthority() {
    authority_valid_ = false;
    discardResults();
    status_->setText(QStringLiteral(
        "The server connection changed. Reopen Dynamic playlists for the current library."));
}
} // namespace trackknife::bench
