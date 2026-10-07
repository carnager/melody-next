// SPDX-License-Identifier: GPL-3.0-only
#include "bench/identify_albums_dialog.hpp"

#include "bench/apply_actions_popover.hpp"
#include "bench/musicbrainz_track_match_widget.hpp"
#include "bench/preparation_feedback_dialog.hpp"
#include "trackknife/musicbrainz/web_service.hpp"
#include "workspace/album_batch_session.hpp"
#include "workspace/identify_session.hpp"
#include "workspace/tagger_session.hpp"

#include <QButtonGroup>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QShortcut>
#include <QTimer>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

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

// Its media, counted by kind: "1×CD", "2×CD + DVD".
[[nodiscard]] QString format_of(const musicbrainz::Release& release) {
    std::vector<std::pair<QString, int>> kinds;
    for (const auto& medium : release.media) {
        const auto kind = medium.format.empty() ? QStringLiteral("Medium")
                                                : QString::fromStdString(medium.format);
        const auto found = std::ranges::find(kinds, kind, &std::pair<QString, int>::first);
        if (found == kinds.end()) {
            kinds.emplace_back(kind, 1);
        } else {
            ++found->second;
        }
    }
    QStringList parts;
    for (const auto& [kind, count] : kinds) {
        parts << (count == 1 && kinds.size() > 1U ? kind
                                                  : QStringLiteral("%1×%2").arg(count).arg(kind));
    }
    return parts.join(QStringLiteral(" + "));
}

[[nodiscard]] QString check(const bool ok, const QString& text) {
    return QStringLiteral("<div>%1 %2</div>")
        .arg(ok ? QStringLiteral("✓") : QStringLiteral("✗"), escaped(text));
}

} // namespace

IdentifyAlbumsDialog::IdentifyAlbumsDialog(TaggerSession& tagger, MusicBrainzLookupService service,
                                           QWidget* parent)
    : IdentifyAlbumsDialog(parent) {
    tagger_ = &tagger;
    begin(std::move(service));
}

IdentifyAlbumsDialog::IdentifyAlbumsDialog(const std::size_t track_count,
                                           MetadataPropertiesSourceReader reader,
                                           std::span<const std::string_view> fields,
                                           TaggerServices services, QWidget* parent)
    : IdentifyAlbumsDialog(parent) {
    // Its own tagger, without the editor's window: the files read, and
    // grouped once they are.
    auto service = services.musicbrainz;
    owns_tagger_ = true;
    tagger_ = new TaggerSession(track_count, std::move(reader), fields, std::move(services), this);
    heading_->setText(QStringLiteral("Reading %1 files…").arg(track_count));
    pages_->setEnabled(false);
    bottom_bar_->setEnabled(false);
    connect(tagger_, &TaggerSession::changed, this, [this] {
        if (session_ == nullptr && tagger_ != nullptr) {
            const auto loading = tagger_->loadingText();
            heading_->setText(loading.isEmpty() ? tagger_->status() : loading);
        }
    });
    connect(tagger_, &TaggerSession::gridReady, this,
            [this, service] {
                pages_->setEnabled(true);
                bottom_bar_->setEnabled(true);
                begin(service);
            },
            Qt::SingleShotConnection);
    // What the editor would show: the files a scan could not measure, say.
    connect(tagger_, &TaggerSession::feedbackRequested, this,
            [this](const QString& title, const QString& summary,
                   std::vector<PreparationFeedbackRow> rows, bool) {
                createPreparationFeedbackDialog(title, summary, rows, this)->show();
            });
    tagger_->start();
}

