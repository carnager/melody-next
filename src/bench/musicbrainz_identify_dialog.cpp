// SPDX-License-Identifier: GPL-3.0-only

#include "bench/musicbrainz_identify_dialog.hpp"
#include "bench/musicbrainz_track_match_widget.hpp"
#include "uicommon/queue_item_delegate.hpp"
#include "workspace/identify_session.hpp"

#include "trackknife/musicbrainz/acoustid.hpp"
#include "trackknife/musicbrainz/proposal_bridge.hpp"
#include "trackknife/musicbrainz/web_service.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
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

// The quieter line beneath an item's text.
constexpr int second_line_role = Qt::UserRole + 1;

// Two lines to a row: the item's text, and beneath it, smaller and quieter,
// its second line -- each cut short on its own, as the track lists draw a
// second line.
class TwoLineDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        auto item = option;
        initStyleOption(&item, index);
        const auto* widget = item.widget;
        auto* style = widget != nullptr ? widget->style() : QApplication::style();
        const auto first = item.text;
        item.text.clear();
        style->drawControl(QStyle::CE_ItemViewItem, &item, painter, widget);
        const auto area =
            style->subElementRect(QStyle::SE_ItemViewItemText, &item, widget).adjusted(3, 0, -3, 0);
        const bool selected = (item.state & QStyle::State_Selected) != 0;
        const QFontMetrics metrics{item.font};
        const auto quiet_font = ui::trackSecondLineFont(item.font);
        const QFontMetrics quiet_metrics{quiet_font};
        const auto quiet_height = ui::trackSecondLineHeight(item.font);
        const auto top = area.top() + (area.height() - metrics.height() - quiet_height) / 2;
        painter->save();
        painter->setFont(item.font);
        painter->setPen(item.palette.color(selected ? QPalette::HighlightedText : QPalette::Text));
        painter->drawText(QRect{area.left(), top, area.width(), metrics.height()},
                          Qt::AlignLeft | Qt::AlignVCenter,
                          metrics.elidedText(first, Qt::ElideRight, area.width()));
        painter->setFont(quiet_font);
        painter->setPen(
            item.palette.color(selected ? QPalette::HighlightedText : QPalette::PlaceholderText));
        painter->drawText(
            QRect{area.left(), top + metrics.height(), area.width(), quiet_height},
            Qt::AlignLeft | Qt::AlignVCenter,
            quiet_metrics.elidedText(index.data(second_line_role).toString(), Qt::ElideRight,
                                     area.width()));
        painter->restore();
    }

    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(QFontMetrics{option.font}.height() + ui::trackSecondLineHeight(option.font) +
                       8);
        return size;
    }
};

// A list of two-line rows, one column, no header.
void twoLines(QTreeWidget* list) {
    list->setColumnCount(1);
    list->setHeaderHidden(true);
    list->setRootIsDecorated(false);
    list->setUniformRowHeights(true);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list->setItemDelegate(new TwoLineDelegate(list));
}

// One album's page over its IdentifySession (ADR-0311): its search and
// release versions above, and below them how its files pair with the version
// chosen -- previewed as it is chosen, changed by dragging files.
class IdentifyAlbumPage final : public QWidget {
  public:
    IdentifyAlbumPage(IdentifySession* session, QWidget* parent)
        : QWidget(parent), session_(session) {
        setObjectName(QStringLiteral("bench-musicbrainz-identify-album"));
        auto* layout = new QVBoxLayout(this);
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
            QStringLiteral("An album title, or a MusicBrainz release's or album's link"));
        release_->setToolTip(QStringLiteral(
            "A release's link or id — musicbrainz.org/release/… — looks up that release "
            "instead of searching, with its album's other versions; an album's link — "
            "musicbrainz.org/release-group/… — lists every version of it"));
        form->addRow(QStringLiteral("Album:"), release_);
        auto* form_row = new QHBoxLayout;
        form_row->addLayout(form, 1);
        search_ = new QPushButton(QStringLiteral("Search"), this);
        search_->setObjectName(QStringLiteral("bench-musicbrainz-identify-search"));
        search_->setAutoDefault(false);
        form_row->addWidget(search_, 0, Qt::AlignBottom);
        scan_ = new QPushButton(QStringLiteral("Fingerprint files"), this);
        scan_->setObjectName(QStringLiteral("bench-musicbrainz-identify-scan"));
        scan_->setAutoDefault(false);
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

