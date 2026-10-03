// SPDX-License-Identifier: GPL-3.0-only
#include "bench/identify_albums_dialog.hpp"

#include "bench/musicbrainz_track_match_widget.hpp"
#include "trackknife/musicbrainz/web_service.hpp"
#include "workspace/album_batch_session.hpp"
#include "workspace/identify_session.hpp"
#include "workspace/tagger_session.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace trackknife::bench {
namespace {

using State = AlbumBatchSession::State;
using Album = AlbumBatchSession::Album;

enum Filter : int { all, needs_you, matched, no_match, waiting };

[[nodiscard]] bool in_filter(const Album& album, const int filter) {
    switch (filter) {
    case needs_you:
        return album.state == State::needs_choice || album.state == State::failed;
    case matched:
        return album.state == State::staging || album.state == State::staged ||
               album.state == State::written;
    case no_match:
        return album.state == State::no_match;
    case waiting:
        return album.state == State::waiting || album.state == State::searching;
    default:
        return true;
    }
}

[[nodiscard]] QString basis_text(const Album& album) {
    switch (album.basis) {
    case musicbrainz::AlbumGroupBasis::release_id:
        return QStringLiteral("release id");
    case musicbrainz::AlbumGroupBasis::tags:
        return QStringLiteral("tags");
    case musicbrainz::AlbumGroupBasis::folder:
        return QStringLiteral("folder");
    }
    return {};
}

[[nodiscard]] QString name_of(const Album& album) {
    return album.artist.isEmpty() ? album.title : album.artist + QStringLiteral(" — ") + album.title;
}

[[nodiscard]] QString detail_of(const Album& album) {
    QStringList parts;
    if (!album.year.isEmpty()) {
        parts << album.year;
    }
    parts << QStringLiteral("%1 %2")
                 .arg(album.items.size())
                 .arg(album.items.size() == 1U ? QStringLiteral("file") : QStringLiteral("files"));
    if (album.folders.size() > 1U) {
        parts << QStringLiteral("%1 folders").arg(album.folders.size());
    }
    return parts.join(QStringLiteral(" · "));
}

[[nodiscard]] QString escaped(const QString& text) { return text.toHtmlEscaped(); }

// A release as one line: what tells its versions apart.
[[nodiscard]] QStringList release_parts(const musicbrainz::Release& release) {
    QStringList parts;
    for (const auto& part : {release.date, release.country, release.label}) {
        if (!part.empty()) {
            parts << QString::fromStdString(part);
        }
    }
    if (!release.media.empty() && !release.media.front().format.empty()) {
        parts << QStringLiteral("%1×%2")
                     .arg(release.media.size())
                     .arg(QString::fromStdString(release.media.front().format));
    }
    return parts;
}

[[nodiscard]] QString files_text(const std::size_t count) {
    return QStringLiteral("%1 %2").arg(count).arg(count == 1U ? QStringLiteral("file")
                                                              : QStringLiteral("files"));
}

[[nodiscard]] QString check(const bool ok, const QString& text) {
    return QStringLiteral("<div>%1 %2</div>")
        .arg(ok ? QStringLiteral("✓") : QStringLiteral("✗"), escaped(text));
}

} // namespace