IdentifyAlbumsDialog::IdentifyAlbumsDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("bench-identify-albums"));
    setWindowTitle(QStringLiteral("Identify albums"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(880, 620);
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
    progress_text_->setWordWrap(true);
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

    // The review: one album needing a person -- its versions above, the
    // files and tracks below, and one line to go on. ↑/↓ in the versions
    // choose one; Tab reaches the files.
    auto* review_page = new QWidget(pages_);
    auto* review_layout = new QVBoxLayout(review_page);
    review_layout->setContentsMargins(0, 0, 0, 0);
    auto* review_top = new QHBoxLayout;
    review_heading_ = new QLabel(review_page);
    review_heading_->setObjectName(QStringLiteral("bench-identify-albums-review-heading"));
    review_heading_->setWordWrap(true);
    review_place_ = new QLabel(review_page);
    review_place_->setObjectName(QStringLiteral("bench-identify-albums-review-place"));
    review_top->addWidget(review_heading_, 1);
    review_top->addWidget(review_place_);
    review_layout->addLayout(review_top);
    // The versions that fit, side by side in what tells them apart.
    versions_ = new QTreeWidget(review_page);
    versions_->setObjectName(QStringLiteral("bench-identify-albums-versions"));
    versions_->setToolTip(QStringLiteral("The versions that fit: ↑/↓ choose one"));
    versions_->setRootIsDecorated(false);
    versions_->setUniformRowHeights(true);
    versions_->setAllColumnsShowFocus(true);
    versions_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    versions_->setHeaderLabels({QStringLiteral("Version"), QStringLiteral("Released"),
                                QStringLiteral("Country"), QStringLiteral("Label"),
                                QStringLiteral("Cat. no."), QStringLiteral("Format"),
                                QStringLiteral("Tracks"), QStringLiteral("Fit")});
    versions_->header()->setStretchLastSection(false);
    for (int column = 0; column < versions_->columnCount(); ++column) {
        versions_->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }
    versions_->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    review_layout->addWidget(versions_);
    match_holder_ = new QWidget(review_page);
    auto* holder_layout = new QVBoxLayout(match_holder_);
    holder_layout->setContentsMargins(0, 0, 0, 0);
    review_layout->addWidget(match_holder_, 1);
    auto* review_buttons = new QHBoxLayout;
    review_status_ = new QLabel(review_page);
    review_status_->setObjectName(QStringLiteral("bench-identify-albums-review-status"));
    auto* back = new QPushButton(QStringLiteral("Back to the list"), review_page);
    back->setObjectName(QStringLiteral("bench-identify-albums-review-back"));
    auto* skip_button = new QPushButton(QStringLiteral("Skip (S)"), review_page);
    skip_button->setObjectName(QStringLiteral("bench-identify-albums-review-skip"));
    skip_button->setToolTip(QStringLiteral("Leave this album for later and go to the next"));
    accept_ = new QPushButton(QStringLiteral("Accept and next (Enter)"), review_page);
    accept_->setObjectName(QStringLiteral("bench-identify-albums-review-accept"));
    accept_->setToolTip(QStringLiteral("Stage this pairing and go to the next album needing you"));
    review_buttons->addWidget(review_status_, 1);
    review_buttons->addWidget(back);
    review_buttons->addWidget(skip_button);
    review_buttons->addWidget(accept_);
    review_layout->addLayout(review_buttons);
    pages_->addWidget(review_page);
    for (const auto& key : {QKeySequence{Qt::Key_Return}, QKeySequence{Qt::Key_Enter}}) {
        auto* accept_key = new QShortcut(key, review_page);
        accept_key->setContext(Qt::WidgetWithChildrenShortcut);
        connect(accept_key, &QShortcut::activated, this, &IdentifyAlbumsDialog::accept);
    }
    auto* skip_key = new QShortcut(QKeySequence{Qt::Key_S}, review_page);
    skip_key->setContext(Qt::WidgetWithChildrenShortcut);
    connect(skip_key, &QShortcut::activated, this, &IdentifyAlbumsDialog::skip);
    connect(accept_, &QPushButton::clicked, this, &IdentifyAlbumsDialog::accept);
    connect(skip_button, &QPushButton::clicked, this, &IdentifyAlbumsDialog::skip);
    connect(back, &QPushButton::clicked, this, &IdentifyAlbumsDialog::backToList);
    connect(versions_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        showVersion(versions_->indexOfTopLevelItem(item));
    });
    layout->addWidget(pages_, 1);

    // Write as the tagger's Actions say (ADR-0262).
    bottom_bar_ = new QWidget(this);
    auto* bottom = new QHBoxLayout(bottom_bar_);
    bottom->setContentsMargins(0, 0, 0, 0);
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("bench-identify-albums-summary"));
    summary_->setWordWrap(true);
    auto* close = new QPushButton(QStringLiteral("Close"), this);
    look_up_ = new QPushButton(QStringLiteral("Look up again"), this);
    look_up_->setObjectName(QStringLiteral("bench-identify-albums-look-up"));
    review_next_ = new QPushButton(this);
    review_next_->setObjectName(QStringLiteral("bench-identify-albums-review-next"));
    actions_ = new QPushButton(QStringLiteral("Actions…"), this);
    actions_->setObjectName(QStringLiteral("bench-identify-albums-actions"));
    write_ = new QPushButton(this);
    write_->setObjectName(QStringLiteral("bench-identify-albums-write"));
    // ADR-0263: the window stays after a Write, so it can be taken back.
    undo_ = new QPushButton(QStringLiteral("Undo this batch"), this);
    undo_->setObjectName(QStringLiteral("bench-identify-albums-undo"));
    undo_->setToolTip(
        QStringLiteral("Put every file the last Write changed back as it was, moved back where "
                       "it was moved, and stage its albums again"));
    // Actions on the left, as in the tag editor.
    bottom->addWidget(actions_);
    bottom->addWidget(summary_, 1);
    bottom->addWidget(close);
    bottom->addWidget(look_up_);
    bottom->addWidget(review_next_);
    bottom->addWidget(undo_);
    bottom->addWidget(write_);
    layout->addWidget(bottom_bar_);
    connect(review_next_, &QPushButton::clicked, this, &IdentifyAlbumsDialog::reviewNext);
    connect(actions_, &QPushButton::clicked, this, [this] {
        if (tagger_ == nullptr) {
            return;
        }
        auto* popover = new ApplyActionsPopover(*tagger_, {}, this);
        popover->showAt(actions_);
    });
    connect(write_, &QPushButton::clicked, this, [this] {
        if (session_ != nullptr) {
            session_->write();
        }
    });
    connect(undo_, &QPushButton::clicked, this, [this] {
        if (session_ != nullptr) {
            session_->undoLastWrite();
        }
    });
    connect(list_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        const auto album = static_cast<std::size_t>(item->data(0, Qt::UserRole).toInt());
        if (session_->albums()[album].state == State::needs_choice) {
            review(album);
        }
    });

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
        for (const auto other : session_->order()) {
            if (static_cast<int>(other) == album || !session_->editable(other)) {
                continue;
            }
            auto* action = menu.addAction(name_of(albums[other]));
            connect(action, &QAction::triggered, this, [this, album, other] {
                session_->merge(static_cast<std::size_t>(album), other);
            });
        }
        menu.exec(merge_->mapToGlobal(QPoint{0, merge_->height()}));
    });
    connect(look_up_, &QPushButton::clicked, this, [this] { session_->lookUp(); });
    connect(stop_, &QPushButton::clicked, this, [this] {
        if (session_->writing() != nullptr || session_->measuring()) {
            session_->stopWriting();
        } else {
            session_->stop();
        }
    });
    connect(close, &QPushButton::clicked, this, &IdentifyAlbumsDialog::reject);
}