        auto* splitter = new QSplitter(Qt::Vertical, this);
        splitter->setChildrenCollapsible(false);
        results_ = new QTreeWidget(splitter);
        results_->setObjectName(QStringLiteral("bench-musicbrainz-identify-results"));
        results_->setAccessibleName(QStringLiteral("MusicBrainz release versions"));
        // Two lines each: artist and album, as the albums are named, then
        // what tells this version from the others.
        twoLines(results_);
        results_->setAlternatingRowColors(true);
        match_holder_ = new QWidget(splitter);
        match_holder_->setObjectName(QStringLiteral("bench-musicbrainz-identify-matching"));
        auto* holder_layout = new QVBoxLayout(match_holder_);
        holder_layout->setContentsMargins(0, 0, 0, 0);
        match_heading_ = new QLabel(match_holder_);
        match_heading_->setObjectName(QStringLiteral("bench-musicbrainz-identify-match-heading"));
        match_heading_->setTextFormat(Qt::PlainText);
        match_heading_->setWordWrap(true);
        holder_layout->addWidget(match_heading_);
        splitter->addWidget(results_);
        splitter->addWidget(match_holder_);
        // The versions a few rows high -- they are seldom many -- and the
        // pairing the rest: it is what is checked, file by file.
        splitter->setStretchFactor(0, 0);
        splitter->setStretchFactor(1, 1);
        const auto row = results_->fontMetrics().height() * 2 + 10;
        splitter->setSizes({row * 6, row * 20});
        layout->addWidget(splitter, 1);

