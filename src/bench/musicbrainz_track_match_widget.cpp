// SPDX-License-Identifier: GPL-3.0-only

#include "bench/musicbrainz_track_match_widget.hpp"
#include "workspace/identify_session.hpp"

#include <QCollator>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMimeData>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSplitter>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <optional>
#include <utility>

namespace trackknife::bench {
namespace {

// Own the drag lifecycle so Qt never deletes source rows after the mapping
// controller has already moved them. External/cross-window drops are rejected.
class LocalFileOrderView final : public QTreeWidget {
  public:
    explicit LocalFileOrderView(QWidget* parent) : QTreeWidget(parent) {
        setDragEnabled(true);
        setAcceptDrops(true);
        setDragDropMode(QAbstractItemView::DragDrop);
        setDefaultDropAction(Qt::MoveAction);
        setDropIndicatorShown(true);
        setAutoScroll(true);
    }
    std::function<void(std::size_t, std::size_t)> moveFile;
    // ADR-0265: dropped on a gap, a file fills it, nothing else moving.
    std::function<bool(std::size_t)> isGap;
    std::function<void(std::size_t, std::size_t)> fillGap;
    void invalidateDrag() { identity_ = QUuid::createUuid().toString(); }

  protected:
    QStringList mimeTypes() const override {
        return {QStringLiteral("application/x-trackbench-match-row")};
    }
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override {
        if (items.size() != 1) {
            return nullptr;
        }
        auto* mime = new QMimeData;
        mime->setData(mimeTypes().front(),
                      identity_.toUtf8() + ':' +
                          QByteArray::number(indexOfTopLevelItem(items.front())));
        return mime;
    }
    void startDrag(Qt::DropActions) override {
        auto* mime = mimeData(selectedItems());
        if (!mime) {
            return;
        }
        QDrag drag{this};
        drag.setMimeData(mime);
        if (currentItem()) {
            drag.setPixmap(viewport()->grab(visualItemRect(currentItem())));
        }
        drag.exec(Qt::MoveAction);
    }
    void dragEnterEvent(QDragEnterEvent* event) override {
        if (sourceRow(event->mimeData())) {
            event->acceptProposedAction();
        } else {
            event->ignore();
        }
    }
    void dragMoveEvent(QDragMoveEvent* event) override {
        if (!sourceRow(event->mimeData())) {
            event->ignore();
            return;
        }
        QTreeWidget::dragMoveEvent(event);
        event->acceptProposedAction();
    }
    void dropEvent(QDropEvent* event) override {
        const auto source = sourceRow(event->mimeData());
        if (!source || !moveFile) {
            event->ignore();
            return;
        }
        auto* target = itemAt(event->position().toPoint());
        if (target && isGap && fillGap) {
            const auto row = static_cast<std::size_t>(indexOfTopLevelItem(target));
            if (isGap(row)) {
                fillGap(*source, row);
                event->setDropAction(Qt::MoveAction);
                event->accept();
                return;
            }
        }
        std::size_t boundary = static_cast<std::size_t>(topLevelItemCount());
        if (target) {
            boundary = static_cast<std::size_t>(indexOfTopLevelItem(target));
            if (event->position().y() > visualItemRect(target).center().y()) {
                ++boundary;
            }
        }
        if (boundary > *source) {
            --boundary;
        }
        moveFile(*source, boundary);
        event->setDropAction(Qt::MoveAction);
        event->accept();
    }

  public:
    // The row a drag from this view started at, if it is one of its own.
    std::optional<std::size_t> sourceRow(const QMimeData* mime) const {
        const auto payload = mime->data(mimeTypes().front());
        const auto prefix = identity_.toUtf8() + ':';
        if (!payload.startsWith(prefix)) {
            return std::nullopt;
        }
        bool valid = false;
        const auto row = payload.sliced(prefix.size()).toULongLong(&valid);
        if (!valid || row >= static_cast<qulonglong>(topLevelItemCount())) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(row);
    }
    QString identity_{QUuid::createUuid().toString()};
};

// ADR-0265: a file dragged from the files pane onto a MusicBrainz track is
// put on that track: in its gap, or in place of the file there, which takes
// the dragged file's place.
class ReleaseTrackDropView final : public QTreeWidget {
  public:
    ReleaseTrackDropView(QWidget* parent, LocalFileOrderView* files)
        : QTreeWidget(parent), files_(files) {
        setAcceptDrops(true);
        setDragDropMode(QAbstractItemView::DropOnly);
        setDropIndicatorShown(true);
    }
    std::function<void(std::size_t, std::size_t)> placeOn;