void IdentifyAlbumsDialog::begin(MusicBrainzLookupService service) {
    session_ = new AlbumBatchSession(*tagger_, std::move(service), this);
    connect(session_, &AlbumBatchSession::changed, this, &IdentifyAlbumsDialog::sync);
    sync();
    if (list_->topLevelItemCount() > 0) {
        list_->setCurrentItem(list_->topLevelItem(0));
    }
    // Looked up from the start: a wrong group is put right meanwhile, and
    // looked up again. Test seam: held while the window it opened over says.
    if (parentWidget() != nullptr &&
        parentWidget()->window()->property("trackknife-identify-hold-lookup").toBool()) {
        return;
    }
    QTimer::singleShot(0, session_, &AlbumBatchSession::lookUp);
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
    const auto& order = session_->order();
    const auto started = session_->started();
    const auto included = order.size() - session_->count(State::left_out);
    heading_->setText(QStringLiteral("%1 files grouped into %2 albums")
                          .arg(session_->fileCount())
                          .arg(order.size()));

    // The lookup's progress -- the albums answered of those asked about --
    // or the write's.
    const auto open = session_->count(State::waiting) + session_->count(State::searching);
    const auto answered = included - open;
    const auto* writing = session_->writing();
    const auto measuring = session_->measuring();
    const auto undoing = session_->undoing();
    const auto busy = writing != nullptr || measuring || undoing;
    // Reviewing, only what still runs is shown above the album.
    const auto reviewing = pages_->currentIndex() == 1;
    heading_->setVisible(!reviewing);
    // The bar and Stop while something runs; the line saying how it went
    // after.
    const auto running = session_->lookingUp() || busy;
    progress_->setVisible(running);
    // An undo runs to its end: half undone is worse than either.
    stop_->setVisible(running && !undoing);
    progress_text_->setVisible(started && (!reviewing || running));
    if (writing != nullptr) {
        progress_->setMaximum(static_cast<int>(std::max<std::size_t>(writing->filesTotal(), 1U)));
        progress_->setValue(static_cast<int>(writing->filesDone()));
        progress_text_->setText(QStringLiteral("Writing · %1 of %2 files")
                                    .arg(writing->filesDone())
                                    .arg(writing->filesTotal()));
    } else if (measuring) {
        progress_->setMaximum(0);
        progress_text_->setText(QStringLiteral("Measuring ReplayGain before writing…"));
    } else if (undoing) {
        progress_->setMaximum(0);
        progress_text_->setText(QStringLiteral("Undoing the last Write…"));
    } else {
        progress_->setMaximum(static_cast<int>(std::max<std::size_t>(included, 1U)));
        progress_->setValue(static_cast<int>(answered));
        const auto seconds = session_->requestsLeft() *
                             static_cast<std::size_t>(musicbrainz::minimum_request_interval_ms) /
                             1000U;
        progress_text_->setText(
            session_->lookingUp()
                ? QStringLiteral("Looking up · %1 of %2 · MusicBrainz answers one request a "
                                 "second · about %3 left · you can review while it runs")
                      .arg(answered)
                      .arg(included)
                      .arg(seconds >= 120U ? QStringLiteral("%1 min").arg(seconds / 60U)
                                           : QStringLiteral("%1 s").arg(seconds))
            : !session_->writeSummary().isEmpty()
                ? session_->writeSummary()
                : QStringLiteral("Looked up · %1 of %2").arg(answered).arg(included));
    }
    const std::pair<int, std::size_t> counts[]{
        {all, order.size()},
        {needs_you, session_->count(State::needs_choice) + session_->count(State::failed)},
        {matched, session_->count(State::staging) + session_->count(State::staged) +
                      session_->count(State::written)},
        {no_match, session_->count(State::no_match)},
        {waiting, open}};
    for (const auto& [id, count] : counts) {
        auto* button = filter_group_->button(id);
        button->setText(QStringLiteral("%1 %2").arg(button->property("label").toString()).arg(count));
    }

    // The list, keeping the album looked at. The checkbox: looked up (and
    // then written), or left out.
    const auto current = selectedAlbum();
    syncing_ = true;
    {
        const QSignalBlocker blocker{list_};
        list_->clear();
        for (const auto index : order) {
            if (!shown(static_cast<int>(index))) {
                continue;
            }
            const auto& album = albums[index];
            auto* item = new QTreeWidgetItem(list_);
            item->setText(0, name_of(album));
            item->setText(1, detail_of(album));
            auto state = !started ? basis_text(album)
                          : session_->alreadyTagged(index)
                              ? QStringLiteral("Matched · already tagged so")
                              : AlbumBatchSession::stateText(album);
            if (album.excluded && album.state == State::staged) {
                state += QStringLiteral(" · not to be written");
            }
            item->setText(2, state);
            item->setData(0, Qt::UserRole, static_cast<int>(index));
            if (album.state != State::written && album.state != State::staging) {
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(0, album.state == State::left_out || album.excluded
                                           ? Qt::Unchecked
                                           : Qt::Checked);
            }
            if (static_cast<int>(index) == current) {
                list_->setCurrentItem(item);
            }
        }
    }
    // Something is always looked at, its details beside it.
    if (list_->currentItem() == nullptr && list_->topLevelItemCount() > 0) {
        const QSignalBlocker blocker{list_};
        list_->setCurrentItem(list_->topLevelItem(0));
    }
    syncing_ = false;
    const auto needing = session_->count(State::needs_choice);
    // Only what can be done now is offered.
    const auto can_review = session_->nextNeedingYou(std::nullopt).has_value();
    review_next_->setVisible(can_review);
    review_next_->setEnabled(can_review);
    review_next_->setText(QStringLiteral("Review next needing you (%1)").arg(needing));
    const auto again = session_->count(State::failed) + session_->count(State::waiting) +
                       session_->count(State::no_match);
    look_up_->setVisible(started && !session_->lookingUp() && !busy && again > 0U);
    const auto to_write = session_->toWrite().size();
    write_->setText(QStringLiteral("Write %1 %2")
                        .arg(to_write)
                        .arg(to_write == 1U ? QStringLiteral("album") : QStringLiteral("albums")));
    write_->setEnabled(session_->canWrite());
    write_->setVisible(to_write > 0U || !session_->canUndoLastWrite());
    undo_->setVisible(session_->canUndoLastWrite() || undoing);
    undo_->setEnabled(session_->canUndoLastWrite());
    write_->setDefault(needing == 0U && to_write > 0U);
    review_next_->setDefault(needing > 0U);
    actions_->setEnabled(!busy);
    // Staged and already tagged so is not something to write.
    const auto tagged_already = static_cast<std::size_t>(std::ranges::count_if(
        order, [this](const auto album) { return session_->alreadyTagged(album); }));
    summary_->setText(QStringLiteral("%1 staged · %2 need you · %3 no match · %4 waiting")
                          .arg(session_->count(State::staged) - tagged_already)
                          .arg(needing + session_->count(State::failed))
                          .arg(session_->count(State::no_match))
                          .arg(open) +
                      (tagged_already > 0U ? QStringLiteral(" · %1 already tagged").arg(tagged_already)
                                           : QString{}));
    showDetail();
    // The review opens by itself, once, as soon as an album needs a person.
    if (!reviewed_ && pages_->currentIndex() == 0 && !busy) {
        if (const auto next = session_->nextNeedingYou(std::nullopt)) {
            reviewed_ = true;
            QMetaObject::invokeMethod(
                this, [this, album = *next] { review(album); }, Qt::QueuedConnection);
        }
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
    // Why it is back, when it was accepted before and could not be staged.
    review_heading_->setText(
        QStringLiteral("<b>%1</b> · %2%3")
            .arg(escaped(name_of(entry)), escaped(detail_of(entry)),
                 entry.note.isEmpty() ? QString{}
                                      : QStringLiteral("<br>%1").arg(escaped(entry.note))));
    review_place_->setText(QStringLiteral("%1 of %2 needing you")
                               .arg(place)
                               .arg(session_->count(State::needs_choice)));
    {
        const QSignalBlocker blocker{versions_};
        versions_->clear();
        // A version's name only where it is not the album's own.
        bool named = false;
        const auto row_height = fontMetrics().height() + 12;
        for (const auto& candidate : entry.result->candidates) {
            const auto& release = candidate.release;
            auto name = QString::fromStdString(release.title);
            if (!release.disambiguation.empty()) {
                name += QStringLiteral(" (%1)").arg(QString::fromStdString(release.disambiguation));
            }
            const auto own = name == entry.title;
            named = named || !own;
            auto* item = new QTreeWidgetItem(versions_);
            item->setText(0, own ? QStringLiteral("—") : name);
            item->setText(1, QString::fromStdString(release.date));
            item->setText(2, QString::fromStdString(release.country));
            item->setText(3, QString::fromStdString(release.label));
            item->setText(4, QString::fromStdString(release.catalog_number));
            item->setText(5, format_of(release));
            item->setText(6, QString::number(candidate.alignment.release_tracks.size()));
            item->setText(7, QStringLiteral("%1%").arg(qRound(candidate.alignment.confidence * 100.0)));
            item->setTextAlignment(6, Qt::AlignRight | Qt::AlignVCenter);
            item->setTextAlignment(7, Qt::AlignRight | Qt::AlignVCenter);
            item->setToolTip(0, QStringLiteral("MusicBrainz release %1")
                                    .arg(QString::fromStdString(release.id)));
            // Roomy rows, each cell as wide as its text.
            for (int column = 0; column < versions_->columnCount(); ++column) {
                item->setSizeHint(
                    column,
                    QSize{fontMetrics().horizontalAdvance(item->text(column)) + 16, row_height});
            }
        }
        versions_->setColumnHidden(0, !named);
    }
    // As tall as its versions, up to five.
    const auto shown_rows = std::min(versions_->topLevelItemCount(), 5);
    versions_->setFixedHeight(versions_->header()->sizeHint().height() +
                              shown_rows * (fontMetrics().height() + 12) +
                              2 * versions_->frameWidth());
    pages_->setCurrentIndex(1);
    filters_->hide();
    bottom_bar_->hide();
    sync();
    {
        const QSignalBlocker blocker{versions_};
        versions_->setCurrentItem(versions_->topLevelItem(0));
    }
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
        std::move(files.descriptors), std::move(files.paths), std::move(files.items), this,
        std::move(files.other_albums));
    connect(match_, &TrackMatchSession::accepted, this,
            [this](metadata::MetadataProposalSet proposals) {
                if (reviewing_) {
                    session_->choose(*reviewing_,
                                     static_cast<std::size_t>(std::max(
                                         0, versions_->indexOfTopLevelItem(versions_->currentItem()))),
                                     std::move(proposals));
                    reviewNext();
                }
            });
    connect(match_, &TrackMatchSession::changed, this, &IdentifyAlbumsDialog::syncReviewStatus);
    match_view_ = createMusicBrainzTrackMatchPanes(match_, match_holder_);
    match_holder_->layout()->addWidget(match_view_);
    syncReviewStatus();
}