IdentifyAlbumsDialog::IdentifyAlbumsDialog(TaggerSession& tagger, MusicBrainzLookupService service,
                                           QWidget* parent)
    : QDialog(parent), tagger_(&tagger),
      session_(new AlbumBatchSession(tagger, std::move(service), this)) {
    setObjectName(QStringLiteral("bench-identify-albums"));
    setWindowTitle(QStringLiteral("Identify albums"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1100, 700);
    auto* layout = new QVBoxLayout(this);

    heading_ = new QLabel(this);
    heading_->setObjectName(QStringLiteral("bench-identify-albums-heading"));
    layout->addWidget(heading_);

    auto* progress_row = new QHBoxLayout;
    progress_ = new QProgressBar(this);
    progress_->setObjectName(QStringLiteral("bench-identify-albums-progress"));
    progress_->setTextVisible(false);
    progress_->setMaximumWidth(280);
    progress_text_ = new QLabel(this);
    progress_text_->setObjectName(QStringLiteral("bench-identify-albums-progress-text"));
    stop_ = new QPushButton(QStringLiteral("Stop"), this);
    stop_->setObjectName(QStringLiteral("bench-identify-albums-stop"));
    progress_row->addWidget(progress_);
    progress_row->addWidget(progress_text_, 1);
    progress_row->addWidget(stop_);
    layout->addLayout(progress_row);

    filters_ = new QWidget(this);
    auto* filter_row = new QHBoxLayout(filters_);
    filter_row->setContentsMargins(0, 0, 0, 0);
    filter_group_ = new QButtonGroup(this);
    filter_group_->setExclusive(true);
    for (const auto& [id, name] : {std::pair{all, "All"}, std::pair{needs_you, "Needs you"},
                                   std::pair{matched, "Matched"}, std::pair{no_match, "No match"},
                                   std::pair{waiting, "Waiting"}}) {
        auto* button = new QPushButton(filters_);
        button->setCheckable(true);
        button->setObjectName(QStringLiteral("bench-identify-albums-filter-%1").arg(id));
        button->setProperty("label", QString::fromLatin1(name));
        filter_group_->addButton(button, id);
        filter_row->addWidget(button);
    }
    filter_group_->button(all)->setChecked(true);
    filter_row->addStretch(1);
    layout->addWidget(filters_);

    pages_ = new QStackedWidget(this);
    auto* splitter = new QSplitter(pages_);
    list_ = new QTreeWidget(splitter);
    list_->setObjectName(QStringLiteral("bench-identify-albums-list"));
    list_->setColumnCount(3);
    list_->setHeaderLabels({QStringLiteral("Album"), QStringLiteral("Files"),
                            QStringLiteral("State")});
    list_->setRootIsDecorated(false);
    list_->setUniformRowHeights(true);
    list_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    auto* side = new QWidget(splitter);
    auto* side_layout = new QVBoxLayout(side);
    side_layout->setContentsMargins(0, 0, 0, 0);
    detail_ = new QTextBrowser(side);
    detail_->setObjectName(QStringLiteral("bench-identify-albums-detail"));
    side_layout->addWidget(detail_, 1);
    auto* group_row = new QHBoxLayout;
    split_ = new QPushButton(QStringLiteral("Split by folder"), side);
    split_->setObjectName(QStringLiteral("bench-identify-albums-split"));
    merge_ = new QPushButton(QStringLiteral("Merge with…"), side);
    merge_->setObjectName(QStringLiteral("bench-identify-albums-merge"));
    group_row->addWidget(split_);
    group_row->addWidget(merge_);
    group_row->addStretch(1);
    side_layout->addLayout(group_row);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    pages_->addWidget(splitter);

    // Step 3: one album needing a person -- its versions above, the matcher
    // below. ↑/↓ in the versions choose one; Tab reaches the files.
    auto* review_page = new QWidget(pages_);
    auto* review_layout = new QVBoxLayout(review_page);
    review_layout->setContentsMargins(0, 0, 0, 0);
    review_heading_ = new QLabel(review_page);
    review_heading_->setObjectName(QStringLiteral("bench-identify-albums-review-heading"));
    review_layout->addWidget(review_heading_);
    versions_ = new QListWidget(review_page);
    versions_->setObjectName(QStringLiteral("bench-identify-albums-versions"));
    versions_->setMaximumHeight(120);
    review_layout->addWidget(versions_);
    match_holder_ = new QWidget(review_page);
    auto* holder_layout = new QVBoxLayout(match_holder_);
    holder_layout->setContentsMargins(0, 0, 0, 0);
    review_layout->addWidget(match_holder_, 1);
    auto* review_keys = new QHBoxLayout;
    auto* keys = new QLabel(
        QStringLiteral("Enter accept and next · S skip · ↑/↓ another version · Tab to the "
                       "files · Alt+↑/↓ move a file · U leave a file unmatched"),
        review_page);
    keys->setObjectName(QStringLiteral("bench-identify-albums-review-keys"));
    auto* back = new QPushButton(QStringLiteral("Back to the list"), review_page);
    back->setObjectName(QStringLiteral("bench-identify-albums-review-back"));
    auto* skip_button = new QPushButton(QStringLiteral("Skip"), review_page);
    skip_button->setObjectName(QStringLiteral("bench-identify-albums-review-skip"));
    auto* accept_button = new QPushButton(QStringLiteral("Accept and next"), review_page);
    accept_button->setObjectName(QStringLiteral("bench-identify-albums-review-accept"));
    review_keys->addWidget(keys, 1);
    review_keys->addWidget(back);
    review_keys->addWidget(skip_button);
    review_keys->addWidget(accept_button);
    review_layout->addLayout(review_keys);
    pages_->addWidget(review_page);
    for (const auto& key : {QKeySequence{Qt::Key_Return}, QKeySequence{Qt::Key_Enter}}) {
        auto* accept_key = new QShortcut(key, review_page);
        accept_key->setContext(Qt::WidgetWithChildrenShortcut);
        connect(accept_key, &QShortcut::activated, this, &IdentifyAlbumsDialog::accept);
    }
    auto* skip_key = new QShortcut(QKeySequence{Qt::Key_S}, review_page);
    skip_key->setContext(Qt::WidgetWithChildrenShortcut);
    connect(skip_key, &QShortcut::activated, this, &IdentifyAlbumsDialog::skip);
    connect(accept_button, &QPushButton::clicked, this, &IdentifyAlbumsDialog::accept);
    connect(skip_button, &QPushButton::clicked, this, &IdentifyAlbumsDialog::skip);
    connect(back, &QPushButton::clicked, this, &IdentifyAlbumsDialog::backToList);
    connect(versions_, &QListWidget::currentRowChanged, this, &IdentifyAlbumsDialog::showVersion);

    // Step 4: what is staged, written in one go -- with each album's own
    // checkbox, and renaming and moving each chosen on its own.
    auto* apply_page = new QWidget(pages_);
    auto* apply_layout = new QVBoxLayout(apply_page);
    apply_layout->setContentsMargins(0, 0, 0, 0);
    apply_heading_ = new QLabel(apply_page);
    apply_heading_->setObjectName(QStringLiteral("bench-identify-albums-apply-heading"));
    apply_heading_->setWordWrap(true);
    apply_layout->addWidget(apply_heading_);
    apply_list_ = new QTreeWidget(apply_page);
    apply_list_->setObjectName(QStringLiteral("bench-identify-albums-apply-list"));
    apply_list_->setColumnCount(4);
    apply_list_->setHeaderLabels({QStringLiteral("Album"), QStringLiteral("Release"),
                                  QStringLiteral("Changes"), QStringLiteral("State")});
    apply_list_->setRootIsDecorated(false);
    apply_list_->setUniformRowHeights(true);
    apply_list_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    apply_layout->addWidget(apply_list_, 1);
    auto* options_row = new QHBoxLayout;
    auto* options_label =
        new QLabel(QStringLiteral("Also, in the same write — each chosen on its own"), apply_page);
    auto* presets = new QPushButton(QStringLiteral("Edit naming presets…"), apply_page);
    presets->setObjectName(QStringLiteral("bench-identify-albums-apply-presets"));
    presets->setFlat(true);
    options_row->addWidget(options_label, 1);
    options_row->addWidget(presets);
    apply_layout->addLayout(options_row);
    auto* options = new QGridLayout;
    rename_ = new QCheckBox(QStringLiteral("Rename the files"), apply_page);
    rename_->setObjectName(QStringLiteral("bench-identify-albums-apply-rename"));
    rename_preset_ = new QComboBox(apply_page);
    rename_preset_->setObjectName(QStringLiteral("bench-identify-albums-apply-rename-preset"));
    rename_preset_->setAccessibleName(QStringLiteral("Naming preset for renaming"));
    rename_pattern_ = new QLabel(apply_page);
    rename_pattern_->setObjectName(QStringLiteral("bench-identify-albums-apply-rename-pattern"));
    move_ = new QCheckBox(QStringLiteral("Move into folders"), apply_page);
    move_->setObjectName(QStringLiteral("bench-identify-albums-apply-move"));
    move_preset_ = new QComboBox(apply_page);
    move_preset_->setObjectName(QStringLiteral("bench-identify-albums-apply-move-preset"));
    move_preset_->setAccessibleName(QStringLiteral("Naming preset for moving"));
    move_pattern_ = new QLabel(apply_page);
    move_pattern_->setObjectName(QStringLiteral("bench-identify-albums-apply-move-pattern"));
    for (auto* pattern : {rename_pattern_, move_pattern_}) {
        pattern->setTextInteractionFlags(Qt::TextSelectableByMouse);
        QFont mono = pattern->font();
        mono.setFamily(QStringLiteral("monospace"));
        mono.setStyleHint(QFont::Monospace);
        pattern->setFont(mono);
    }
    options->addWidget(rename_, 0, 0);
    options->addWidget(rename_preset_, 0, 1);
    options->addWidget(rename_pattern_, 0, 2);
    options->addWidget(new QLabel(QStringLiteral("in the folder they are in"), apply_page), 0, 3);
    options->addWidget(move_, 1, 0);
    options->addWidget(move_preset_, 1, 1);
    options->addWidget(move_pattern_, 1, 2);
    options->addWidget(new QLabel(QStringLiteral("under the library folder they are in"), apply_page),
                       1, 3);
    // A second step, once the tags are written.
    replaygain_ = new QCheckBox(QStringLiteral("Scan ReplayGain"), apply_page);
    replaygain_->setObjectName(QStringLiteral("bench-identify-albums-apply-replaygain"));
    replaygain_mode_ = new QComboBox(apply_page);
    replaygain_mode_->setObjectName(QStringLiteral("bench-identify-albums-apply-replaygain-mode"));
    replaygain_mode_->setAccessibleName(QStringLiteral("What to scan"));
    replaygain_mode_->addItems({QStringLiteral("Track and album gain"),
                                QStringLiteral("Track gain only")});
    replaygain_skip_ = new QCheckBox(QStringLiteral("Skip albums that already have gain"), apply_page);
    replaygain_skip_->setObjectName(QStringLiteral("bench-identify-albums-apply-replaygain-skip"));
    replaygain_skip_->setChecked(true);
    replaygain_hint_ = new QLabel(apply_page);
    replaygain_hint_->setObjectName(QStringLiteral("bench-identify-albums-apply-replaygain-hint"));
    auto* replaygain_row = new QHBoxLayout;
    replaygain_row->addWidget(replaygain_skip_);
    replaygain_row->addWidget(replaygain_hint_, 1);
    options->addWidget(replaygain_, 2, 0);
    options->addWidget(replaygain_mode_, 2, 1);
    options->addLayout(replaygain_row, 2, 2, 1, 2);
    options->setColumnStretch(2, 1);
    apply_layout->addLayout(options);
    auto* write_row = new QHBoxLayout;
    apply_note_ = new QLabel(apply_page);
    apply_note_->setObjectName(QStringLiteral("bench-identify-albums-apply-note"));
    apply_note_->setWordWrap(true);
    write_progress_ = new QProgressBar(apply_page);
    write_progress_->setObjectName(QStringLiteral("bench-identify-albums-write-progress"));
    write_progress_->setMaximumWidth(220);
    write_stop_ = new QPushButton(QStringLiteral("Stop"), apply_page);
    write_stop_->setObjectName(QStringLiteral("bench-identify-albums-write-stop"));
    apply_back_ = new QPushButton(QStringLiteral("Back to the list"), apply_page);
    apply_back_->setObjectName(QStringLiteral("bench-identify-albums-apply-back"));
    write_ = new QPushButton(apply_page);
    write_->setObjectName(QStringLiteral("bench-identify-albums-write"));
    write_row->addWidget(apply_note_, 1);
    write_row->addWidget(write_progress_);
    write_row->addWidget(write_stop_);
    write_row->addWidget(apply_back_);
    write_row->addWidget(write_);
    apply_layout->addLayout(write_row);
    pages_->addWidget(apply_page);
    const auto fill_presets = [this] {
        if (tagger_ == nullptr) {
            return;
        }
        for (auto* combo : {rename_preset_, move_preset_}) {
            const QSignalBlocker blocker{combo};
            const auto kept = combo->currentIndex();
            combo->clear();
            for (const auto& choice : tagger_->layouts()) {
                combo->addItem(choice.name, choice.id);
            }
            combo->setCurrentIndex(kept >= 0 && kept < combo->count() ? kept
                                                                      : tagger_->layoutIndex());
        }
        syncApply();
    };
    fill_presets();
    connect(&tagger, &TaggerSession::outputProfilesChanged, this, fill_presets);
    connect(presets, &QPushButton::clicked, this, [this] {
        if (tagger_ != nullptr) {
            emit tagger_->openSettingsRequested(TaggerSession::SettingsPage::naming);
        }
    });
    for (auto* box : {rename_, move_, replaygain_, replaygain_skip_}) {
        connect(box, &QCheckBox::toggled, this, &IdentifyAlbumsDialog::syncApply);
    }
    for (auto* combo : {rename_preset_, move_preset_, replaygain_mode_}) {
        connect(combo, &QComboBox::currentIndexChanged, this, &IdentifyAlbumsDialog::syncApply);
    }
    connect(apply_list_, &QTreeWidget::itemChanged, this, [this] {
        if (!syncing_) {
            syncApply();
        }
    });
    connect(apply_back_, &QPushButton::clicked, this, &IdentifyAlbumsDialog::backToList);
    connect(write_stop_, &QPushButton::clicked, session_, &AlbumBatchSession::stopWriting);
    connect(write_, &QPushButton::clicked, this, [this] {
        if (written_.empty()) {
            write();
        } else {
            backToList();
        }
    });
    connect(session_, &AlbumBatchSession::writeFinished, this, &IdentifyAlbumsDialog::syncApply);
    layout->addWidget(pages_, 1);

    bottom_bar_ = new QWidget(this);
    auto* bottom = new QHBoxLayout(bottom_bar_);
    bottom->setContentsMargins(0, 0, 0, 0);
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("bench-identify-albums-summary"));
    auto* close = new QPushButton(QStringLiteral("Close"), this);
    look_up_ = new QPushButton(this);
    look_up_->setObjectName(QStringLiteral("bench-identify-albums-look-up"));
    look_up_->setDefault(true);
    review_next_ = new QPushButton(this);
    review_next_->setObjectName(QStringLiteral("bench-identify-albums-review-next"));
    apply_ = new QPushButton(this);
    apply_->setObjectName(QStringLiteral("bench-identify-albums-apply"));
    bottom->addWidget(summary_, 1);
    bottom->addWidget(close);
    bottom->addWidget(look_up_);
    bottom->addWidget(review_next_);
    bottom->addWidget(apply_);
    connect(apply_, &QPushButton::clicked, this, &IdentifyAlbumsDialog::showApply);
    layout->addWidget(bottom_bar_);
    connect(review_next_, &QPushButton::clicked, this, &IdentifyAlbumsDialog::reviewNext);
    connect(list_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        const auto album = static_cast<std::size_t>(item->data(0, Qt::UserRole).toInt());
        if (session_->albums()[album].state == State::needs_choice) {
            review(album);
        }
    });

    connect(session_, &AlbumBatchSession::changed, this, &IdentifyAlbumsDialog::sync);
    connect(filter_group_, &QButtonGroup::idClicked, this, &IdentifyAlbumsDialog::sync);
    connect(list_, &QTreeWidget::currentItemChanged, this, &IdentifyAlbumsDialog::showDetail);
    connect(list_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, const int column) {
        if (syncing_ || column != 0) {
            return;
        }
        // Later: the list is rebuilt when it changes, and this item with it.
        const auto album = static_cast<std::size_t>(item->data(0, Qt::UserRole).toInt());
        const auto included = item->checkState(0) == Qt::Checked;
        QMetaObject::invokeMethod(
            this, [this, album, included] { session_->setIncluded(album, included); },
            Qt::QueuedConnection);
    });
    connect(split_, &QPushButton::clicked, this, [this] {
        if (const auto album = selectedAlbum(); album >= 0) {
            session_->splitByFolder(static_cast<std::size_t>(album));
        }
    });
    connect(merge_, &QPushButton::clicked, this, [this] {
        const auto album = selectedAlbum();
        if (album < 0) {
            return;
        }
        QMenu menu(this);
        const auto& albums = session_->albums();
        for (int other = 0; other < static_cast<int>(albums.size()); ++other) {
            if (other == album) {
                continue;
            }
            auto* action = menu.addAction(name_of(albums[static_cast<std::size_t>(other)]));
            connect(action, &QAction::triggered, this, [this, album, other] {
                session_->merge(static_cast<std::size_t>(album), static_cast<std::size_t>(other));
            });
        }
        menu.exec(merge_->mapToGlobal(QPoint{0, merge_->height()}));
    });
    connect(look_up_, &QPushButton::clicked, session_, &AlbumBatchSession::lookUp);
    connect(stop_, &QPushButton::clicked, session_, &AlbumBatchSession::stop);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    sync();
    if (list_->topLevelItemCount() > 0) {
        list_->setCurrentItem(list_->topLevelItem(0));
    }
}

