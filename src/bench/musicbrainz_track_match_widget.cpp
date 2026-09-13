// SPDX-License-Identifier: GPL-3.0-only

#include "bench/musicbrainz_track_match_widget.hpp"

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

QString text(const std::string& value) { return QString::fromUtf8(value); }

QString duration(const std::optional<std::int64_t> milliseconds) {
    if (!milliseconds || *milliseconds < 0) {
        return QStringLiteral("—");
    }
    const auto seconds = *milliseconds / 1'000;
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char{'0'});
}

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

  private:
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

class TrackMatchWidget final : public QWidget {
  public:
    TrackMatchWidget(musicbrainz::Release release,
                     std::vector<musicbrainz::LocalTrackDescriptor> local_tracks,
                     std::vector<QString> local_paths, std::vector<std::size_t> item_indexes,
                     std::function<void(metadata::MetadataProposalSet)> accepted,
                     std::function<void()> back, QWidget* parent)
        : QWidget(parent), release_(std::move(release)), local_tracks_(std::move(local_tracks)),
          local_paths_(std::move(local_paths)), item_indexes_(std::move(item_indexes)),
          accepted_(std::move(accepted)) {
        setObjectName(QStringLiteral("bench-musicbrainz-track-match"));
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* heading =
            new QLabel(tr("Match files to %1 · %2 · %3")
                           .arg(text(release_.title), text(release_.date), text(release_.country)),
                       this);
        heading->setTextFormat(Qt::PlainText);
        heading->setWordWrap(true);
        layout->addWidget(heading);
        auto* help = new QLabel(
            tr("Each row pairs a local file with the MusicBrainz track beside it. "
               "Drag files in the left pane or use Move file up/down to change pairings. "
               "MusicBrainz tracks stay in album order. Review before staging."),
            this);
        help->setWordWrap(true);
        layout->addWidget(help);
        auto* splitter = new QSplitter(this);
        rows_ = new LocalFileOrderView(splitter);
        rows_->setObjectName(QStringLiteral("bench-musicbrainz-match-files"));
        rows_->setHeaderLabels({tr("Local filename"), tr("Length"), tr("Pairing")});
        tracks_ = new QTreeWidget(splitter);
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
            if (!ready_ || from >= slots_.size() || to >= slots_.size() || from == to) {
                return;
            }
            const auto local = slots_[from];
            slots_.erase(slots_.begin() + static_cast<std::ptrdiff_t>(from));
            slots_.insert(slots_.begin() + static_cast<std::ptrdiff_t>(to), local);
            refresh(to);
        };
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
        layout->addLayout(actions);
        status_ = new QLabel(tr("Preparing suggested matches…"), this);
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

        connect(rows_, &QTreeWidget::itemSelectionChanged, this, [this] { updateButtons(); });
        connect(up_, &QPushButton::clicked, this, [this] { moveFile(-1); });
        connect(down_, &QPushButton::clicked, this, [this] { moveFile(1); });
        for (const auto& entry : {std::pair{QKeySequence{Qt::ALT | Qt::Key_Up}, -1},
                                  std::pair{QKeySequence{Qt::ALT | Qt::Key_Down}, 1}}) {
            auto* shortcut = new QShortcut(entry.first, rows_);
            shortcut->setContext(Qt::WidgetWithChildrenShortcut);
            connect(shortcut, &QShortcut::activated, this,
                    [this, direction = entry.second] { moveFile(direction); });
        }
        connect(unmatch_, &QPushButton::clicked, this, [this] {
            const auto row = selectedRow();
            if (!ready_ || !row || *row >= alignment_.release_tracks.size() || !slots_[*row]) {
                return;
            }
            const auto local = slots_[*row];
            slots_[*row].reset();
            slots_.push_back(local);
            refresh(slots_.size() - 1);
        });
        connect(sort_, &QPushButton::clicked, this, [this] { resetOrder(true); });
        connect(order_, &QPushButton::clicked, this, [this] { resetOrder(false); });
        connect(stage_, &QPushButton::clicked, this, [this] { stage(); });
        updateButtons();

