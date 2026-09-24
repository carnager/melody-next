// SPDX-License-Identifier: GPL-3.0-only

#include "bench/quick_pick_popup.hpp"

#include "trackknife/engine/catalogue.hpp"

#include <QApplication>
#include <QDateTime>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace trackknife::bench {
namespace {

constexpr int row_height = 30;
constexpr std::size_t result_limit = 60U;
constexpr int name_role = Qt::UserRole + 1;
constexpr int details_role = Qt::UserRole + 2;

[[nodiscard]] QString text(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

// One line per album: its title, then quieter who, when and how long.
class AlbumRowDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(row_height);
        return size;
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const auto& palette = option.palette;
        painter->save();
        if (option.state.testFlag(QStyle::State_Selected)) {
            const auto base = palette.color(QPalette::Base);
            const auto accent = palette.color(QPalette::Highlight);
            const auto mix = [](const int a, const int b) { return (a * 68 + b * 32) / 100; };
            painter->fillRect(option.rect, QColor::fromRgb(mix(base.red(), accent.red()),
                                                           mix(base.green(), accent.green()),
                                                           mix(base.blue(), accent.blue())));
        }
        const auto area = option.rect.adjusted(12, 0, -12, 0);
        const auto album = index.data(name_role).toString(); // the album or the title
        const auto details = index.data(details_role).toString();
        auto album_font = option.font;
        album_font.setWeight(QFont::DemiBold);
        const QFontMetrics album_metrics{album_font};
        const auto shown = album_metrics.elidedText(album, Qt::ElideRight, area.width() * 3 / 5);
        painter->setFont(album_font);
        painter->setPen(palette.color(QPalette::Text));
        painter->drawText(area, Qt::AlignLeft | Qt::AlignVCenter, shown);
        const auto used = album_metrics.horizontalAdvance(shown) + 10;
        painter->setFont(option.font);
        painter->setPen(palette.color(QPalette::PlaceholderText));
        painter->drawText(area.adjusted(used, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter,
                          option.fontMetrics.elidedText(details, Qt::ElideRight,
                                                        std::max(0, area.width() - used)));
        painter->restore();
    }
};

// "today", "yesterday", "5 days ago", "3 weeks ago", "4 months ago".
[[nodiscard]] QString added_ago(const std::int64_t added) {
    const auto days = (QDateTime::currentSecsSinceEpoch() - added) / 86'400;
    if (days <= 0) {
        return QObject::tr("added today");
    }
    if (days == 1) {
        return QObject::tr("added yesterday");
    }
    if (days < 14) {
        return QObject::tr("added %1 days ago").arg(days);
    }
    if (days < 61) {
        return QObject::tr("added %1 weeks ago").arg(days / 7);
    }
    if (days < 730) {
        return QObject::tr("added %1 months ago").arg(days / 30);
    }
    return QObject::tr("added %1 years ago").arg(days / 365);
}

} // namespace

QuickPickPopup::QuickPickPopup(const QuickPickKind kind,
                               std::shared_ptr<engine::Catalogue> catalogue, const QString& scope,
                               QWidget* parent)
    : QFrame(parent, Qt::Popup), kind_(kind), catalogue_(std::move(catalogue)) {
    const bool albums = kind_ == QuickPickKind::album;
    setObjectName(albums ? QStringLiteral("bench-quick-album") : QStringLiteral("bench-quick-track"));
    setAttribute(Qt::WA_DeleteOnClose);
    setFrameShape(QFrame::StyledPanel);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 8);
    layout->setSpacing(8);

    auto* row = new QHBoxLayout;
    input_ = new QLineEdit(this);
    input_->setObjectName(QStringLiteral("bench-quick-album-input"));
    input_->setPlaceholderText(albums ? tr("Album, artist or year — e.g. doors 1967")
                                      : tr("Title, artist, album or year — e.g. crystal doors"));
    input_->setClearButtonEnabled(true);
    input_->installEventFilter(this);
    auto input_font = input_->font();
    input_font.setPointSizeF(input_font.pointSizeF() * 1.1);
    input_->setFont(input_font);
    row->addWidget(input_, 1);
    // Which library is searched, so a remote tab's albums are not expected
    // from this computer's.
    auto* where = new QLabel(scope, this);
    where->setObjectName(QStringLiteral("bench-quick-album-scope"));
    where->setForegroundRole(QPalette::PlaceholderText);
    row->addWidget(where);
    layout->addLayout(row);

    results_ = new QListWidget(this);
    results_->setObjectName(QStringLiteral("bench-quick-album-results"));
    results_->setFrameShape(QFrame::NoFrame);
    results_->setItemDelegate(new AlbumRowDelegate(results_));
    results_->setUniformItemSizes(true);
    results_->setFocusPolicy(Qt::NoFocus);
    results_->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(results_, &QListWidget::itemActivated, this,
            [this] { choose(LocalLibraryAction::append); });
    layout->addWidget(results_, 1);

    status_ = new QLabel(idleText(), this);
    status_->setObjectName(QStringLiteral("bench-quick-album-status"));
    status_->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(status_);
    auto* keys = new QLabel(
        tr("Enter add · Shift+Enter replace and play · Ctrl+Enter play next · "
           "Ctrl+Shift+Enter add to Up Next · Alt+Enter new tab"),
        this);
    keys->setObjectName(QStringLiteral("bench-quick-album-keys"));
    keys->setForegroundRole(QPalette::PlaceholderText);
    keys->setWordWrap(true);
    auto small = keys->font();
    small.setPointSizeF(small.pointSizeF() * 0.88);
    keys->setFont(small);
    status_->setFont(small);
    layout->addWidget(keys);

    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(120);
    connect(input_, &QLineEdit::textChanged, debounce_, qOverload<>(&QTimer::start));
    connect(debounce_, &QTimer::timeout, this, &QuickPickPopup::search);
    connect(&watcher_, &QFutureWatcher<Found>::finished, this, &QuickPickPopup::showResults);
}