int IdentifyAlbumsDialog::selectedAlbum() const {
    const auto* item = list_->currentItem();
    return item == nullptr ? -1 : item->data(0, Qt::UserRole).toInt();
}

bool IdentifyAlbumsDialog::shown(const int album) const {
    return in_filter(session_->albums()[static_cast<std::size_t>(album)], filter_group_->checkedId());
}

void IdentifyAlbumsDialog::sync() {
    const auto& albums = session_->albums();
    const auto started = session_->started();
    const auto included =
        albums.size() - session_->count(State::left_out);
    heading_->setText(QStringLiteral("%1 files grouped into %2 albums")
                          .arg(session_->fileCount())
                          .arg(albums.size()));

    // Step 2's progress: the albums answered of those asked about.
    const auto open = session_->count(State::waiting) + session_->count(State::searching);
    const auto answered = included - open;
    progress_->setVisible(started);
    progress_text_->setVisible(started);
    stop_->setVisible(started);
    stop_->setEnabled(session_->lookingUp());
    filters_->setVisible(started);
    progress_->setMaximum(static_cast<int>(std::max<std::size_t>(included, 1U)));
    progress_->setValue(static_cast<int>(answered));
    const auto seconds =
        session_->requestsLeft() * static_cast<std::size_t>(musicbrainz::minimum_request_interval_ms) /
        1000U;
    progress_text_->setText(
        session_->lookingUp()
            ? QStringLiteral("Looking up · %1 of %2 · MusicBrainz answers one request a second · "
                             "about %3 left · you can review while it runs")
                  .arg(answered)
                  .arg(included)
                  .arg(seconds >= 120U ? QStringLiteral("%1 min").arg(seconds / 60U)
                                       : QStringLiteral("%1 s").arg(seconds))
            : QStringLiteral("Looked up · %1 of %2").arg(answered).arg(included));
    const std::pair<int, std::size_t> counts[]{
        {all, albums.size()},
        {needs_you, session_->count(State::needs_choice) + session_->count(State::failed)},
        {matched, session_->count(State::staging) + session_->count(State::staged) +
                      session_->count(State::written)},
        {no_match, session_->count(State::no_match)},
        {waiting, open}};
    for (const auto& [id, count] : counts) {
        auto* button = filter_group_->button(id);
        button->setText(QStringLiteral("%1 %2").arg(button->property("label").toString()).arg(count));
    }

    // The list, keeping the album looked at.
    const auto current = selectedAlbum();
    syncing_ = true;
    {
        const QSignalBlocker blocker{list_};
        list_->clear();
        for (int index = 0; index < static_cast<int>(albums.size()); ++index) {
            if (!shown(index)) {
                continue;
            }
            const auto& album = albums[static_cast<std::size_t>(index)];
            auto* item = new QTreeWidgetItem(list_);
            item->setText(0, name_of(album));
            item->setText(1, detail_of(album));
            item->setText(2, started ? AlbumBatchSession::stateText(album) : basis_text(album));
            item->setData(0, Qt::UserRole, index);
            if (!started) {
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(0, album.state == State::left_out ? Qt::Unchecked : Qt::Checked);
            }
            if (index == current) {
                list_->setCurrentItem(item);
            }
        }
    }
    syncing_ = false;
    list_->setHeaderLabels({QStringLiteral("Album"), QStringLiteral("Files"),
                            started ? QStringLiteral("State") : QStringLiteral("Grouped by")});
    split_->setVisible(!started);
    merge_->setVisible(!started);
    const auto needing = session_->count(State::needs_choice);
    review_next_->setVisible(started);
    review_next_->setEnabled(session_->nextNeedingYou(std::nullopt).has_value());
    review_next_->setText(QStringLiteral("Review next needing you (%1)").arg(needing));
    review_next_->setDefault(started && needing > 0U);
    const auto staged = session_->count(State::staged);
    apply_->setVisible(started);
    apply_->setEnabled(staged > 0U);
    apply_->setText(QStringLiteral("Apply %1 staged…").arg(staged));
    apply_->setDefault(started && needing == 0U && staged > 0U);
    look_up_->setVisible(!started || !session_->lookingUp());
    look_up_->setText(started ? QStringLiteral("Look up again")
                              : QStringLiteral("Look up %1 albums").arg(included));
    look_up_->setEnabled(!started ? included > 0
                                  : session_->count(State::failed) + session_->count(State::waiting) > 0);
    summary_->setText(
        started ? QStringLiteral("%1 staged · %2 need you · %3 no match · %4 waiting")
                      .arg(session_->count(State::staged))
                      .arg(session_->count(State::needs_choice) + session_->count(State::failed))
                      .arg(session_->count(State::no_match))
                      .arg(open)
                : QStringLiteral("%1 albums · %2 files · grouped by release id, then tags, "
                                 "then folder")
                      .arg(included)
                      .arg(session_->fileCount()));
    showDetail();
    if (pages_->currentIndex() == 2) {
        syncApply();
    }
}

