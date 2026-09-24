// SPDX-License-Identifier: GPL-3.0-only

#include "bench/quick_album_popup.hpp"

#include "trackknife/engine/catalogue.hpp"

#include <QApplication>
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
constexpr int album_role = Qt::UserRole + 1;
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
        const auto album = index.data(album_role).toString();
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

} // namespace

QuickAlbumPopup::QuickAlbumPopup(std::shared_ptr<engine::Catalogue> catalogue,
                                 const QString& scope, QWidget* parent)
    : QFrame(parent, Qt::Popup), catalogue_(std::move(catalogue)) {
    setObjectName(QStringLiteral("bench-quick-album"));
    setAttribute(Qt::WA_DeleteOnClose);
    setFrameShape(QFrame::StyledPanel);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 8);
    layout->setSpacing(8);

    auto* row = new QHBoxLayout;
    input_ = new QLineEdit(this);
    input_->setObjectName(QStringLiteral("bench-quick-album-input"));
    input_->setPlaceholderText(tr("Album, artist or year — e.g. doors 1967"));
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

    status_ = new QLabel(tr("Type part of an album, its artist or its year."), this);
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
    connect(debounce_, &QTimer::timeout, this, &QuickAlbumPopup::search);
    connect(&watcher_, &QFutureWatcher<Found>::finished, this, &QuickAlbumPopup::showResults);
}

QuickAlbumPopup::~QuickAlbumPopup() {
    // The search holds the catalogue it was given; let it finish with it.
    watcher_.waitForFinished();
}

void QuickAlbumPopup::popUp(const QWidget* over) {
    const auto width = std::min(640, std::max(420, over->width() - 80));
    resize(width, 420);
    const auto top_left = over->mapToGlobal(QPoint{(over->width() - width) / 2, 90});
    move(top_left);
    show();
    input_->setFocus(Qt::PopupFocusReason);
}

void QuickAlbumPopup::search() {
    const auto words = input_->text().simplified();
    ++generation_;
    if (words.isEmpty()) {
        albums_.clear();
        results_->clear();
        status_->setText(tr("Type part of an album, its artist or its year."));
        return;
    }
    // One search at a time: a newer text waits for it, then runs.
    if (watcher_.isRunning()) {
        pending_ = true;
        return;
    }
    status_->setText(tr("Searching…"));
    persistence::LibraryQuery query;
    query.kind = persistence::LibraryEntryKind::album;
    query.text = words.toStdString();
    query.limit = result_limit;
    watcher_.setFuture(QtConcurrent::run(
        [catalogue = catalogue_, query = std::move(query), generation = generation_] {
            Found found;
            found.generation = generation;
            auto page = catalogue->query(query);
            if (!page) {
                found.error = QString::fromStdString(page.error().message);
                return found;
            }
            found.albums = std::move(page->entries);
            found.more = page->more;
            return found;
        }));
}

void QuickAlbumPopup::showResults() {
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
    albums_ = std::move(found.albums);
    results_->clear();
    for (const auto& album : albums_) {
        QStringList details;
        details << text(album.artist);
        if (!album.date.empty()) {
            details << text(album.date);
        }
        details << (album.tracks == 1U ? tr("1 track") : tr("%1 tracks").arg(album.tracks));
        auto* item = new QListWidgetItem(results_);
        item->setData(album_role, text(album.album));
        item->setData(details_role, details.join(QStringLiteral(" · ")));
        item->setText(text(album.album) + QStringLiteral(" — ") + details.join(QStringLiteral(" · ")));
    }
    if (!albums_.empty()) {
        results_->setCurrentRow(0);
    }
    status_->setText(albums_.empty() ? tr("No albums match.")
                     : found.more    ? tr("First %1 albums — type more to narrow them.")
                                        .arg(albums_.size())
                     : albums_.size() == 1U ? tr("1 album")
                                            : tr("%1 albums").arg(albums_.size()));
}

void QuickAlbumPopup::choose(const LocalLibraryAction action) {
    const auto row = results_->currentRow();
    if (row < 0 || row >= static_cast<int>(albums_.size())) {
        return;
    }
    emit chosen({albums_[static_cast<std::size_t>(row)]}, action);
    close();
}

bool QuickAlbumPopup::eventFilter(QObject* watched, QEvent* event) {
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