        // Enter in a search field searches.
        for (auto* field : {artist_, release_}) {
            connect(field, &QLineEdit::returnPressed, search_, &QPushButton::click);
        }
        connect(search_, &QPushButton::clicked, this,
                [this] { session_->search(artist_->text(), release_->text()); });
        connect(scan_, &QPushButton::clicked, session_, &IdentifySession::scan);
        // A version chosen: its pairing previewed beneath.
        connect(results_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
            if (item != nullptr) {
                session_->preview(results_->indexOfTopLevelItem(item));
            }
        });
        connect(session_, &IdentifySession::changed, this, &IdentifyAlbumPage::sync);
        connect(session_, &IdentifySession::matchOpened, this, [this](TrackMatchSession* match) {
            delete panes_.data();
            panes_ = createMusicBrainzTrackMatchPanes(match, match_holder_);
            match_holder_->layout()->addWidget(panes_);
            // ADR-0330: and how its files fit, as they are paired.
            const auto heading = [this, match] {
                const auto fit = match->fitSummary();
                match_heading_->setText(fit.isEmpty() ? match->heading()
                                                      : match->heading() + QStringLiteral(" — ") +
                                                            fit);
            };
            connect(match, &TrackMatchSession::changed, panes_, heading);
            heading();
        });
        connect(session_, &IdentifySession::matchClosed, this, [this] {
            delete panes_.data();
            match_heading_->clear();
        });
        sync();
    }

  private:
    void sync() {
        const auto& rows = session_->candidates();
        // Rows come a search at a time, or one candidate after another.
        if (results_->topLevelItemCount() > static_cast<int>(rows.size())) {
            const QSignalBlocker blocker{results_};
            results_->clear();
        }
        for (auto row = static_cast<std::size_t>(results_->topLevelItemCount()); row < rows.size();
             ++row) {
            const auto& candidate = rows[row];
            auto* item = new QTreeWidgetItem(results_);
            // Another version of the same album says nothing the list does
            // not; how a release was found, or its track count, does.
            auto match = candidate.match;
            if (match.startsWith(QStringLiteral("Same album"))) {
                match = match.section(QStringLiteral(" · "), 1);
            }
            item->setText(0, candidate.artist.isEmpty()
                                 ? candidate.album
                                 : QStringLiteral("%1 — %2").arg(candidate.artist, candidate.album));
            QStringList details;
            if (!match.isEmpty()) {
                details << match;
            }
            details << (candidate.tracks == QStringLiteral("1")
                            ? QStringLiteral("1 track")
                            : QStringLiteral("%1 tracks").arg(candidate.tracks));
            for (const auto& part : {candidate.media, candidate.version}) {
                if (!part.isEmpty()) {
                    details << part;
                }
            }
            item->setData(0, second_line_role, details.join(QStringLiteral(" · ")));
            item->setToolTip(0, QStringLiteral("%1\n%2\n%3")
                                    .arg(item->text(0), details.join(QStringLiteral(" · ")),
                                         candidate.match_tool_tip.isEmpty()
                                             ? candidate.match
                                             : candidate.match + QStringLiteral(" — ") +
                                                   candidate.match_tool_tip));
        }
        // The best one previewed by itself once the versions are all there.
        if (results_->currentItem() == nullptr && !session_->busy() && session_->suggested() >= 0 &&
            session_->suggested() < results_->topLevelItemCount()) {
            results_->setCurrentItem(results_->topLevelItem(session_->suggested()));
        }
        status_->setText(session_->status());
        search_->setEnabled(session_->canSearch());
        scan_->setEnabled(session_->canScan());
        if (session_->match() == nullptr) {
            match_heading_->setText(results_->topLevelItemCount() > 0 && session_->busy()
                                        ? QStringLiteral("Loading its track list…")
                                        : QString{});
        }
    }

    IdentifySession* session_;
    QLineEdit* artist_{nullptr};
    QLineEdit* release_{nullptr};
    QPushButton* search_{nullptr};
    QPushButton* scan_{nullptr};
    QLabel* status_{nullptr};
    QTreeWidget* results_{nullptr};
    QWidget* match_holder_{nullptr};
    QLabel* match_heading_{nullptr};
    QPointer<QWidget> panes_;
};

// The albums, one page at a time (ADR-0311): a list of them beside the page
// shown, where there is more than one.
class MusicBrainzIdentifyDialog final : public QDialog {
    static constexpr auto fetch_covers_key = "identify/fetch-covers";