void IdentifyAlbumsDialog::review(const std::size_t album) {
    const auto& albums = session_->albums();
    if (album >= albums.size() || !albums[album].result ||
        albums[album].result->candidates.empty()) {
        return;
    }
    reviewing_ = album;
    const auto& entry = albums[album];
    std::size_t place = 1U;
    for (std::size_t index = 0; index < album; ++index) {
        place += albums[index].state == State::needs_choice ? 1U : 0U;
    }
    review_heading_->setText(QStringLiteral("<b>%1</b> · %2 · %3 of %4 needing you")
                                 .arg(escaped(name_of(entry)), escaped(detail_of(entry)))
                                 .arg(place)
                                 .arg(session_->count(State::needs_choice)));
    {
        const QSignalBlocker blocker{versions_};
        versions_->clear();
        for (const auto& candidate : entry.result->candidates) {
            const auto& release = candidate.release;
            QStringList parts{QString::fromStdString(release.title)};
            parts << release_parts(release);
            parts << QStringLiteral("%1 tracks").arg(candidate.alignment.release_tracks.size());
            parts << QStringLiteral("%1%").arg(qRound(candidate.alignment.confidence * 100.0));
            versions_->addItem(parts.join(QStringLiteral(" · ")));
        }
    }
    pages_->setCurrentIndex(1);
    filters_->hide();
    bottom_bar_->hide();
    versions_->setCurrentRow(0);
    showVersion(0);
    versions_->setFocus();
}

