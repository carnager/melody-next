// SPDX-License-Identifier: GPL-3.0-only
#include "bench/dynamic_playlist_dialog.hpp"

#include "bench/bench_main_window_helpers.hpp"
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
#include <QSpinBox>
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
DynamicPlaylistDialog::DynamicPlaylistDialog(QString profile, std::vector<Library> libraries,
                                             LibrarySearch search, QWidget* parent)
    : QDialog(parent), session_(new DynamicPlaylistSession(std::move(profile), std::move(libraries),
                                                           std::move(search), this)) {
    setObjectName(QStringLiteral("bench-dynamic-playlists"));
    setWindowTitle(QStringLiteral("Dynamic playlists"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(900, 720);
    auto* layout = new QVBoxLayout(this);
    auto* explanation = new QLabel(
        QStringLiteral("Save rules or a Last.fm source, then refresh to see the matching tracks. "
                       "Rules update while this window is open. Last.fm refreshes draw a fresh "
                       "selection when requested. "
                       "Opening a snapshot keeps that list stable while you listen."),
        this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    // As in Search: the library searched, and so the engine that plays what
    // is found (ADR-0227). The definitions are the same for both.
    library_ = new QComboBox(this);
    library_->setObjectName(QStringLiteral("dynamic-library"));
    library_->setAccessibleName(QStringLiteral("Library"));
    const auto libraries_shown = session_->libraryNames();
    for (int index = 0; index < libraries_shown.size(); ++index) {
        library_->addItem(libraries_shown.at(index), session_->libraryKey(index));
    }
    auto* catalog_row = new QHBoxLayout;
    catalog_ = new QComboBox(this);
    catalog_->setObjectName(QStringLiteral("dynamic-catalog"));
    catalog_->setAccessibleName(QStringLiteral("Saved dynamic playlists"));
    catalog_row->addWidget(library_);
    catalog_row->addWidget(catalog_, 1);
    save_ = new QPushButton(QStringLiteral("Save definition"), this);
    save_->setObjectName(QStringLiteral("dynamic-save"));
    remove_ = new QPushButton(QStringLiteral("Remove"), this);
    remove_->setObjectName(QStringLiteral("dynamic-remove"));
    catalog_row->addWidget(save_);
    catalog_row->addWidget(remove_);
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
    for (const auto& choice : DynamicPlaylistSession::sources()) {
        source_->addItem(choice.label, choice.value);
    }
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
    // ADR-0258: groups of the matches -- albums, artists -- each as likely
    // as any other, whatever its size.
    group_by_ = line(QStringLiteral("Pick groups by:"), QStringLiteral("dynamic-group-by"));
    group_by_->setPlaceholderText(QStringLiteral("%albumartist%"));
    group_by_->setToolTip(
        QStringLiteral("A formatting expression; tracks giving the same text form a group. "
                       "Example: %albumartist% — %album% for albums"));
    groups_ = new QSpinBox(this);
    groups_->setObjectName(QStringLiteral("dynamic-groups"));
    groups_->setRange(0, 500);
    groups_->setSpecialValueText(QStringLiteral("No groups"));
    form_->addRow(QStringLiteral("Groups:"), groups_);
    per_group_ = new QSpinBox(this);
    per_group_->setObjectName(QStringLiteral("dynamic-per-group"));
    per_group_->setRange(0, 500);
    per_group_->setSpecialValueText(QStringLiteral("All"));
    form_->addRow(QStringLiteral("Tracks per group:"), per_group_);
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
    view_->setModel(session_->results());
    // Hidden by default; the owning window supplies the authority's history service.
    view_->setColumnHidden(ui::track_play_count_column, true);
    view_->setColumnHidden(ui::track_last_played_column, true);
    layout->addWidget(view_, 1);

    connect(session_, &DynamicPlaylistSession::changed, this, &DynamicPlaylistDialog::sync);
    connect(session_, &DynamicPlaylistSession::definitionsSaved, this,
            &DynamicPlaylistDialog::definitionsSaved);
    connect(session_, &DynamicPlaylistSession::catalogChanged, this,
            &DynamicPlaylistDialog::syncCatalog);
    connect(session_, &DynamicPlaylistSession::resultsAboutToChange, this,
            &DynamicPlaylistDialog::keepPlace);
    connect(session_, &DynamicPlaylistSession::resultsChanged, this, [this] {
        restorePlace();
        emit resultsChanged();
    });
    connect(session_, &DynamicPlaylistSession::libraryChosen, this,
            [this](const EngineKey& chosen) {
                markViewEngine(view_, chosen);
                emit libraryChosen(chosen);
            });
    connect(refresh_, &QPushButton::clicked, session_, &DynamicPlaylistSession::refresh);
    connect(stop, &QPushButton::clicked, session_, &DynamicPlaylistSession::stop);
    connect(open_, &QPushButton::clicked, this,
            [this] { emit snapshotRequested(session_->playlistName(), session_->tracks()); });
    connect(catalog_, &QComboBox::activated, session_, &DynamicPlaylistSession::selectDefinition);
    connect(library_, &QComboBox::currentIndexChanged, session_,
            &DynamicPlaylistSession::chooseLibrary);
    connect(source_, &QComboBox::currentIndexChanged, this,
            [this] { session_->setSource(source_->currentData().toString()); });
    connect(name_, &QLineEdit::textChanged, session_, &DynamicPlaylistSession::setName);
    connect(query_, &QLineEdit::textChanged, session_, &DynamicPlaylistSession::setQuery);
    connect(artist_, &QLineEdit::textChanged, session_, &DynamicPlaylistSession::setArtist);
    connect(track_, &QLineEdit::textChanged, session_, &DynamicPlaylistSession::setTrack);
    connect(user_, &QLineEdit::textChanged, session_, &DynamicPlaylistSession::setUser);
    connect(tag_, &QLineEdit::textChanged, session_, &DynamicPlaylistSession::setTag);
    connect(limit_, &QSpinBox::valueChanged, session_, &DynamicPlaylistSession::setLimit);
    connect(shuffle_, &QCheckBox::toggled, session_, &DynamicPlaylistSession::setShuffle);
    connect(group_by_, &QLineEdit::textChanged, session_, &DynamicPlaylistSession::setGroupBy);
    connect(groups_, &QSpinBox::valueChanged, session_, &DynamicPlaylistSession::setGroups);
    connect(per_group_, &QSpinBox::valueChanged, session_, &DynamicPlaylistSession::setPerGroup);
    connect(save_, &QPushButton::clicked, session_, &DynamicPlaylistSession::save);
    connect(remove_, &QPushButton::clicked, session_, &DynamicPlaylistSession::remove);
    syncCatalog();
    sync();
}
DynamicPlaylistDialog::~DynamicPlaylistDialog() = default;
void DynamicPlaylistDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    session_->setShown(true);
}
void DynamicPlaylistDialog::hideEvent(QHideEvent* event) {
    QDialog::hideEvent(event);
    session_->setShown(false);
}
void DynamicPlaylistDialog::playCurrent() {
    if (session_->authorityValid() && view_->currentIndex().isValid())
        emit playRequested(view_->currentIndex().row());
}
void DynamicPlaylistDialog::syncCatalog() {
    const QSignalBlocker blocker{catalog_};
    catalog_->clear();
    catalog_->addItems(session_->catalogNames());
    catalog_->setCurrentIndex(session_->catalogIndex());
    save_->setEnabled(session_->catalogWritable());
    remove_->setEnabled(session_->catalogWritable() && !session_->definitionShipped());
    save_->setText(session_->definitionShipped() ? QStringLiteral("Save as copy")
                                                 : QStringLiteral("Save definition"));
}
void DynamicPlaylistDialog::sync() {
    const auto show = [](QLineEdit* field, const QString& text) {
        if (field->text() != text) {
            const QSignalBlocker blocker{field};
            field->setText(text);
        }
    };
    {
        const QSignalBlocker library_blocker{library_};
        const QSignalBlocker source_blocker{source_};
        const QSignalBlocker limit_blocker{limit_};
        const QSignalBlocker shuffle_blocker{shuffle_};
        const QSignalBlocker groups_blocker{groups_};
        const QSignalBlocker per_group_blocker{per_group_};
        groups_->setValue(session_->groups());
        per_group_->setValue(session_->perGroup());
        library_->setCurrentIndex(session_->library());
        source_->setCurrentIndex(source_->findData(session_->source()));
        limit_->setValue(session_->limit());
        shuffle_->setChecked(session_->shuffle());
    }
    show(name_, session_->name());
    show(query_, session_->query());
    show(artist_, session_->artist());
    show(track_, session_->track());
    show(user_, session_->user());
    show(tag_, session_->tag());
    show(group_by_, session_->groupBy());
    const auto source = session_->source();
    shuffle_->setText(session_->shuffleText());
    shuffle_->setToolTip(session_->shuffleTip());
    form_->setRowVisible(query_, source == QStringLiteral("rules"));
    form_->setRowVisible(group_by_, source == QStringLiteral("rules"));
    form_->setRowVisible(groups_, source == QStringLiteral("rules"));
    form_->setRowVisible(per_group_, source == QStringLiteral("rules") && session_->groups() > 0);
    form_->setRowVisible(artist_, source == QStringLiteral("similar"));
    form_->setRowVisible(track_, source == QStringLiteral("similar"));
    form_->setRowVisible(user_,
                         source == QStringLiteral("loved") || source == QStringLiteral("top"));
    form_->setRowVisible(tag_, source == QStringLiteral("tag"));
    status_->setText(session_->status());
    refresh_->setEnabled(session_->canRefresh());
    open_->setEnabled(session_->canOpen());
}
void DynamicPlaylistDialog::keepPlace() {
    const auto keys = resultKeys(view_->model());
    kept_selected_.clear();
    for (const auto& index : view_->selectionModel()->selectedRows())
        kept_selected_.insert(keys.value(index.row()));
    kept_current_ = keys.value(view_->currentIndex().row());
    const auto top = view_->indexAt(QPoint{1, 1});
    kept_top_ = keys.value(top.row());
    kept_offset_ = top.isValid() ? view_->visualRect(top).top() : 0;
    kept_horizontal_ = view_->horizontalScrollBar()->value();
}
void DynamicPlaylistDialog::restorePlace() {
    const auto keys = resultKeys(view_->model());
    view_->selectionModel()->clearSelection();
    for (int i = 0; i < keys.size(); ++i) {
        const auto index = view_->model()->index(i, 0);
        if (kept_selected_.contains(keys[i]))
            view_->selectionModel()->select(index, QItemSelectionModel::Select |
                                                       QItemSelectionModel::Rows);
        if (!kept_current_.isEmpty() && keys[i] == kept_current_)
            view_->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
        if (!kept_top_.isEmpty() && keys[i] == kept_top_) {
            view_->scrollTo(index, QAbstractItemView::PositionAtTop);
            if (view_->verticalScrollMode() == QAbstractItemView::ScrollPerPixel)
                view_->verticalScrollBar()->setValue(view_->verticalScrollBar()->value() -
                                                     kept_offset_);
        }
    }
    view_->horizontalScrollBar()->setValue(kept_horizontal_);
}
} // namespace trackknife::bench