QuickPickPopup::~QuickPickPopup() {
    // The search holds the catalogue it was given; let it finish with it.
    watcher_.waitForFinished();
}

void QuickPickPopup::popUp(const QWidget* over) {
    const auto width = std::min(640, std::max(420, over->width() - 80));
    resize(width, 420);
    const auto top_left = over->mapToGlobal(QPoint{(over->width() - width) / 2, 90});
    move(top_left);
    show();
    input_->setFocus(Qt::PopupFocusReason);
    search();
}

void QuickPickPopup::search() {
    const auto words = input_->text().simplified();
    ++generation_;
    // Nothing typed: what came in most recently, newest first.
    const bool newest = words.isEmpty();
    // One search at a time: a newer text waits for it, then runs.
    if (watcher_.isRunning()) {
        pending_ = true;
        return;
    }
    status_->setText(tr("Searching…"));
    persistence::LibraryQuery query;
    query.kind = kind_ == QuickPickKind::album ? persistence::LibraryEntryKind::album
                                               : persistence::LibraryEntryKind::track;
    query.text = words.toStdString();
    query.limit = result_limit;
    query.newest_first = newest;
    watcher_.setFuture(QtConcurrent::run(
        [catalogue = catalogue_, query = std::move(query), generation = generation_, newest] {
            Found found;
            found.generation = generation;
            found.newest = newest;
            auto page = catalogue->query(query);
            if (!page) {
                found.error = QString::fromStdString(page.error().message);
                return found;
            }
            found.entries = std::move(page->entries);
            found.more = page->more;
            return found;
        }));
}

void QuickPickPopup::showResults() {
    auto found = watcher_.result();
    if (pending_) {
        pending_ = false;
        search();
        return;
    }
    if (found.generation != generation_) {
        return;
    }
    if (!found.error.isEmpty()) {
        status_->setText(found.error);
        return;
    }
    entries_ = std::move(found.entries);
    results_->clear();
    const bool albums = kind_ == QuickPickKind::album;
    for (const auto& entry : entries_) {
        // Album: who, when, how long. Track: who, from what, when.
        QStringList details;
        details << text(entry.artist);
        if (!albums && !entry.album.empty()) {
            details << text(entry.album);
        }
        if (!entry.date.empty()) {
            details << text(entry.date);
        }
        if (albums) {
            details << (entry.tracks == 1U ? tr("1 track") : tr("%1 tracks").arg(entry.tracks));
        }
        // Listed for being new, so it says how new.
        if (found.newest && entry.added > 0) {
            details << added_ago(entry.added);
        }
        const auto name = text(albums ? entry.album : (entry.title.empty() ? entry.label : entry.title));
        auto* item = new QListWidgetItem(results_);
        item->setData(name_role, name);
        item->setData(details_role, details.join(QStringLiteral(" · ")));
        item->setText(name + QStringLiteral(" — ") + details.join(QStringLiteral(" · ")));
    }
    if (!entries_.empty()) {
        results_->setCurrentRow(0);
    }
    const auto count = entries_.size();
    const auto noun = [albums](const std::size_t n) {
        return albums ? (n == 1U ? tr("1 album") : tr("%1 albums").arg(n))
                      : (n == 1U ? tr("1 track") : tr("%1 tracks").arg(n));
    };
    if (found.newest) {
        status_->setText(count == 0U ? idleText()
                                     : tr("Recently added — type to search the whole library"));
        return;
    }
    status_->setText(count == 0U ? (albums ? tr("No albums match.") : tr("No tracks match."))
                     : found.more ? tr("First %1 — type more to narrow them.").arg(noun(count))
                                  : noun(count));
}

QString QuickPickPopup::idleText() const {
    return kind_ == QuickPickKind::album ? tr("Type part of an album, its artist or its year.")
                                         : tr("Type part of a title, its artist, album or year.");
}

void QuickPickPopup::choose(const LocalLibraryAction action) {
    const auto row = results_->currentRow();
    if (row < 0 || row >= static_cast<int>(entries_.size())) {
        return;
    }
    emit chosen({entries_[static_cast<std::size_t>(row)]}, action);
    close();
}

bool QuickPickPopup::eventFilter(QObject* watched, QEvent* event) {
    if (watched != input_ || event->type() != QEvent::KeyPress) {
        return QFrame::eventFilter(watched, event);
    }
    const auto* key = static_cast<QKeyEvent*>(event);
    const auto move = [this](const int by) {
        if (results_->count() == 0) {
            return true;
        }
        results_->setCurrentRow(
            std::clamp(results_->currentRow() + by, 0, results_->count() - 1));
        return true;
    };
    switch (key->key()) {
    case Qt::Key_Down:
        return move(1);
    case Qt::Key_Up:
        return move(-1);
    case Qt::Key_PageDown:
        return move(8);
    case Qt::Key_PageUp:
        return move(-8);
    case Qt::Key_Escape:
        close();
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter: {
        const auto modifiers = key->modifiers() & ~Qt::KeypadModifier;
        const auto action = modifiers == (Qt::ControlModifier | Qt::ShiftModifier)
                                ? LocalLibraryAction::request_end
                            : modifiers == Qt::ControlModifier ? LocalLibraryAction::request_next
                            : modifiers == Qt::ShiftModifier   ? LocalLibraryAction::replace
                            : modifiers == Qt::AltModifier     ? LocalLibraryAction::new_list
                                                               : LocalLibraryAction::append;
        choose(action);
        return true;
    }
    default:
        return QFrame::eventFilter(watched, event);
    }
}

} // namespace trackknife::bench