void IdentifyAlbumsDialog::reviewNext() {
    if (const auto next = session_->nextNeedingYou(reviewing_)) {
        review(*next);
    } else {
        backToList();
    }
}

void IdentifyAlbumsDialog::showVersion(const int version) {
    if (!reviewing_ || version < 0) {
        return;
    }
    const auto& album = session_->albums()[*reviewing_];
    if (static_cast<std::size_t>(version) >= album.result->candidates.size()) {
        return;
    }
    // Later, not now: this may run inside the old matcher's own signal.
    if (match_view_ != nullptr) {
        match_view_->hide();
        match_view_->deleteLater();
        match_view_ = nullptr;
    }
    if (match_ != nullptr) {
        match_->disconnect(this);
        match_->deleteLater();
        match_ = nullptr;
    }
    auto files = session_->filesOf(*reviewing_);
    match_ = new TrackMatchSession(
        album.result->candidates[static_cast<std::size_t>(version)].release,
        std::move(files.descriptors), std::move(files.paths), std::move(files.items), this);
    connect(match_, &TrackMatchSession::accepted, this,
            [this](metadata::MetadataProposalSet proposals) {
                if (reviewing_) {
                    session_->choose(*reviewing_,
                                     static_cast<std::size_t>(std::max(0, versions_->currentRow())),
                                     std::move(proposals));
                    reviewNext();
                }
            });
    match_view_ = createMusicBrainzTrackMatchView(match_, [this] { backToList(); }, match_holder_);
    match_holder_->layout()->addWidget(match_view_);
}