  protected:
    void dragEnterEvent(QDragEnterEvent* event) override {
        if (files_->sourceRow(event->mimeData())) {
            event->acceptProposedAction();
        } else {
            event->ignore();
        }
    }
    void dragMoveEvent(QDragMoveEvent* event) override {
        auto* target = itemAt(event->position().toPoint());
        if (!files_->sourceRow(event->mimeData()) || target == nullptr) {
            event->ignore();
            return;
        }
        setCurrentItem(target);
        event->acceptProposedAction();
    }
    void dropEvent(QDropEvent* event) override {
        const auto source = files_->sourceRow(event->mimeData());
        auto* target = itemAt(event->position().toPoint());
        if (!source || target == nullptr || !placeOn) {
            event->ignore();
            return;
        }
        placeOn(*source, static_cast<std::size_t>(indexOfTopLevelItem(target)));
        event->setDropAction(Qt::MoveAction);
        event->accept();
    }

  private:
    LocalFileOrderView* files_;
};

// A view over a TrackMatchSession: the files and the album's tracks side by
// side, scrolled together, a row's pair shaded.
class TrackMatchWidget final : public QWidget {
  public:
    // Embedded, it is the panes and their buttons only: the window around it
    // names the release, says how far the pairing is, and stages.
    TrackMatchWidget(TrackMatchSession* session, std::function<void()> back, QWidget* parent,
                     const bool embedded = false)
        : QWidget(parent), session_(session) {
        setObjectName(QStringLiteral("bench-musicbrainz-track-match"));
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        if (!embedded) {
            auto* heading = new QLabel(session_->heading(), this);
            heading->setTextFormat(Qt::PlainText);
            heading->setWordWrap(true);
            layout->addWidget(heading);
            auto* help = new QLabel(TrackMatchSession::help(), this);
            help->setWordWrap(true);
            layout->addWidget(help);
        }
        auto* splitter = new QSplitter(this);
        rows_ = new LocalFileOrderView(splitter);
        rows_->setObjectName(QStringLiteral("bench-musicbrainz-match-files"));
        rows_->setHeaderLabels({tr("Local filename"), tr("Length"), tr("Pairing")});
        auto* tracks = new ReleaseTrackDropView(splitter, rows_);
        tracks->placeOn = [this](std::size_t from, std::size_t to) { session_->placeOn(from, to); };
        tracks_ = tracks;
        tracks_->setObjectName(QStringLiteral("bench-musicbrainz-match-tracks"));
        tracks_->setHeaderLabels({tr("MusicBrainz track"), tr("Length")});
        for (auto* view : {static_cast<QTreeWidget*>(rows_), tracks_}) {
            view->setAccessibleName(view->headerItem()->text(0));
            view->setRootIsDecorated(false);
            view->setUniformRowHeights(true);
            view->setAlternatingRowColors(true);
            view->setSelectionMode(QAbstractItemView::SingleSelection);
            view->setEditTriggers(QAbstractItemView::NoEditTriggers);
            view->setTextElideMode(Qt::ElideMiddle);
            view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
            view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            view->header()->setStretchLastSection(false);
            view->header()->setSectionResizeMode(0, QHeaderView::Stretch);
            view->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        }
        rows_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        connect(rows_->verticalScrollBar(), &QScrollBar::valueChanged, tracks_->verticalScrollBar(),
                &QScrollBar::setValue);
        connect(tracks_->verticalScrollBar(), &QScrollBar::valueChanged, rows_->verticalScrollBar(),
                &QScrollBar::setValue);
        connect(rows_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
            tracks_->setCurrentItem(tracks_->topLevelItem(rows_->indexOfTopLevelItem(item)));
        });
        connect(tracks_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
            rows_->setCurrentItem(rows_->topLevelItem(tracks_->indexOfTopLevelItem(item)));
        });
        rows_->moveFile = [this](std::size_t from, std::size_t to) {
            session_->moveFile(from, to);
        };
        rows_->isGap = [this](std::size_t row) { return session_->isGap(row); };
        rows_->fillGap = [this](std::size_t from, std::size_t gap) {
            session_->fillGap(from, gap);
        };
        if (embedded) {
            rows_->setToolTip(TrackMatchSession::help());
        }
        layout->addWidget(splitter, 1);
        auto* actions = new QHBoxLayout;
        const auto button = [this, actions](const QString& label, const QString& name) {
            auto* result = new QPushButton(label, this);
            result->setObjectName(name);
            actions->addWidget(result);
            return result;
        };
        up_ = button(tr("Move file up"), QStringLiteral("bench-musicbrainz-match-up"));
        down_ = button(tr("Move file down"), QStringLiteral("bench-musicbrainz-match-down"));
        up_->setToolTip(tr("Swap local files with the row above (Alt+Up)"));
        down_->setToolTip(tr("Swap local files with the row below (Alt+Down)"));
        unmatch_ = button(tr("Leave unmatched"), QStringLiteral("bench-musicbrainz-match-unmatch"));
        unmatch_->setToolTip(
            tr("Move this file below the album tracks, leaving a gap. It will receive no tags."));
        sort_ = button(tr("Match by filename"), QStringLiteral("bench-musicbrainz-match-sort"));
        sort_->setToolTip(tr("Pair files in natural filename order with the album tracks. This "
                             "replaces the current pairings."));
        order_ = button(tr("Reset file order"), QStringLiteral("bench-musicbrainz-match-order"));
        order_->setToolTip(
            tr("Pair files in their original selection order with the album tracks."));
        if (embedded) {
            // Buttons as wide as their words, not a row of equal slabs; what
            // works on a file at the right, there once a file is chosen.
            for (auto* file_button : {up_, down_, unmatch_}) {
                actions->removeWidget(file_button);
            }
            actions->addStretch(1);
            for (auto* file_button : {up_, down_, unmatch_}) {
                actions->addWidget(file_button);
            }
            embedded_ = true;
        }
        layout->addLayout(actions);
        status_ = new QLabel(this);
        status_->setObjectName(QStringLiteral("bench-musicbrainz-match-status"));
        status_->setWordWrap(true);
        layout->addWidget(status_);
        auto* footer = new QHBoxLayout;
        auto* back_button = new QPushButton(tr("Back to releases"), this);
        back_button->setObjectName(QStringLiteral("bench-musicbrainz-match-back"));
        connect(back_button, &QPushButton::clicked, this, [back = std::move(back)] { back(); });
        footer->addWidget(back_button);
        footer->addStretch();
        stage_ = new QPushButton(tr("Stage matches"), this);
        stage_->setObjectName(QStringLiteral("bench-musicbrainz-match-stage"));
        stage_->setToolTip(tr("Confirm the displayed assignments and stage tags for matched files. "
                              "Unmatched files are untouched; Apply writes the draft later."));
        footer->addWidget(stage_);
        layout->addLayout(footer);
        if (embedded) {
            status_->hide();
            back_button->hide();
            stage_->hide();
        }