void IdentifyAlbumsDialog::syncReviewStatus() {
    if (match_ == nullptr) {
        review_status_->clear();
        accept_->setEnabled(false);
        return;
    }
    const auto paired = match_->pairedCount();
    const auto files = match_->fileCount();
    const auto gaps = match_->trackCount() - std::min(match_->trackCount(), paired);
    QStringList parts{QStringLiteral("%1 of %2 files paired").arg(paired).arg(files)};
    if (files > paired) {
        parts << QStringLiteral("%1 left unmatched").arg(files - paired);
    }
    if (gaps > 0U) {
        parts << QStringLiteral("%1 %2 without a file")
                     .arg(gaps)
                     .arg(gaps == 1U ? QStringLiteral("track") : QStringLiteral("tracks"));
    }
    review_status_->setText(parts.join(QStringLiteral(" · ")));
    accept_->setEnabled(match_->canStage());
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
    reviewing_.reset();
    pages_->setCurrentIndex(0);
    filters_->show();
    bottom_bar_->show();
    sync();
}

bool IdentifyAlbumsDialog::mayClose() {
    if (session_ == nullptr) {
        return true;
    }
    // An undo finishes first, as it cannot stop halfway.
    if (session_->undoing()) {
        return false;
    }
    // The files under way are finished first.
    if (session_->writing() != nullptr || session_->measuring()) {
        session_->stopWriting();
        return false;
    }
    // Opened by itself, nothing else holds what is staged -- what would
    // change, that is: an album already tagged so loses nothing.
    std::size_t staged = session_->count(State::staging);
    for (const auto album : session_->order()) {
        if (session_->albums()[album].state == State::staged && !session_->alreadyTagged(album)) {
            ++staged;
        }
    }
    if (!owns_tagger_ || staged == 0U) {
        return true;
    }
    return QMessageBox::question(
               this, QStringLiteral("Close Identify albums"),
               QStringLiteral("%1 %2 staged but not written. Close and drop them?")
                   .arg(staged)
                   .arg(staged == 1U ? QStringLiteral("album is") : QStringLiteral("albums are")),
               QMessageBox::Close | QMessageBox::Cancel, QMessageBox::Cancel) ==
           QMessageBox::Close;
}