        std::size_t release_track_count = 0;
        for (const auto& medium : release_.media) {
            release_track_count += medium.tracks.size();
        }
        if (local_tracks_.size() > 2'000U || release_track_count > 2'000U ||
            local_tracks_.size() != item_indexes_.size()) {
            status_->setText(tr("Track matching supports up to 2,000 local files and 2,000 release "
                                "tracks. Choose a smaller selection."));
            return;
        }
        // Keep the potentially expensive similarity matcher off the UI thread.
        auto* watcher = new QFutureWatcher<musicbrainz::ReleaseAlignment>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
            alignment_ = watcher->result();
            watcher->deleteLater();
            slots_.resize(alignment_.release_tracks.size());
            std::vector<bool> placed(local_tracks_.size(), false);
            for (std::size_t local = 0; local < local_tracks_.size(); ++local) {
                const auto& match = alignment_.tracks[local];
                if (match.confidence >= 0.5 && match.release_track_index &&
                    *match.release_track_index < slots_.size() &&
                    !slots_[*match.release_track_index]) {
                    slots_[*match.release_track_index] = local;
                    placed[local] = true;
                }
            }
            // Fill remaining gaps in file order as reviewable proposals, not confidence claims.
            std::size_t gap = 0;
            for (std::size_t local = 0; local < local_tracks_.size(); ++local) {
                if (placed[local]) {
                    continue;
                }
                while (gap < slots_.size() && slots_[gap]) {
                    ++gap;
                }
                if (gap == slots_.size()) {
                    slots_.push_back(local);
                } else {
                    slots_[gap] = local;
                }
            }
            ready_ = true;
            refresh();
        });
        watcher->setFuture(QtConcurrent::run(
            [release = release_, tracks = local_tracks_, token = cancellation_.token()] {
                return musicbrainz::align_release_tracks(tracks, release, token);
            }));
    }

    ~TrackMatchWidget() override { cancellation_.request_cancellation(); }

  private:
    std::optional<std::size_t> selectedRow() const {
        const auto row = rows_->indexOfTopLevelItem(rows_->currentItem());
        return row < 0 ? std::nullopt : std::optional{static_cast<std::size_t>(row)};
    }
    QString localPath(std::size_t local) const {
        if (local < local_paths_.size() && !local_paths_[local].isEmpty()) {
            return local_paths_[local];
        }
        return local_tracks_[local].title.empty() ? tr("File %1").arg(local + 1)
                                                  : text(local_tracks_[local].title);
    }
    void resetOrder(bool sort) {
        if (!ready_) {
            return;
        }
        std::vector<std::size_t> files;
        for (std::size_t local = 0; local < local_tracks_.size(); ++local) {
            files.push_back(local);
        }
        if (sort) {
            QCollator collator;
            if (collator.locale().language() == QLocale::C) {
                collator.setLocale(QLocale{QLocale::English});
            }
            collator.setNumericMode(true);
            collator.setCaseSensitivity(Qt::CaseInsensitive);
            std::stable_sort(files.begin(), files.end(), [this, &collator](auto a, auto b) {
                return collator.compare(QFileInfo(localPath(a)).fileName(),
                                        QFileInfo(localPath(b)).fileName()) < 0;
            });
        }
        slots_.assign(std::max(files.size(), alignment_.release_tracks.size()), std::nullopt);
        for (std::size_t row = 0; row < files.size(); ++row) {
            slots_[row] = files[row];
        }
        refresh(0);
    }
    void moveFile(int direction) {
        const auto row = selectedRow();
        if (!ready_ || !row || (direction < 0 && *row == 0) ||
            (direction > 0 && *row + 1 >= slots_.size())) {
            return;
        }
        const auto target = direction < 0 ? *row - 1 : *row + 1;
        std::swap(slots_[*row], slots_[target]);
        refresh(target);
    }
    QString trackLabel(const std::size_t target) const {
        const auto& track = alignment_.release_tracks[target];
        return tr("%1.%2 · %3")
            .arg(track.medium_position)
            .arg(track.track.position)
            .arg(text(track.track.title));
    }
    void updateButtons() {
        const auto row = selectedRow();
        up_->setEnabled(ready_ && row && *row > 0);
        down_->setEnabled(ready_ && row && *row + 1 < slots_.size());
        unmatch_->setEnabled(ready_ && row && *row < alignment_.release_tracks.size() &&
                             slots_[*row]);
        sort_->setEnabled(ready_);
        order_->setEnabled(ready_);
        stage_->setEnabled(ready_ && std::ranges::any_of(assignments_, [](const auto& value) {
                               return value.has_value();
                           }));
    }
    void refresh(std::optional<std::size_t> selected = std::nullopt) {
        while (slots_.size() > alignment_.release_tracks.size() && !slots_.back()) {
            slots_.pop_back();
        }
        rows_->invalidateDrag();
        assignments_.assign(local_tracks_.size(), std::nullopt);
        rows_->clear();
        tracks_->clear();
        std::size_t count = 0;
        for (std::size_t row = 0; row < slots_.size(); ++row) {
            auto* item = new QTreeWidgetItem(rows_);
            item->setFlags(item->flags() & ~Qt::ItemIsDropEnabled);
            auto* track_item = new QTreeWidgetItem(tracks_);
            // Equal row heights keep the two panes aligned, including gaps.
            const auto height = fontMetrics().height() + 12;
            item->setSizeHint(0, QSize{0, height});
            track_item->setSizeHint(0, QSize{0, height});
            if (const auto local = slots_[row]) {
                item->setText(0, QFileInfo(localPath(*local)).fileName());
                item->setToolTip(0, localPath(*local));
                item->setText(1, duration(local_tracks_[*local].duration_ms));
                if (row < alignment_.release_tracks.size()) {
                    assignments_[*local] = row;
                    const auto& track = alignment_.release_tracks[row];
                    item->setText(
                        2, tr("→ %1.%2").arg(track.medium_position).arg(track.track.position));
                    item->setToolTip(
                        2, tr("Paired with %1. Review before staging.").arg(trackLabel(row)));
                    auto font = item->font(2);
                    font.setBold(true);
                    item->setFont(2, font);
                    item->setForeground(2, palette().brush(QPalette::Link));
                    const auto base = palette().color(QPalette::Base);
                    const auto accent = palette().color(QPalette::Highlight);
                    const QColor tint{(base.red() * 9 + accent.red()) / 10,
                                      (base.green() * 9 + accent.green()) / 10,
                                      (base.blue() * 9 + accent.blue()) / 10};
                    for (int column = 0; column < 3; ++column) {
                        item->setBackground(column, tint);
                    }
                    for (int column = 0; column < 2; ++column) {
                        track_item->setBackground(column, tint);
                    }
                    ++count;
                }
            } else {
                item->setText(0, tr("No local file"));
                item->setText(2, tr("— Gap"));
            }
            if (row < alignment_.release_tracks.size()) {
                track_item->setText(0, trackLabel(row));
                track_item->setToolTip(0, trackLabel(row));
                track_item->setText(1, duration(alignment_.release_tracks[row].track.length_ms));
            } else {
                item->setText(2, tr("Unmatched"));
                track_item->setText(0, tr("— No tags will be staged"));
            }
        }
        if (selected && !slots_.empty()) {
            auto* item =
                rows_->topLevelItem(static_cast<int>(std::min(*selected, slots_.size() - 1)));
            rows_->setCurrentItem(item);
            rows_->scrollToItem(item);
        }
        status_->setText(
            tr("%1 files paired · %2 files unmatched · %3 album tracks without a file. "
               "Pairings are suggestions until you Stage matches.")
                .arg(count)
                .arg(local_tracks_.size() - count)
                .arg(alignment_.release_tracks.size() - count));
        updateButtons();
    }
    void stage() {
        if (!ready_ || !stage_->isEnabled()) {
            return;
        }
        auto confirmed = musicbrainz::confirm_release_mapping(alignment_, assignments_);
        if (!confirmed) {
            status_->setText(text(confirmed.error().message));
            return;
        }
        auto proposals =
            musicbrainz::release_metadata_proposals(release_, *confirmed, item_indexes_);
        if (!proposals) {
            status_->setText(text(proposals.error().message));
            return;
        }
        if (accepted_) {
            accepted_(std::move(*proposals));
        }
    }

    musicbrainz::Release release_;
    std::vector<musicbrainz::LocalTrackDescriptor> local_tracks_;
    std::vector<QString> local_paths_;
    std::vector<std::size_t> item_indexes_;
    std::function<void(metadata::MetadataProposalSet)> accepted_;
    musicbrainz::ReleaseAlignment alignment_;
    std::vector<std::optional<std::size_t>> assignments_;
    std::vector<std::optional<std::size_t>> slots_;
    LocalFileOrderView* rows_;
    QTreeWidget* tracks_;
    QPushButton* up_;
    QPushButton* down_;
    QPushButton* sort_;
    QPushButton* unmatch_;
    QPushButton* order_;
    QPushButton* stage_;
    QLabel* status_;
    bool ready_{false};
    core::CancellationSource cancellation_;
};

} // namespace

QWidget* createMusicBrainzTrackMatchWidget(
    musicbrainz::Release release, std::vector<musicbrainz::LocalTrackDescriptor> local_tracks,
    std::vector<QString> local_paths, std::vector<std::size_t> item_indexes,
    std::function<void(metadata::MetadataProposalSet)> accepted, std::function<void()> back,
    QWidget* parent) {
    return new TrackMatchWidget(std::move(release), std::move(local_tracks), std::move(local_paths),
                                std::move(item_indexes), std::move(accepted), std::move(back),
                                parent);
}

} // namespace trackknife::bench