        connect(rows_, &QTreeWidget::itemSelectionChanged, this, [this] { updateButtons(); });
        connect(up_, &QPushButton::clicked, this, [this] { session_->move(selectedRow(), -1); });
        connect(down_, &QPushButton::clicked, this, [this] { session_->move(selectedRow(), 1); });
        for (const auto& entry : {std::pair{QKeySequence{Qt::ALT | Qt::Key_Up}, -1},
                                  std::pair{QKeySequence{Qt::ALT | Qt::Key_Down}, 1}}) {
            auto* shortcut = new QShortcut(entry.first, rows_);
            shortcut->setContext(Qt::WidgetWithChildrenShortcut);
            connect(shortcut, &QShortcut::activated, this,
                    [this, direction = entry.second] { session_->move(selectedRow(), direction); });
        }
        connect(unmatch_, &QPushButton::clicked, this,
                [this] { session_->unmatch(selectedRow()); });
        // ADR-0261: U leaves the file unmatched, from the keyboard.
        auto* unmatch_key = new QShortcut(QKeySequence{Qt::Key_U}, rows_);
        unmatch_key->setContext(Qt::WidgetWithChildrenShortcut);
        connect(unmatch_key, &QShortcut::activated, this, [this] {
            if (session_->canUnmatch(selectedRow())) {
                session_->unmatch(selectedRow());
            }
        });
        connect(sort_, &QPushButton::clicked, this, [this] { session_->resetOrder(true); });
        connect(order_, &QPushButton::clicked, this, [this] { session_->resetOrder(false); });
        connect(stage_, &QPushButton::clicked, session_, &TrackMatchSession::stage);
        connect(session_, &TrackMatchSession::changed, this, &TrackMatchWidget::refresh);
        refresh(-1);
    }