// Closed from the window's frame: asked once, in reject() -- QDialog's own
// closeEvent calls reject() too, which asked a second time.
void IdentifyAlbumsDialog::closeEvent(QCloseEvent* event) {
    event->ignore();
    reject();
}

void IdentifyAlbumsDialog::reject() {
    if (mayClose()) {
        QDialog::reject();
    }
}

void IdentifyAlbumsDialog::showDetail() {
    const auto index = selectedAlbum();
    if (index < 0) {
        detail_->clear();
        split_->hide();
        merge_->hide();
        return;
    }
    const auto& album = session_->albums()[static_cast<std::size_t>(index)];
    const auto editable = session_->editable(static_cast<std::size_t>(index));
    // Grouping changes while it can.
    split_->setVisible(editable && album.folders.size() > 1U);
    merge_->setVisible(editable && session_->order().size() > 1U);
    QString html = QStringLiteral("<h3>%1</h3>").arg(escaped(name_of(album)));
    html += QStringLiteral("<p>%1 · grouped by %2</p>")
                .arg(escaped(detail_of(album)), basis_text(album));
    if (session_->started()) {
        html += QStringLiteral("<p><b>%1</b></p>")
                    .arg(escaped(session_->alreadyTagged(static_cast<std::size_t>(index))
                                     ? QStringLiteral("Matched · its files are already tagged as "
                                                      "this release: nothing to write")
                                     : AlbumBatchSession::stateText(album)));
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
