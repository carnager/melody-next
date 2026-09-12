// SPDX-License-Identifier: GPL-3.0-only

#include "bench/musicbrainz_identify_dialog.hpp"
#include "bench/musicbrainz_track_match_widget.hpp"

#include "trackknife/musicbrainz/acoustid.hpp"
#include "trackknife/musicbrainz/proposal_bridge.hpp"
#include "trackknife/musicbrainz/web_service.hpp"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace trackknife::bench {
namespace {

[[nodiscard]] QString display_utf8(const std::string& text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

[[nodiscard]] QString version_text(const musicbrainz::Release& release) {
    QStringList parts;
    for (const auto& part : {release.date, release.country, release.disambiguation, release.label,
                             release.catalog_number}) {
        if (!part.empty()) {
            parts.push_back(display_utf8(part));
        }
    }
    return parts.join(QStringLiteral(" · "));
}

[[nodiscard]] QString media_text(const musicbrainz::Release& release) {
    QStringList formats;
    for (const auto& medium : release.media) {
        if (!medium.format.empty()) {
            formats.push_back(display_utf8(medium.format));
        }
    }
    formats.removeDuplicates();
    auto text = formats.join(QStringLiteral(" + "));
    if (release.media.size() > 1U) {
        text += QStringLiteral(" × %1").arg(release.media.size());
    }
    return text;
}

[[nodiscard]] QString credit_text(const std::vector<musicbrainz::ArtistCredit>& credits) {
    std::string joined;
    for (const auto& credit : credits) {
        joined += credit.name;
        joined += credit.join_phrase;
    }
    return display_utf8(joined);
}

class MusicBrainzIdentifyDialog final : public QDialog {
  public:
    MusicBrainzIdentifyDialog(MusicBrainzLookupService service,
                              std::vector<musicbrainz::LocalTrackDescriptor> local_tracks,
                              std::vector<QString> local_paths,
                              std::vector<std::size_t> item_indexes, const QString& initial_artist,
                              const QString& initial_release,
                              std::function<void(metadata::MetadataProposalSet)> accepted,
                              QWidget* parent)
        : QDialog(parent), service_(std::move(service)), local_tracks_(std::move(local_tracks)),
          local_paths_(std::move(local_paths)), item_indexes_(std::move(item_indexes)),
          accepted_(std::move(accepted)) {
        setObjectName(QStringLiteral("bench-musicbrainz-identify"));
        setWindowTitle(QStringLiteral("Identify with MusicBrainz"));
        setWindowModality(Qt::WindowModal);
        setAttribute(Qt::WA_DeleteOnClose);
        setMinimumSize(640, 400);
        resize(940, 520);

        auto* root = new QVBoxLayout(this);
        pages_ = new QStackedWidget(this);
        root->addWidget(pages_);
        auto* search_page = new QWidget(pages_);
        pages_->addWidget(search_page);
        auto* layout = new QVBoxLayout(search_page);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* form = new QFormLayout;
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        artist_ = new QLineEdit(initial_artist, this);
        artist_->setObjectName(QStringLiteral("bench-musicbrainz-identify-artist"));
        artist_->setPlaceholderText(QStringLiteral("Artist name (optional if you enter an album)"));
        form->addRow(QStringLiteral("Artist:"), artist_);
        release_ = new QLineEdit(initial_release, this);
        release_->setObjectName(QStringLiteral("bench-musicbrainz-identify-release"));
        release_->setPlaceholderText(
            QStringLiteral("Type an album title — existing tags are not required"));
        form->addRow(QStringLiteral("Album:"), release_);
        auto* form_row = new QHBoxLayout;
        form_row->addLayout(form, 1);
        search_ = new QPushButton(QStringLiteral("Search"), this);
        search_->setObjectName(QStringLiteral("bench-musicbrainz-identify-search"));
        search_->setDefault(true);
        form_row->addWidget(search_, 0, Qt::AlignBottom);
        scan_ = new QPushButton(QStringLiteral("Fingerprint files"), this);
        scan_->setObjectName(QStringLiteral("bench-musicbrainz-identify-scan"));
        scan_->setToolTip(
            QStringLiteral("Identify by audio fingerprint (AcoustID) — works with no usable "
                           "tags at all; candidates are ranked by how many selected files "
                           "match each release"));
        scan_->setEnabled(static_cast<bool>(service_.fingerprint) &&
                          static_cast<bool>(service_.acoustid_lookup) && !local_paths_.empty());
        form_row->addWidget(scan_, 0, Qt::AlignBottom);
        layout->addLayout(form_row);

        status_ = new QLabel(
            QStringLiteral("Searches MusicBrainz by text — no MusicBrainz tags are needed"), this);
        status_->setObjectName(QStringLiteral("bench-musicbrainz-identify-status"));
        status_->setWordWrap(true);
        layout->addWidget(status_);

        results_ = new QTreeWidget(this);
        results_->setObjectName(QStringLiteral("bench-musicbrainz-identify-results"));
        results_->setAccessibleName(QStringLiteral("MusicBrainz release versions"));
        results_->setColumnCount(6);
        results_->setHeaderLabels({QStringLiteral("Match"), QStringLiteral("Album"),
                                   QStringLiteral("Artist"), QStringLiteral("Tracks"),
                                   QStringLiteral("Media"), QStringLiteral("Version")});
        results_->setRootIsDecorated(false);
        results_->setAlternatingRowColors(true);
        results_->setUniformRowHeights(true);
        results_->setTextElideMode(Qt::ElideRight);
        results_->setSelectionMode(QAbstractItemView::SingleSelection);
        results_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        results_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        results_->header()->setStretchLastSection(true);
        layout->addWidget(results_, 1);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        buttons->setObjectName(QStringLiteral("bench-musicbrainz-identify-buttons"));
        use_ =
            buttons->addButton(QStringLiteral("Match this version…"), QDialogButtonBox::ActionRole);
        use_->setObjectName(QStringLiteral("bench-musicbrainz-identify-use"));
        use_->setToolTip(
            QStringLiteral("Load the release tracks and review their assignments to local files"));
        use_->setEnabled(false);
        layout->addWidget(buttons);

        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
        connect(search_, &QPushButton::clicked, this, [this] { startSearch(); });
        connect(scan_, &QPushButton::clicked, this, [this] { startScan(); });
        connect(results_, &QTreeWidget::itemSelectionChanged, this,
                [this] { use_->setEnabled(!busy_ && results_->currentItem() != nullptr); });
        connect(results_, &QTreeWidget::itemDoubleClicked, this,
                [this](QTreeWidgetItem*, int) { useSelected(); });
        connect(use_, &QPushButton::clicked, this, [this] { useSelected(); });
    }

  private:
    void setBusy(const bool busy) {
        busy_ = busy;
        search_->setEnabled(!busy);
        scan_->setEnabled(!busy && static_cast<bool>(service_.fingerprint) &&
                          static_cast<bool>(service_.acoustid_lookup) && !local_paths_.empty());
        use_->setEnabled(!busy && results_->currentItem() != nullptr);
    }

    // AcoustID identification (ADR-0096): fingerprint every selected file,
    // vote releases by how many files matched a recording on them, then load
    // the top candidates into the same version picker.
    void startScan() {
        if (busy_ || !service_.fingerprint || !service_.acoustid_lookup || local_paths_.empty()) {
            return;
        }
        setBusy(true);
        results_->clear();
        candidates_.clear();
        scan_votes_.clear();
        scan_matched_files_ = 0U;
        scanFile(0U);
    }

    void scanFile(const std::size_t position) {
        if (position >= local_paths_.size()) {
            finishScan();
            return;
        }
        status_->setText(QStringLiteral("Fingerprinting file %1 of %2…")
                             .arg(position + 1U)
                             .arg(local_paths_.size()));
        QPointer<MusicBrainzIdentifyDialog> self{this};
        service_.fingerprint(
            local_paths_[position],
            [self, position](core::Result<AcoustIdFingerprint> fingerprint) {
                if (self.isNull()) {
                    return;
                }
                if (!fingerprint) {
                    self->setBusy(false);
                    self->status_->setText(QStringLiteral("Fingerprinting failed · %1")
                                               .arg(display_utf8(fingerprint.error().message)));
                    return;
                }
                self->status_->setText(QStringLiteral("Looking up file %1 of %2…")
                                           .arg(position + 1U)
                                           .arg(self->local_paths_.size()));
                self->service_.acoustid_lookup(
                    *fingerprint, [self, position](core::Result<QByteArray> body) {
                        if (self.isNull()) {
                            return;
                        }
                        if (!body) {
                            self->setBusy(false);
                            self->status_->setText(QStringLiteral("AcoustID lookup failed · %1")
                                                       .arg(display_utf8(body.error().message)));
                            return;
                        }
                        const auto lookup = musicbrainz::parse_acoustid_lookup(std::string_view{
                            body->constData(), static_cast<std::size_t>(body->size())});
                        if (!lookup) {
                            self->setBusy(false);
                            self->status_->setText(QStringLiteral("AcoustID lookup failed · %1")
                                                       .arg(display_utf8(lookup.error().message)));
                            return;
                        }
                        auto matched = false;
                        for (const auto& result : lookup->results) {
                            if (result.score < 0.5) {
                                continue;
                            }
                            for (const auto& recording : result.recordings) {
                                for (const auto& release_id : recording.release_ids) {
                                    self->scan_votes_[release_id].insert(position);
                                    matched = true;
                                }
                            }
                        }
                        if (matched) {
                            ++self->scan_matched_files_;
                        }
                        self->scanFile(position + 1U);
                    });
            });
    }

    void finishScan() {
        if (scan_votes_.empty()) {
            setBusy(false);
            status_->setText(QStringLiteral(
                "No AcoustID matches — try the text search, or the files may be unsubmitted"));
            return;
        }
        std::vector<std::pair<std::string, std::size_t>> ranked;
        ranked.reserve(scan_votes_.size());
        for (const auto& [release_id, files] : scan_votes_) {
            ranked.emplace_back(release_id, files.size());
        }
        std::ranges::stable_sort(
            ranked, [](const auto& left, const auto& right) { return left.second > right.second; });
        constexpr std::size_t maximum_candidates = 5U;
        if (ranked.size() > maximum_candidates) {
            ranked.resize(maximum_candidates);
        }
        scan_candidates_ = std::move(ranked);
        loadScanCandidate(0U);
    }

    void loadScanCandidate(const std::size_t rank) {
        if (rank >= scan_candidates_.size()) {
            setBusy(false);
            if (candidates_.empty()) {
                status_->setText(QStringLiteral("No AcoustID candidate could be loaded"));
                return;
            }
            status_->setText(
                QStringLiteral(
                    "%1 fingerprint %2 · matched %3 of %4 files · every version is its own row")
                    .arg(candidates_.size())
                    .arg(candidates_.size() == 1U ? QStringLiteral("candidate")
                                                  : QStringLiteral("candidates"))
                    .arg(scan_matched_files_)
                    .arg(local_paths_.size()));
            results_->setCurrentItem(results_->topLevelItem(0));
            use_->setEnabled(results_->currentItem() != nullptr);
            return;
        }
        status_->setText(QStringLiteral("Loading candidate %1 of %2…")
                             .arg(rank + 1U)
                             .arg(scan_candidates_.size()));
        const auto url = musicbrainz::build_release_lookup_url(scan_candidates_[rank].first);
        if (!url) {
            loadScanCandidate(rank + 1U);
            return;
        }
        QPointer<MusicBrainzIdentifyDialog> self{this};
        service_.fetch(QString::fromStdString(*url), [self, rank](core::Result<QByteArray> body) {
            if (self.isNull()) {
                return;
            }
            if (body) {
                auto release = musicbrainz::parse_release_lookup(
                    std::string_view{body->constData(), static_cast<std::size_t>(body->size())});
                if (release) {
                    const auto matched = self->scan_candidates_[rank].second;
                    auto* item = new QTreeWidgetItem(self->results_);
                    item->setData(0, Qt::UserRole,
                                  static_cast<qulonglong>(self->candidates_.size()));
                    item->setText(
                        0,
                        QStringLiteral("%1/%2 files").arg(matched).arg(self->local_paths_.size()));
                    item->setText(1, display_utf8(release->title));
                    item->setText(2, credit_text(release->artist_credits));
                    item->setText(3, QString::number(release->track_count));
                    item->setText(4, media_text(*release));
                    item->setText(5, version_text(*release));
                    item->setToolTip(1, display_utf8(release->id));
                    self->candidates_.push_back(std::move(*release));
                }
            }
            self->loadScanCandidate(rank + 1U);
        });
    }

    void startSearch() {
        if (busy_ || !service_.fetch) {
            return;
        }
        const auto url = musicbrainz::build_release_search_url(musicbrainz::ReleaseSearchQuery{
            .artist = artist_->text().trimmed().toStdString(),
            .release = release_->text().trimmed().toStdString(),
            .track_count = std::nullopt,
            .limit = 25U,
        });
        if (!url) {
            status_->setText(QStringLiteral("Enter an artist or an album to search"));
            return;
        }
        setBusy(true);
        status_->setText(QStringLiteral("Searching MusicBrainz…"));
        results_->clear();
        candidates_.clear();
        QPointer<MusicBrainzIdentifyDialog> self{this};
        service_.fetch(QString::fromStdString(*url), [self](core::Result<QByteArray> body) {
            if (!self.isNull()) {
                self->finishSearch(std::move(body));
            }
        });
    }

    void finishSearch(core::Result<QByteArray> body) {
        setBusy(false);
        if (!body) {
            status_->setText(QStringLiteral("Search failed · %1")
                                 .arg(QString::fromStdString(body.error().message)));
            return;
        }
        auto parsed = musicbrainz::parse_release_search(
            std::string_view{body->constData(), static_cast<std::size_t>(body->size())});
        if (!parsed) {
            status_->setText(QStringLiteral("Search failed · %1")
                                 .arg(QString::fromStdString(parsed.error().message)));
            return;
        }
        const auto ranked = musicbrainz::rank_release_candidates(local_tracks_, *parsed);
        candidates_ = std::move(parsed->releases);
        for (const auto& entry : ranked) {
            const auto& release = candidates_[entry.release_index];
            auto* item = new QTreeWidgetItem(results_);
            item->setData(0, Qt::UserRole, static_cast<qulonglong>(entry.release_index));
            item->setText(0, release.track_count == local_tracks_.size()
                                 ? QStringLiteral("Same track count")
                                 : QStringLiteral("Different track count"));
            item->setToolTip(0, QStringLiteral("Search relevance: %1/100 · ranking score: %2")
                                    .arg(release.search_score)
                                    .arg(entry.score));
            item->setText(1, display_utf8(release.title));
            item->setText(2, credit_text(release.artist_credits));
            item->setText(3, QString::number(release.track_count));
            item->setText(4, media_text(release));
            item->setText(5, version_text(release));
            item->setToolTip(1, display_utf8(release.id));
            item->setToolTip(5, version_text(release));
        }
        if (candidates_.empty()) {
            status_->setText(QStringLiteral("No releases found — adjust the search text"));
        } else {
            status_->setText(
                QStringLiteral("%1 release %2 · every version of an album is its own row")
                    .arg(candidates_.size())
                    .arg(candidates_.size() == 1U ? QStringLiteral("version")
                                                  : QStringLiteral("versions")));
            results_->setCurrentItem(results_->topLevelItem(0));
        }
    }

    void useSelected() {
        if (busy_ || results_->currentItem() == nullptr || !service_.fetch) {
            return;
        }
        const auto index =
            static_cast<std::size_t>(results_->currentItem()->data(0, Qt::UserRole).toULongLong());
        if (index >= candidates_.size()) {
            return;
        }
        const auto url = musicbrainz::build_release_lookup_url(candidates_[index].id);
        if (!url) {
            status_->setText(QStringLiteral("This candidate has no usable release id"));
            return;
        }
        setBusy(true);
        status_->setText(QStringLiteral("Loading the release's track list…"));
        QPointer<MusicBrainzIdentifyDialog> self{this};
        service_.fetch(QString::fromStdString(*url), [self](core::Result<QByteArray> body) {
            if (!self.isNull()) {
                self->finishLookup(std::move(body));
            }
        });
    }

    void finishLookup(core::Result<QByteArray> body) {
        setBusy(false);
        if (!body) {
            status_->setText(QStringLiteral("Loading failed · %1")
                                 .arg(QString::fromStdString(body.error().message)));
            return;
        }
        auto release = musicbrainz::parse_release_lookup(
            std::string_view{body->constData(), static_cast<std::size_t>(body->size())});
        if (!release) {
            status_->setText(QStringLiteral("Loading failed · %1")
                                 .arg(QString::fromStdString(release.error().message)));
            return;
        }
        auto* review = createMusicBrainzTrackMatchWidget(
            std::move(*release), local_tracks_, local_paths_, item_indexes_,
            [this](metadata::MetadataProposalSet proposals) {
                if (accepted_) {
                    accepted_(std::move(proposals));
                }
                close();
            },
            [this] {
                auto* current = pages_->currentWidget();
                pages_->setCurrentIndex(0);
                pages_->removeWidget(current);
                current->deleteLater();
            },
            pages_);
        pages_->addWidget(review);
        pages_->setCurrentWidget(review);
    }

    MusicBrainzLookupService service_;
    std::vector<musicbrainz::LocalTrackDescriptor> local_tracks_;
    std::vector<QString> local_paths_;
    std::vector<std::size_t> item_indexes_;
    std::function<void(metadata::MetadataProposalSet)> accepted_;
    std::vector<musicbrainz::Release> candidates_;
    QLineEdit* artist_{nullptr};
    QLineEdit* release_{nullptr};
    QPushButton* search_{nullptr};
    QPushButton* scan_{nullptr};
    QPushButton* use_{nullptr};
    QLabel* status_{nullptr};
    QTreeWidget* results_{nullptr};
    QStackedWidget* pages_{nullptr};
    bool busy_{false};
    std::map<std::string, std::set<std::size_t>> scan_votes_;
    std::vector<std::pair<std::string, std::size_t>> scan_candidates_;
    std::size_t scan_matched_files_{0U};
};

} // namespace

QDialog* createMusicBrainzIdentifyDialog(
    MusicBrainzLookupService service, std::vector<musicbrainz::LocalTrackDescriptor> local_tracks,
    std::vector<QString> local_paths, std::vector<std::size_t> item_indexes,
    const QString initial_artist, const QString initial_release,
    std::function<void(metadata::MetadataProposalSet)> accepted, QWidget* parent) {
    return new MusicBrainzIdentifyDialog(
        std::move(service), std::move(local_tracks), std::move(local_paths),
        std::move(item_indexes), initial_artist, initial_release, std::move(accepted), parent);
}

} // namespace trackknife::bench