  private:
    [[nodiscard]] int selectedRow() const {
        return rows_->indexOfTopLevelItem(rows_->currentItem());
    }
    void updateButtons() {
        const auto row = selectedRow();
        up_->setEnabled(session_->canMoveUp(row));
        down_->setEnabled(session_->canMoveDown(row));
        unmatch_->setEnabled(session_->canUnmatch(row));
        if (embedded_) {
            for (auto* file_button : {up_, down_, unmatch_}) {
                file_button->setVisible(row >= 0);
            }
        }
        sort_->setEnabled(session_->ready());
        order_->setEnabled(session_->ready());
        stage_->setEnabled(session_->canStage());
    }
    void refresh(const int selected) {
        rows_->invalidateDrag();
        rows_->clear();
        tracks_->clear();
        const auto base = palette().color(QPalette::Base);
        const auto accent = palette().color(QPalette::Highlight);
        const QColor tint{(base.red() * 9 + accent.red()) / 10,
                          (base.green() * 9 + accent.green()) / 10,
                          (base.blue() * 9 + accent.blue()) / 10};
        // Equal row heights keep the two panes aligned, including gaps.
        const auto height = fontMetrics().height() + 12;
        for (const auto& row : session_->rows()) {
            auto* item = new QTreeWidgetItem(rows_);
            item->setFlags(item->flags() & ~Qt::ItemIsDropEnabled);
            auto* track_item = new QTreeWidgetItem(tracks_);
            item->setSizeHint(0, QSize{0, height});
            track_item->setSizeHint(0, QSize{0, height});
            item->setText(0, row.file);
            item->setToolTip(0, row.file_tool_tip);
            item->setText(1, row.length);
            item->setText(2, row.pairing);
            item->setToolTip(2, row.pairing_tool_tip);
            if (row.paired) {
                auto font = item->font(2);
                font.setBold(true);
                item->setFont(2, font);
                item->setForeground(2, palette().brush(QPalette::Link));
                for (int column = 0; column < 3; ++column) {
                    item->setBackground(column, tint);
                }
                for (int column = 0; column < 2; ++column) {
                    track_item->setBackground(column, tint);
                }
            }
            track_item->setText(0, row.track);
            track_item->setToolTip(0, row.track);
            track_item->setText(1, row.track_length);
            // Another album's file, offered: quieter than the album's own.
            if (row.other) {
                for (int column = 0; column < 3; ++column) {
                    auto font = item->font(column);
                    font.setItalic(true);
                    item->setFont(column, font);
                    item->setForeground(column, palette().brush(QPalette::PlaceholderText));
                }
                track_item->setForeground(0, palette().brush(QPalette::PlaceholderText));
            }
        }
        if (selected >= 0 && selected < rows_->topLevelItemCount()) {
            auto* item = rows_->topLevelItem(selected);
            rows_->setCurrentItem(item);
            rows_->scrollToItem(item);
        }
        status_->setText(session_->status());
        updateButtons();
    }

    TrackMatchSession* session_;
    LocalFileOrderView* rows_;
    QTreeWidget* tracks_;
    QPushButton* up_;
    QPushButton* down_;
    QPushButton* sort_;
    QPushButton* unmatch_;
    QPushButton* order_;
    QPushButton* stage_;
    QLabel* status_;
    bool embedded_{false};
};

} // namespace

QWidget* createMusicBrainzTrackMatchWidget(
    musicbrainz::Release release, std::vector<musicbrainz::LocalTrackDescriptor> local_tracks,
    std::vector<QString> local_paths, std::vector<std::size_t> item_indexes,
    std::function<void(metadata::MetadataProposalSet)> accepted, std::function<void()> back,
    QWidget* parent) {
    auto* session = new TrackMatchSession(std::move(release), std::move(local_tracks),
                                          std::move(local_paths), std::move(item_indexes));
    auto* widget = new TrackMatchWidget(session, std::move(back), parent);
    session->setParent(widget);
    QObject::connect(session, &TrackMatchSession::accepted, widget,
                     [accepted = std::move(accepted)](metadata::MetadataProposalSet proposals) {
                         if (accepted) {
                             accepted(std::move(proposals));
                         }
                     });
    return widget;
}

QWidget* createMusicBrainzTrackMatchView(TrackMatchSession* session, std::function<void()> back,
                                         QWidget* parent) {
    return new TrackMatchWidget(session, std::move(back), parent);
}

QWidget* createMusicBrainzTrackMatchPanes(TrackMatchSession* session, QWidget* parent) {
    return new TrackMatchWidget(session, {}, parent, true);
}

} // namespace trackknife::bench
