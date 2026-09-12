// SPDX-License-Identifier: GPL-3.0-only

#include "bench/musicbrainz_track_match_widget.hpp"

#include <QCollator>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
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
        auto* help =
            new QLabel(tr("Select a local file and a release track, then Assign. "
                          "For untagged albums, sort the files and use Match in file order. "
                          "Review the assignments before staging tags."),
                       this);
        help->setWordWrap(true);
        layout->addWidget(help);
        auto* splitter = new QSplitter(this);
        const auto tree = [splitter](const QString& name, const QStringList& headers) {
            auto* view = new QTreeWidget(splitter);
            view->setObjectName(name);
            view->setHeaderLabels(headers);
            view->setAccessibleName(headers.front());
            view->setRootIsDecorated(false);
            view->setUniformRowHeights(true);
            view->setAlternatingRowColors(true);
            view->setSelectionMode(QAbstractItemView::SingleSelection);
            view->setEditTriggers(QAbstractItemView::NoEditTriggers);
            view->header()->setSectionResizeMode(0, QHeaderView::Stretch);
            return view;
        };
        files_ = tree(QStringLiteral("bench-musicbrainz-match-files"),
                      {tr("Local file"), tr("Length"), tr("Assigned release track")});
        tracks_ = tree(QStringLiteral("bench-musicbrainz-match-tracks"),
                       {tr("Release track"), tr("Length"), tr("Assigned local file")});
        layout->addWidget(splitter, 1);
        auto* actions = new QHBoxLayout;
        const auto button = [this, actions](const QString& label, const QString& name) {
            auto* result = new QPushButton(label, this);
            result->setObjectName(name);
            actions->addWidget(result);
            return result;
        };
        assign_ = button(tr("Assign"), QStringLiteral("bench-musicbrainz-match-assign"));
        assign_->setToolTip(tr("Assign the selected pair. If the track is already assigned, swap "
                               "the two assignments."));
        unmatch_ = button(tr("Unmatch"), QStringLiteral("bench-musicbrainz-match-unmatch"));
        auto* sort =
            button(tr("Sort files by name"), QStringLiteral("bench-musicbrainz-match-sort"));
        order_ = button(tr("Match in file order"), QStringLiteral("bench-musicbrainz-match-order"));
        order_->setToolTip(tr("Replace assignments with the displayed file order, across all "
                              "release discs. Extra files remain unmatched."));
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

        connect(files_, &QTreeWidget::itemSelectionChanged, this, [this] { updateButtons(); });
        connect(tracks_, &QTreeWidget::itemSelectionChanged, this, [this] { updateButtons(); });
        connect(assign_, &QPushButton::clicked, this, [this] {
            const auto local = selected(files_);
            const auto target = selected(tracks_);
            if (!ready_ || !local || !target) {
                return;
            }
            const auto previous = assignments_[*local];
            for (std::size_t other = 0; other < assignments_.size(); ++other) {
                if (other != *local && assignments_[other] == target) {
                    assignments_[other] = previous;
                }
            }
            assignments_[*local] = target;
            refresh();
        });
        connect(unmatch_, &QPushButton::clicked, this, [this] {
            if (const auto local = selected(files_); ready_ && local) {
                assignments_[*local].reset();
                refresh();
            }
        });
        connect(sort, &QPushButton::clicked, this, [this] {
            QCollator collator;
            if (collator.locale().language() == QLocale::C) {
                collator.setLocale(QLocale{QLocale::English});
            }
            collator.setNumericMode(true);
            collator.setCaseSensitivity(Qt::CaseInsensitive);
            QList<QTreeWidgetItem*> rows;
            while (files_->topLevelItemCount() > 0) {
                rows.push_back(files_->takeTopLevelItem(0));
            }
            std::stable_sort(rows.begin(), rows.end(), [&collator](const auto* a, const auto* b) {
                return collator.compare(a->text(0), b->text(0)) < 0;
            });
            files_->addTopLevelItems(rows);
            updateButtons();
        });
        connect(order_, &QPushButton::clicked, this, [this] {
            if (!ready_) {
                return;
            }
            std::fill(assignments_.begin(), assignments_.end(), std::nullopt);
            for (int row = 0; row < files_->topLevelItemCount() &&
                              static_cast<std::size_t>(row) < alignment_.release_tracks.size();
                 ++row) {
                assignments_[identity(files_->topLevelItem(row))] = static_cast<std::size_t>(row);
            }
            refresh();
        });
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
            assignments_.resize(local_tracks_.size());
            for (std::size_t local = 0; local < local_tracks_.size(); ++local) {
                const auto& match = alignment_.tracks[local];
                if (match.confidence >= 0.5) {
                    assignments_[local] = match.release_track_index;
                }
                auto* item = new QTreeWidgetItem(files_);
                item->setData(0, Qt::UserRole, static_cast<qulonglong>(local));
                const auto path = local < local_paths_.size() ? local_paths_[local] : QString{};
                auto name = QFileInfo(path).fileName();
                if (name.isEmpty()) {
                    name = text(local_tracks_[local].title);
                }
                if (name.isEmpty()) {
                    name = tr("File %1").arg(local + 1);
                }
                item->setText(0, name);
                item->setToolTip(0, path + QStringLiteral("\n") + text(local_tracks_[local].title));
                item->setText(1, duration(local_tracks_[local].duration_ms));
            }
            for (std::size_t target = 0; target < alignment_.release_tracks.size(); ++target) {
                auto* item = new QTreeWidgetItem(tracks_);
                item->setData(0, Qt::UserRole, static_cast<qulonglong>(target));
                item->setText(0, trackLabel(target));
                item->setText(1, duration(alignment_.release_tracks[target].track.length_ms));
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
    static std::size_t identity(QTreeWidgetItem* item) {
        return static_cast<std::size_t>(item->data(0, Qt::UserRole).toULongLong());
    }
    static std::optional<std::size_t> selected(QTreeWidget* tree) {
        return tree->selectedItems().isEmpty()
                   ? std::nullopt
                   : std::optional{identity(tree->selectedItems().front())};
    }
    QString trackLabel(const std::size_t target) const {
        const auto& track = alignment_.release_tracks[target];
        return tr("%1.%2 · %3")
            .arg(track.medium_position)
            .arg(track.track.position)
            .arg(text(track.track.title));
    }
    void updateButtons() {
        assign_->setEnabled(ready_ && selected(files_).has_value() &&
                            selected(tracks_).has_value());
        const auto local = selected(files_);
        unmatch_->setEnabled(ready_ && local && assignments_[*local].has_value());
        order_->setEnabled(ready_ && !alignment_.release_tracks.empty());
        stage_->setEnabled(ready_ && std::ranges::any_of(assignments_, [](const auto& value) {
                               return value.has_value();
                           }));
    }
    void refresh() {
        for (int row = 0; row < tracks_->topLevelItemCount(); ++row) {
            tracks_->topLevelItem(row)->setText(2, tr("Unassigned"));
        }
        std::size_t count = 0;
        for (int row = 0; row < files_->topLevelItemCount(); ++row) {
            auto* file = files_->topLevelItem(row);
            const auto target = assignments_[identity(file)];
            file->setText(2, target ? trackLabel(*target) : tr("Unmatched"));
            if (target) {
                tracks_->topLevelItem(static_cast<int>(*target))->setText(2, file->text(0));
                ++count;
            }
        }
        status_->setText(tr("%1 of %2 files matched · %3 release tracks unassigned. "
                            "Only matched files receive staged tags.")
                             .arg(count)
                             .arg(assignments_.size())
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
    QTreeWidget* files_;
    QTreeWidget* tracks_;
    QPushButton* assign_;
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
