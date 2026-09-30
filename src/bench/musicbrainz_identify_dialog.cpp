// SPDX-License-Identifier: GPL-3.0-only

#include "bench/musicbrainz_identify_dialog.hpp"
#include "bench/musicbrainz_track_match_widget.hpp"
#include "workspace/identify_session.hpp"

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

// A view over an IdentifySession: the search and its release versions, then
// the chosen version's matching.
class MusicBrainzIdentifyDialog final : public QDialog {
  public:
    MusicBrainzIdentifyDialog(IdentifySession* session,
                              std::function<void(metadata::MetadataProposalSet)> accepted,
                              QWidget* parent)
        : QDialog(parent), session_(session), accepted_(std::move(accepted)) {
        session_->setParent(this);
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
        artist_ = new QLineEdit(session_->initialArtist(), this);
        artist_->setObjectName(QStringLiteral("bench-musicbrainz-identify-artist"));
        artist_->setPlaceholderText(QStringLiteral("Artist name (optional if you enter an album)"));
        form->addRow(QStringLiteral("Artist:"), artist_);
        release_ = new QLineEdit(session_->initialRelease(), this);
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
        form_row->addWidget(scan_, 0, Qt::AlignBottom);
        layout->addLayout(form_row);

        status_ = new QLabel(this);
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
        connect(search_, &QPushButton::clicked, this,
                [this] { session_->search(artist_->text(), release_->text()); });
        connect(scan_, &QPushButton::clicked, session_, &IdentifySession::scan);
        connect(results_, &QTreeWidget::itemSelectionChanged, this,
                &MusicBrainzIdentifyDialog::sync);
        connect(results_, &QTreeWidget::itemDoubleClicked, this,
                [this](QTreeWidgetItem*, int) { useSelected(); });
        connect(use_, &QPushButton::clicked, this, [this] { useSelected(); });
        connect(session_, &IdentifySession::changed, this, &MusicBrainzIdentifyDialog::sync);
        connect(session_, &IdentifySession::matchOpened, this, [this](TrackMatchSession* match) {
            auto* review =
                createMusicBrainzTrackMatchView(match, [this] { session_->back(); }, pages_);
            pages_->addWidget(review);
            pages_->setCurrentWidget(review);
        });
        connect(session_, &IdentifySession::matchClosed, this, [this] {
            auto* current = pages_->currentWidget();
            pages_->setCurrentIndex(0);
            if (current != pages_->widget(0)) {
                pages_->removeWidget(current);
                current->deleteLater();
            }
        });
        connect(session_, &IdentifySession::accepted, this,
                [this](metadata::MetadataProposalSet proposals) {
                    if (accepted_) {
                        accepted_(std::move(proposals));
                    }
                    close();
                });
        sync();
    }

  private:
    void useSelected() {
        if (auto* current = results_->currentItem()) {
            session_->use(results_->indexOfTopLevelItem(current));
        }
    }

    void sync() {
        const auto& rows = session_->candidates();
        // Rows come a search at a time, or one candidate after another.
        if (results_->topLevelItemCount() > static_cast<int>(rows.size())) {
            results_->clear();
        }
        for (auto row = static_cast<std::size_t>(results_->topLevelItemCount()); row < rows.size();
             ++row) {
            const auto& candidate = rows[row];
            auto* item = new QTreeWidgetItem(results_);
            item->setText(0, candidate.match);
            item->setToolTip(0, candidate.match_tool_tip);
            item->setText(1, candidate.album);
            item->setToolTip(1, candidate.album_tool_tip);
            item->setText(2, candidate.artist);
            item->setText(3, candidate.tracks);
            item->setText(4, candidate.media);
            item->setText(5, candidate.version);
            item->setToolTip(5, candidate.version);
        }
        if (results_->currentItem() == nullptr && !session_->busy() && session_->suggested() >= 0 &&
            session_->suggested() < results_->topLevelItemCount()) {
            results_->setCurrentItem(results_->topLevelItem(session_->suggested()));
        }
        status_->setText(session_->status());
        search_->setEnabled(session_->canSearch());
        scan_->setEnabled(session_->canScan());
        use_->setEnabled(!session_->busy() && results_->currentItem() != nullptr);
    }

    IdentifySession* session_;
    std::function<void(metadata::MetadataProposalSet)> accepted_;
    QLineEdit* artist_{nullptr};
    QLineEdit* release_{nullptr};
    QPushButton* search_{nullptr};
    QPushButton* scan_{nullptr};
    QPushButton* use_{nullptr};
    QLabel* status_{nullptr};
    QTreeWidget* results_{nullptr};
    QStackedWidget* pages_{nullptr};
};

} // namespace

QDialog* createMusicBrainzIdentifyDialog(
    MusicBrainzLookupService service, std::vector<musicbrainz::LocalTrackDescriptor> local_tracks,
    std::vector<QString> local_paths, std::vector<std::size_t> item_indexes,
    const QString initial_artist, const QString initial_release,
    std::function<void(metadata::MetadataProposalSet)> accepted, QWidget* parent) {
    return new MusicBrainzIdentifyDialog(
        new IdentifySession(std::move(service), std::move(local_tracks), std::move(local_paths),
                            std::move(item_indexes), initial_artist, initial_release),
        std::move(accepted), parent);
}

} // namespace trackknife::bench