void IdentifyAlbumsDialog::accept() {
    if (match_ != nullptr && match_->canStage()) {
        match_->stage();
    }
}

void IdentifyAlbumsDialog::skip() {
    if (reviewing_) {
        session_->skip(*reviewing_);
        reviewNext();
    }
}

void IdentifyAlbumsDialog::backToList() {
    if (session_->writing() != nullptr || session_->scanning() != nullptr) {
        return;
    }
    reviewing_.reset();
    written_.clear();
    pages_->setCurrentIndex(0);
    filters_->setVisible(session_->started());
    bottom_bar_->show();
    sync();
}

void IdentifyAlbumsDialog::showApply() {
    written_.clear();
    const auto& albums = session_->albums();
    const auto changes = session_->changes();
    syncing_ = true;
    {
        const QSignalBlocker blocker{apply_list_};
        apply_list_->clear();
        for (std::size_t index = 0; index < albums.size(); ++index) {
            const auto& album = albums[index];
            if (album.state != State::staged) {
                continue;
            }
            auto* item = new QTreeWidgetItem(apply_list_);
            item->setText(0, name_of(album));
            if (album.result && album.version < album.result->candidates.size()) {
                item->setText(1, release_parts(album.result->candidates[album.version].release)
                                     .join(QStringLiteral(" · ")));
            }
            item->setText(2, QStringLiteral("%1 · %2 %3")
                                 .arg(files_text(album.items.size()))
                                 .arg(changes[index])
                                 .arg(changes[index] == 1U ? QStringLiteral("change")
                                                           : QStringLiteral("changes")));
            item->setText(3, QStringLiteral("Ready"));
            item->setData(0, Qt::UserRole, static_cast<int>(index));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(0, Qt::Checked);
        }
    }
    syncing_ = false;
    reviewing_.reset();
    pages_->setCurrentIndex(2);
    filters_->hide();
    bottom_bar_->hide();
    syncApply();
    write_->setFocus();
}

std::vector<std::size_t> IdentifyAlbumsDialog::albumsToWrite() const {
    std::vector<std::size_t> albums;
    for (int row = 0; row < apply_list_->topLevelItemCount(); ++row) {
        const auto* item = apply_list_->topLevelItem(row);
        if (item->checkState(0) == Qt::Checked) {
            albums.push_back(static_cast<std::size_t>(item->data(0, Qt::UserRole).toInt()));
        }
    }
    return albums;
}

