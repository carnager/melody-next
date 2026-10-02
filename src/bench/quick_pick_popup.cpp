// SPDX-License-Identifier: GPL-3.0-only

#include "bench/quick_pick_popup.hpp"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace trackknife::bench {
namespace {

constexpr int row_height = 30;
constexpr int name_role = Qt::UserRole + 1;
constexpr int details_role = Qt::UserRole + 2;

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

} // namespace

QuickPickPopup::QuickPickPopup(const QuickPickKind kind,
                               std::shared_ptr<engine::Catalogue> catalogue, const QString& scope,
                               QWidget* parent)
    : QFrame(parent, Qt::Popup),
      session_(new QuickPickSession(kind, std::move(catalogue), scope, this)) {
    const bool albums = kind == QuickPickKind::album;
    setObjectName(albums ? QStringLiteral("bench-quick-album") : QStringLiteral("bench-quick-track"));
    setAttribute(Qt::WA_DeleteOnClose);
    setFrameShape(QFrame::StyledPanel);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 8);
    layout->setSpacing(8);

    auto* row = new QHBoxLayout;
    input_ = new QLineEdit(this);
    input_->setObjectName(QStringLiteral("bench-quick-album-input"));
    input_->setPlaceholderText(session_->placeholder());
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
            [this] { session_->choose(results_->currentRow(), LocalLibraryAction::append); });
    layout->addWidget(results_, 1);

    status_ = new QLabel(session_->status(), this);
    status_->setObjectName(QStringLiteral("bench-quick-album-status"));
    status_->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(status_);
    auto* keys = new QLabel(QuickPickSession::keysText(), this);
    keys->setObjectName(QStringLiteral("bench-quick-album-keys"));
    keys->setForegroundRole(QPalette::PlaceholderText);
    keys->setWordWrap(true);
    auto small = keys->font();
    small.setPointSizeF(small.pointSizeF() * 0.88);
    keys->setFont(small);
    status_->setFont(small);
    layout->addWidget(keys);

    connect(input_, &QLineEdit::textChanged, session_, &QuickPickSession::setText);
    connect(session_, &QuickPickSession::changed, this, &QuickPickPopup::showResults);
    connect(session_, &QuickPickSession::chosen, this,
            [this](std::vector<persistence::LibraryEntry> picked, LocalLibraryAction action) {
                emit chosen(std::move(picked), action);
                close();
            });
}

QuickPickPopup::~QuickPickPopup() = default;

void QuickPickPopup::popUp(const QWidget* over) {
    // Centred on the window, not on the part asked for: the track area sits
    // beside the side panels, and centring on it put the popup off the
    // window's middle. Its own width as laid out, which a minimum can widen.
    const auto* window = over->window();
    resize(std::min(640, std::max(420, window->width() - 80)), 420);
    const auto top_left = window->mapToGlobal(QPoint{(window->width() - width()) / 2, 90});
    move(top_left);
    show();
    input_->setFocus(Qt::PopupFocusReason);
    session_->search();
}

void QuickPickPopup::showResults() {
    status_->setText(session_->status());
    const auto& rows = session_->rows();
    // The same rows: the current one stays.
    bool same = results_->count() == static_cast<int>(rows.size());
    for (int row = 0; same && row < results_->count(); ++row) {
        same = results_->item(row)->data(name_role).toString() ==
                   rows[static_cast<std::size_t>(row)].name &&
               results_->item(row)->data(details_role).toString() ==
                   rows[static_cast<std::size_t>(row)].details;
    }
    if (same) {
        return;
    }
    results_->clear();
    for (const auto& found : rows) {
        auto* item = new QListWidgetItem(results_);
        item->setData(name_role, found.name);
        item->setData(details_role, found.details);
        item->setText(found.name + QStringLiteral(" — ") + found.details);
    }
    if (!rows.empty()) {
        results_->setCurrentRow(0);
    }
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
    case Qt::Key_Enter:
        session_->choose(results_->currentRow(), QuickPickSession::actionFor(key->modifiers()));
        return true;
    default:
        return QFrame::eventFilter(watched, event);
    }
}

} // namespace trackknife::bench