  public:
    MusicBrainzIdentifyDialog(MultiIdentifySession* session, QWidget* parent)
        : QDialog(parent), session_(session) {
        setObjectName(QStringLiteral("bench-musicbrainz-identify"));
        setWindowTitle(QStringLiteral("Identify with MusicBrainz"));
        setWindowModality(Qt::WindowModal);
        setAttribute(Qt::WA_DeleteOnClose);
        setMinimumSize(720, 480);
        const auto many = session_->count() > 1U;
        // Over the tag editor it is opened from, as large as it is.
        if (const auto* over = parent != nullptr ? parent->window() : nullptr) {
            resize(over->size());
            move(over->pos());
        } else {
            resize(1000, 700);
        }

        auto* root = new QVBoxLayout(this);
        auto* splitter = new QSplitter(this);
        splitter->setChildrenCollapsible(false);
        albums_ = new QTreeWidget(splitter);
        albums_->setObjectName(QStringLiteral("bench-musicbrainz-identify-albums"));
        albums_->setAccessibleName(QStringLiteral("Albums to identify"));
        // Two lines each: the album, and how far it is.
        twoLines(albums_);
        for (std::size_t index = 0; index < session_->count(); ++index) {
            auto* item = new QTreeWidgetItem(albums_);
            item->setText(0, session_->name(index));
            item->setToolTip(0, session_->name(index));
        }
        albums_->setVisible(many);
        pages_ = new QStackedWidget(splitter);
        splitter->addWidget(albums_);
        splitter->addWidget(pages_);
        splitter->setStretchFactor(1, 1);
        splitter->setSizes({260, 1000});
        root->addWidget(splitter, 1);

        auto* bottom = new QHBoxLayout;
        place_ = new QLabel(this);
        place_->setObjectName(QStringLiteral("bench-musicbrainz-identify-place"));
        bottom->addWidget(place_, 1);
        previous_ = new QPushButton(QStringLiteral("Previous"), this);
        previous_->setObjectName(QStringLiteral("bench-musicbrainz-identify-previous"));
        previous_->setToolTip(QStringLiteral("The album before (Alt+Page Up)"));
        next_ = new QPushButton(QStringLiteral("Next"), this);
        next_->setObjectName(QStringLiteral("bench-musicbrainz-identify-next"));
        next_->setToolTip(QStringLiteral("The album after (Alt+Page Down)"));
        skip_ = new QPushButton(QStringLiteral("Skip"), this);
        skip_->setObjectName(QStringLiteral("bench-musicbrainz-identify-skip"));
        skip_->setToolTip(QStringLiteral("Leave this album as it is, and go on to the next"));
        for (auto* button : {previous_, next_, skip_}) {
            button->setAutoDefault(false);
            button->setVisible(many);
            bottom->addWidget(button);
        }
        // ADR-0316: the covers of the albums applied, fetched with them.
        fetch_covers_ = new QCheckBox(QStringLiteral("Fetch covers"), this);
        fetch_covers_->setObjectName(QStringLiteral("bench-musicbrainz-identify-fetch-covers"));
        fetch_covers_->setToolTip(QStringLiteral(
            "Stage each album's front cover from the Cover Art Archive with its tags, as Fetch "
            "cover… does -- where Cover settings keep covers"));
        fetch_covers_->setChecked(
            QSettings{}.value(QLatin1String{fetch_covers_key}, true).toBool());
        session_->setFetchCovers(fetch_covers_->isChecked());
        connect(fetch_covers_, &QCheckBox::toggled, this, [this](const bool on) {
            QSettings{}.setValue(QLatin1String{fetch_covers_key}, on);
            session_->setFetchCovers(on);
        });
        bottom->addWidget(fetch_covers_);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        buttons->setObjectName(QStringLiteral("bench-musicbrainz-identify-buttons"));
        apply_ = buttons->addButton(QStringLiteral("Apply"), QDialogButtonBox::AcceptRole);
        apply_->setObjectName(QStringLiteral("bench-musicbrainz-identify-apply"));
        apply_->setAutoDefault(false);
        buttons->button(QDialogButtonBox::Close)
            ->setToolTip(QStringLiteral("Close without applying"));
        bottom->addWidget(buttons);
        root->addLayout(bottom);

        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
        connect(apply_, &QPushButton::clicked, session_, &MultiIdentifySession::apply);
        connect(previous_, &QPushButton::clicked, this, [this] { step(-1); });
        connect(next_, &QPushButton::clicked, this, [this] { step(1); });
        connect(skip_, &QPushButton::clicked, this, [this] {
            const auto at = session_->current();
            const auto skipping = session_->state(at) != MultiIdentifySession::State::skipped;
            session_->setSkipped(at, skipping);
            if (skipping) {
                step(1);
            }
        });
        if (many) {
            auto* back = new QShortcut(QKeySequence{Qt::ALT | Qt::Key_PageUp}, this);
            auto* on = new QShortcut(QKeySequence{Qt::ALT | Qt::Key_PageDown}, this);
            connect(back, &QShortcut::activated, this, [this] { step(-1); });
            connect(on, &QShortcut::activated, this, [this] { step(1); });
        }
        connect(albums_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
            if (item != nullptr && !showing_) {
                show(static_cast<std::size_t>(albums_->indexOfTopLevelItem(item)));
            }
        });
        connect(session_, &MultiIdentifySession::changed, this, &MusicBrainzIdentifyDialog::sync);
        // Every album applied staged: done.
        connect(session_, &MultiIdentifySession::finished, this, &QDialog::close);
        show(0U);
    }

    ~MusicBrainzIdentifyDialog() override {
        // Albums applied and not yet taken by the tagger are still staged --
        // unless the tagger went first, closed with its window, and the
        // session with it.
        if (!session_.isNull()) {
            session_->release();
        }
    }

    MusicBrainzIdentifyDialog(const MusicBrainzIdentifyDialog&) = delete;
    MusicBrainzIdentifyDialog& operator=(const MusicBrainzIdentifyDialog&) = delete;

  private:
    void step(const int direction) {
        const auto count = static_cast<int>(session_->count());
        const auto at = static_cast<int>(session_->current()) + direction;
        if (at >= 0 && at < count) {
            show(static_cast<std::size_t>(at));
        }
    }

    void show(const std::size_t index) {
        if (index >= session_->count()) {
            return;
        }
        auto*& page = pages_by_album_[index];
        if (page == nullptr) {
            page = new IdentifyAlbumPage(session_->album(index), pages_);
            pages_->addWidget(page);
        }
        pages_->setCurrentWidget(page);
        session_->setCurrent(index);
        {
            showing_ = true;
            albums_->setCurrentItem(albums_->topLevelItem(static_cast<int>(index)));
            showing_ = false;
        }
        if (auto* field = page->findChild<QLineEdit*>(
                QStringLiteral("bench-musicbrainz-identify-release"))) {
            field->setFocus();
        }
        sync();
    }

    void sync() {
        for (std::size_t index = 0; index < session_->count(); ++index) {
            if (auto* item = albums_->topLevelItem(static_cast<int>(index))) {
                item->setData(0, second_line_role, session_->stateText(index));
            }
        }
        const auto at = session_->current();
        place_->setText(session_->count() > 1U ? QStringLiteral("Album %1 of %2 · %3")
                                                     .arg(at + 1U)
                                                     .arg(session_->count())
                                                     .arg(session_->name(at))
                                               : QString{});
        previous_->setEnabled(at > 0U);
        next_->setEnabled(at + 1U < session_->count());
        const auto ready = session_->readyCount();
        apply_->setText(session_->count() < 2U ? QStringLiteral("Apply")
                        : ready == 1U          ? QStringLiteral("Apply 1 album")
                                               : QStringLiteral("Apply %1 albums").arg(ready));
        apply_->setToolTip(QStringLiteral(
            "Stage the version chosen for every album not skipped into the tag editor's draft"));
        apply_->setEnabled(ready > 0U && !session_->staging());
        skip_->setText(session_->state(at) == MultiIdentifySession::State::skipped
                           ? QStringLiteral("Don't skip")
                           : QStringLiteral("Skip"));
        skip_->setEnabled(!session_->staging());
    }

    QPointer<MultiIdentifySession> session_;
    QTreeWidget* albums_{nullptr};
    QStackedWidget* pages_{nullptr};
    std::map<std::size_t, IdentifyAlbumPage*> pages_by_album_;
    QLabel* place_{nullptr};
    QPushButton* previous_{nullptr};
    QPushButton* next_{nullptr};
    QPushButton* skip_{nullptr};
    QPushButton* apply_{nullptr};
    QCheckBox* fetch_covers_{nullptr};
    bool showing_{false};
};

} // namespace

QDialog* createMusicBrainzIdentifyDialog(TaggerSession& tagger, MusicBrainzLookupService service,
                                         std::vector<TaggerSession::Identify> albums,
                                         QWidget* parent) {
    // The tagger's, not the window's: accepted albums are still staged when
    // the window closes first.
    auto* session = new MultiIdentifySession(tagger, std::move(service), std::move(albums), &tagger);
    return new MusicBrainzIdentifyDialog(session, parent);
}

} // namespace trackknife::bench