void IdentifyAlbumsDialog::syncApply() {
    const auto& albums = session_->albums();
    const auto* writing = session_->writing();
    const auto* scanning = session_->scanning();
    const auto running = writing != nullptr || scanning != nullptr;
    const auto done = !written_.empty() && !running;
    const auto chosen = albumsToWrite();
    std::size_t files = 0U;
    for (const auto album : chosen) {
        files += albums[album].items.size();
    }
    const auto layout_of = [this](const QComboBox* combo) {
        return tagger_ != nullptr ? tagger_->layoutProfile(combo->currentIndex())
                                  : std::optional<operations::OutputLayoutProfile>{};
    };
    const auto rename_layout = layout_of(rename_preset_);
    const auto move_layout = layout_of(move_preset_);
    rename_pattern_->setText(rename_layout ? QString::fromStdString(rename_layout->basename_expression)
                                           : QString{});
    move_pattern_->setText(move_layout
                               ? QString::fromStdString(move_layout->relative_directory_expression)
                               : QString{});
    const auto editable = !running && !done;
    apply_list_->setEnabled(editable);
    rename_->setEnabled(editable && rename_preset_->count() > 0);
    move_->setEnabled(editable && move_preset_->count() > 0);
    rename_preset_->setEnabled(editable && rename_->isChecked());
    move_preset_->setEnabled(editable && move_->isChecked());
    replaygain_->setEnabled(editable);
    replaygain_mode_->setEnabled(editable && replaygain_->isChecked());
    replaygain_skip_->setEnabled(editable && replaygain_->isChecked());
    const auto album_gain = replaygain_mode_->currentIndex() == 0;
    const auto with_gain = static_cast<std::size_t>(std::ranges::count_if(
        chosen, [this, album_gain](auto album) { return session_->hasGain(album, album_gain); }));
    replaygain_hint_->setText(chosen.empty() ? QString{}
                                             : QStringLiteral("%1 of these %2 have it")
                                                   .arg(with_gain)
                                                   .arg(chosen.size()));
    write_progress_->setVisible(running);
    write_stop_->setVisible(running);
    apply_back_->setEnabled(!running);
    if (writing != nullptr) {
        write_progress_->setMaximum(static_cast<int>(std::max<std::size_t>(writing->filesTotal(), 1U)));
        write_progress_->setValue(static_cast<int>(writing->filesDone()));
    } else if (scanning != nullptr) {
        // 0: under way, without a count.
        write_progress_->setMaximum(scanning->progressMaximum());
        write_progress_->setValue(scanning->progressValue());
    }

    // After a write, each album as it came out.
    if (!written_.empty()) {
        const QSignalBlocker blocker{apply_list_};
        for (int row = 0; row < apply_list_->topLevelItemCount(); ++row) {
            auto* item = apply_list_->topLevelItem(row);
            const auto album = static_cast<std::size_t>(item->data(0, Qt::UserRole).toInt());
            if (std::ranges::find(written_, album) != written_.end()) {
                item->setText(3, writing != nullptr ? QStringLiteral("Writing…")
                                                    : AlbumBatchSession::stateText(albums[album]));
            }
        }
    }
    const auto replaygain_status = session_->replayGainStatus();
    if (scanning != nullptr) {
        apply_heading_->setText(QStringLiteral("<b>Scanning ReplayGain</b><br>%1")
                                    .arg(escaped(scanning->status())));
    } else if (done) {
        const auto wrote = static_cast<std::size_t>(std::ranges::count_if(
            written_, [&albums](auto album) { return albums[album].state == State::written; }));
        apply_heading_->setText(
            QStringLiteral("<b>Wrote %1 of %2 albums</b><br>Those not written stay staged, saying "
                           "why; apply again once that is seen to.%3")
                .arg(wrote)
                .arg(written_.size())
                .arg(replaygain_status.isEmpty()
                         ? QString{}
                         : QStringLiteral("<br>ReplayGain: %1")
                               .arg(escaped(replaygain_status.section(QLatin1Char('\n'), 0, 0)))));
        apply_heading_->setToolTip(replaygain_status);
    } else {
        apply_heading_->setText(
            QStringLiteral("<b>%1 of %2 staged albums · %3 to write</b><br>One write, each file "
                           "journaled. Each file is read again first; an album with one that "
                           "changed since is left out, saying why, and the rest written.")
                .arg(chosen.size())
                .arg(apply_list_->topLevelItemCount())
                .arg(files_text(files)));
    }
    const auto later = session_->count(State::needs_choice) + session_->count(State::skipped) +
                       session_->count(State::no_match) + session_->count(State::failed);
    apply_note_->setText(later == 0U ? QString{}
                                     : QStringLiteral("Not in this write: %1 not staged — they "
                                                      "stay in the list for later")
                                           .arg(later));
    if (done) {
        write_->setText(QStringLiteral("Back to the list"));
        write_->setEnabled(true);
    } else if (running) {
        write_->setText(scanning != nullptr ? QStringLiteral("Scanning…")
                                            : QStringLiteral("Writing…"));
        write_->setEnabled(false);
    } else {
        write_->setText(QStringLiteral("Write %1 %2")
                            .arg(chosen.size())
                            .arg(chosen.size() == 1U ? QStringLiteral("album")
                                                     : QStringLiteral("albums")));
        write_->setEnabled(!chosen.empty() && session_->canWrite() &&
                           (!rename_->isChecked() || rename_layout) &&
                           (!move_->isChecked() || move_layout));
    }
    write_->setDefault(true);
}

void IdentifyAlbumsDialog::write() {
    if (tagger_ == nullptr) {
        return;
    }
    AlbumBatchWrite::Options options;
    if (rename_->isChecked()) {
        options.rename = tagger_->layoutProfile(rename_preset_->currentIndex());
        if (!options.rename) {
            return;
        }
    }
    if (move_->isChecked()) {
        options.move = tagger_->layoutProfile(move_preset_->currentIndex());
        if (!options.move) {
            return;
        }
    }
    auto albums = albumsToWrite();
    if (albums.empty()) {
        return;
    }
    std::optional<AlbumBatchSession::ReplayGain> replaygain;
    if (replaygain_->isChecked()) {
        replaygain = AlbumBatchSession::ReplayGain{
            .album_gain = replaygain_mode_->currentIndex() == 0,
            .skip_existing = replaygain_skip_->isChecked(),
        };
    }
    written_ = albums;
    session_->write(std::move(albums), std::move(options), replaygain);
    if (session_->writing() == nullptr) {
        written_.clear();
    }
    syncApply();
}

void IdentifyAlbumsDialog::closeEvent(QCloseEvent* event) {
    // The files under way are finished first.
    if (session_->writing() != nullptr || session_->scanning() != nullptr) {
        session_->stopWriting();
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void IdentifyAlbumsDialog::reject() {
    if (session_->writing() != nullptr || session_->scanning() != nullptr) {
        session_->stopWriting();
        return;
    }
    QDialog::reject();
}

void IdentifyAlbumsDialog::showDetail() {
    const auto index = selectedAlbum();
    if (index < 0) {
        detail_->clear();
        split_->setEnabled(false);
        merge_->setEnabled(false);
        return;
    }
    const auto& album = session_->albums()[static_cast<std::size_t>(index)];
    split_->setEnabled(album.folders.size() > 1U);
    merge_->setEnabled(session_->albums().size() > 1U);
    QString html = QStringLiteral("<h3>%1</h3>").arg(escaped(name_of(album)));
    html += QStringLiteral("<p>%1 · grouped by %2</p>")
                .arg(escaped(detail_of(album)), basis_text(album));
    if (session_->started()) {
        html += QStringLiteral("<p><b>%1</b></p>").arg(escaped(AlbumBatchSession::stateText(album)));
    }
    if (album.result && !album.result->candidates.empty()) {
        const auto& best = album.result->candidates.front();
        const auto& release = best.release;
        const auto& alignment = best.alignment;
        const musicbrainz::ClearMatchRule rule;
        html += QStringLiteral("<p>Release: %1 · %2 · %3 · %4 tracks</p>")
                    .arg(escaped(QString::fromStdString(release.title)),
                         escaped(QString::fromStdString(release.date)),
                         escaped(QString::fromStdString(release.country)))
                    .arg(alignment.release_tracks.size());
        html += QStringLiteral("<p>Why %1:</p>")
                    .arg(best.clear ? QStringLiteral("it matched") : QStringLiteral("it needs you"));
        html += check(alignment.release_tracks.size() == album.items.size() &&
                          alignment.matched_count == album.items.size(),
                      QStringLiteral("%1 files, %2 tracks, %3 paired")
                          .arg(album.items.size())
                          .arg(alignment.release_tracks.size())
                          .arg(alignment.matched_count));
        html += check(alignment.durations_compared == album.items.size() &&
                          alignment.worst_duration_delta_ms <= rule.maximum_duration_delta_ms,
                      alignment.durations_compared == 0U
                          ? QStringLiteral("No lengths to compare")
                          : QStringLiteral("Lengths within %1 s (%2 of %3 compared)")
                                .arg(static_cast<double>(alignment.worst_duration_delta_ms) / 1000.0, 0, 'f', 1)
                                .arg(alignment.durations_compared)
                                .arg(album.items.size()));
        html += check(alignment.weakest_title >= rule.minimum_title_similarity,
                      QStringLiteral("Titles at least %1% alike")
                          .arg(qRound(alignment.weakest_title * 100.0)));
        html += check(alignment.method == musicbrainz::AlignmentMethod::numbers ||
                          alignment.method == musicbrainz::AlignmentMethod::order,
                      alignment.method == musicbrainz::AlignmentMethod::numbers
                          ? QStringLiteral("Paired by track number")
                      : alignment.method == musicbrainz::AlignmentMethod::order
                          ? QStringLiteral("Paired in order")
                          : QStringLiteral("Paired by title, not by number"));
        if (album.result->candidates.size() > 1U) {
            html += QStringLiteral("<p>%1 versions fit.</p>").arg(album.result->candidates.size());
        }
    }
    if (!album.note.isEmpty()) {
        html += QStringLiteral("<p>%1</p>").arg(escaped(album.note));
    }
    html += QStringLiteral("<p>Folders:</p>");
    for (const auto& folder : album.folders) {
        html += QStringLiteral("<div><code>%1</code></div>")
                    .arg(escaped(QString::fromStdString(folder)));
    }
    detail_->setHtml(html);
}

} // namespace trackknife::bench
