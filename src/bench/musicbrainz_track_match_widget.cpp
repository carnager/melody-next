// SPDX-License-Identifier: GPL-3.0-only

#include "bench/musicbrainz_track_match_widget.hpp"
#include "workspace/identify_session.hpp"

#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QStyledItemDelegate>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <utility>

namespace trackknife::bench {
namespace {

// ADR-0324: the pairing as one board -- each album track beside the file on
// it, the release's discs headed, the files on no track last. Files are
// what is moved: a grip, a hand, lifted when picked; the tracks stay put,
// and the line between them says how well each pair agrees.

// What a board item is.
enum class Kind : std::uint8_t { disc, pair, unplaced };
constexpr int kind_role = Qt::UserRole + 1;
// A pair's row in the session; a disc heading's disc.
constexpr int row_role = Qt::UserRole + 2;
constexpr int disc_role = Qt::UserRole + 3;
// What the track side says, for whoever reads the board without seeing it.
constexpr int track_role = Qt::UserRole + 4;

QString mimeType() { return QStringLiteral("application/x-trackbench-match-row"); }

// The parts of a pair's row: the file, the line between, the track.
struct Zones {
    QRect file;
    QRect line;
    QRect track;
};

Zones zonesOf(const QRect& rect) {
    constexpr int line = 64;
    const auto side = std::max(0, (rect.width() - line) / 2);
    return Zones{
        .file = QRect{rect.left(), rect.top(), side, rect.height()}.adjusted(4, 3, 0, -3),
        .line = QRect{rect.left() + side, rect.top(), line, rect.height()},
        .track = QRect{rect.left() + side + line, rect.top(), rect.width() - side - line,
                       rect.height()}
                     .adjusted(0, 3, -4, -3)};
}

QColor blend(const QColor& base, const QColor& over, const int percent) {
    const auto mix = [percent](const int a, const int b) {
        return (a * (100 - percent) + b * percent) / 100;
    };
    return QColor{mix(base.red(), over.red()), mix(base.green(), over.green()),
                  mix(base.blue(), over.blue())};
}

// A warm colour for what is worth a look, readable on light and dark.
QColor warm(const QPalette& palette) {
    return palette.color(QPalette::Base).lightness() > 128 ? QColor{0xb4, 0x6a, 0x00}
                                                           : QColor{0xf0, 0xb0, 0x4a};
}

class MatchBoard;

class MatchDelegate final : public QStyledItemDelegate {
  public:
    MatchDelegate(TrackMatchSession* session, MatchBoard* board, QObject* parent)
        : QStyledItemDelegate(parent), session_(session), board_(board) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override {
        const auto line = QFontMetrics{option.font}.height();
        const auto kind = static_cast<Kind>(index.data(kind_role).toInt());
        return QSize{200, kind == Kind::pair ? line + 16 : line + 18};
    }

  private:
    void paintPair(QPainter* painter, const QStyleOptionViewItem& option,
                   const QModelIndex& index) const;
    void paintHeading(QPainter* painter, const QStyleOptionViewItem& option,
                      const QModelIndex& index) const;

    TrackMatchSession* session_;
    MatchBoard* board_;
};

// The board: every row of the session, headed by disc; a file put on a
// track by dragging it there, or by clicking it and then the track.
class MatchBoard final : public QListWidget {
  public:
    // Where a carried or held file would go now.
    struct Target {
        enum class Kind : std::uint8_t { none, track, between, unplace } kind{Kind::none};
        // The track's row; for `between`, the row the file would take.
        std::size_t row{0};
    };

    MatchBoard(TrackMatchSession* session, QWidget* parent)
        : QListWidget(parent), session_(session) {
        setObjectName(QStringLiteral("bench-musicbrainz-match-board"));
        setAccessibleName(QStringLiteral("Files paired with the release's tracks"));
        setItemDelegate(new MatchDelegate(session, this, this));
        setSelectionMode(QAbstractItemView::SingleSelection);
        setEditTriggers(QAbstractItemView::NoEditTriggers);
        setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setAcceptDrops(true);
        setDropIndicatorShown(false);
        setAutoScroll(true);
        setMouseTracking(true);
    }

    std::function<void(QString)> said;

    [[nodiscard]] std::optional<std::size_t> held() const { return held_; }
    [[nodiscard]] const Target& target() const { return target_; }
    [[nodiscard]] bool hovering(const std::size_t row) const {
        return hover_row_ && *hover_row_ == row;
    }
    [[nodiscard]] static std::optional<std::size_t> rowOf(const QListWidgetItem* item) {
        if (item == nullptr || static_cast<Kind>(item->data(kind_role).toInt()) != Kind::pair) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(item->data(row_role).toULongLong());
    }
    [[nodiscard]] QListWidgetItem* itemOfRow(const std::size_t row) const {
        for (int index = 0; index < count(); ++index) {
            if (const auto at = rowOf(item(index)); at && *at == row) {
                return item(index);
            }
        }
        return nullptr;
    }
    [[nodiscard]] int selectedRow() const {
        const auto row = rowOf(currentItem());
        return row ? static_cast<int>(*row) : -1;
    }
    // "Take the files of disc 1" -- on a disc with none of the album's
    // files, when another has some.
    [[nodiscard]] QString offerText(const std::size_t disc) const {
        std::size_t others = 0;
        std::size_t other = 0;
        for (const auto with : session_->discsWithFiles()) {
            if (with == disc) {
                return {};
            }
            ++others;
            other = with;
        }
        if (others == 0U || session_->discTrackCount(disc) == 0U) {
            return {};
        }
        return others == 1U ? tr("Take the files of disc %1").arg(other)
                            : tr("Take the files of another disc…");
    }
    [[nodiscard]] QRect offerRect(const QListWidgetItem* heading) const {
        const auto text = offerText(static_cast<std::size_t>(heading->data(disc_role).toULongLong()));
        if (text.isEmpty()) {
            return {};
        }
        const auto rect = visualItemRect(heading);
        const auto width = fontMetrics().horizontalAdvance(text) + 12;
        return QRect{zonesOf(rect).track.right() - width, rect.top(), width, rect.height()};
    }

    void drop() {
        held_.reset();
        target_ = {};
        viewport()->update();
        tell();
    }

    // Rebuilt from the session, scrolled where it was.
    void rebuild(const int select) {
        const auto scrolled = verticalScrollBar()->value();
        identity_ = QUuid::createUuid().toString();
        clear();
        std::size_t disc = 0;
        bool below = false;
        const auto& rows = session_->rows();
        for (std::size_t row = 0; row < rows.size(); ++row) {
            const auto& entry = rows[row];
            if (entry.disc != 0U && entry.disc != disc) {
                disc = entry.disc;
                auto* heading = new QListWidgetItem(entry.disc_heading, this);
                heading->setData(kind_role, static_cast<int>(Kind::disc));
                heading->setData(disc_role, static_cast<qulonglong>(disc));
                heading->setFlags(Qt::ItemIsEnabled);
            }
            if (entry.disc == 0U && !below) {
                below = true;
                auto* heading =
                    new QListWidgetItem(tr("On no track — these get no tags"), this);
                heading->setData(kind_role, static_cast<int>(Kind::unplaced));
                heading->setFlags(Qt::ItemIsEnabled);
            }
            auto* pair = new QListWidgetItem(entry.gap ? QString{} : entry.file, this);
            pair->setData(kind_role, static_cast<int>(Kind::pair));
            pair->setData(row_role, static_cast<qulonglong>(row));
            pair->setData(track_role, entry.track);
            QStringList tip;
            if (!entry.gap) {
                tip << entry.file_tool_tip;
            }
            tip << entry.track;
            if (!entry.pairing_tool_tip.isEmpty()) {
                tip << entry.pairing_tool_tip;
            }
            if (entry.delta_seconds && entry.doubtful) {
                tip << tr("The file is %1 s %2 than the track")
                           .arg(std::abs(*entry.delta_seconds))
                           .arg(*entry.delta_seconds > 0 ? tr("longer") : tr("shorter"));
            }
            pair->setToolTip(tip.join(QLatin1Char('\n')));
        }
        verticalScrollBar()->setValue(scrolled);
        if (select >= 0) {
            if (auto* chosen = itemOfRow(static_cast<std::size_t>(select))) {
                setCurrentItem(chosen);
                scrollToItem(chosen);
            }
        }
        held_.reset();
        target_ = {};
        tell();
    }

    // A drag's payload from row `row`, as the board's own: one made before
    // the board was last rebuilt is refused.
    [[nodiscard]] QMimeData* payload(const std::size_t row) const {
        auto* mime = new QMimeData;
        mime->setData(mimeType(), identity_.toUtf8() + ':' + QByteArray::number(row));
        return mime;
    }
    [[nodiscard]] std::optional<std::size_t> sourceRow(const QMimeData* mime) const {
        const auto carried = mime->data(mimeType());
        const auto prefix = identity_.toUtf8() + ':';
        if (!carried.startsWith(prefix)) {
            return std::nullopt;
        }
        bool valid = false;
        const auto row = carried.sliced(prefix.size()).toULongLong(&valid);
        if (!valid || row >= session_->rows().size()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(row);
    }

  protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override {
        const auto row = items.size() == 1 ? rowOf(items.front()) : std::nullopt;
        return row ? payload(*row) : nullptr;
    }
    QStringList mimeTypes() const override { return {mimeType()}; }

    void mousePressEvent(QMouseEvent* event) override {
        press_.reset();
        const auto position = event->position().toPoint();
        auto* at = itemAt(position);
        const auto kind = at != nullptr ? static_cast<Kind>(at->data(kind_role).toInt()) : Kind::pair;
        if (at != nullptr && kind == Kind::disc && event->button() == Qt::LeftButton &&
            offerRect(at).contains(position)) {
            offer(static_cast<std::size_t>(at->data(disc_role).toULongLong()),
                  event->globalPosition().toPoint());
            return;
        }
        if (at != nullptr && kind == Kind::unplaced && held_ &&
            event->button() == Qt::LeftButton) {
            const auto from = static_cast<int>(*held_);
            drop();
            if (session_->canUnmatch(from)) {
                session_->unmatch(from);
            }
            return;
        }
        if (const auto row = rowOf(at); row && event->button() == Qt::LeftButton) {
            press_ = Press{.row = *row, .at = position, .on_file = onFile(*at, position)};
        }
        QListWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        const auto position = event->position().toPoint();
        if (press_ && press_->on_file && (event->buttons() & Qt::LeftButton) != 0 &&
            (position - press_->at).manhattanLength() >= QApplication::startDragDistance()) {
            carry(press_->row);
            return;
        }
        // A hand over a file: it is what is moved.
        auto* at = itemAt(position);
        const auto row = rowOf(at);
        const bool file = row && hasFile(*row) && onFile(*at, position);
        viewport()->setCursor(file ? Qt::OpenHandCursor : Qt::ArrowCursor);
        const auto hovered = file ? row : std::nullopt;
        if (hovered != hover_row_) {
            hover_row_ = hovered;
            viewport()->update();
        }
        if (held_) {
            aim(position, *held_);
        }
        QListWidget::mouseMoveEvent(event);
    }

    void leaveEvent(QEvent* event) override {
        hover_row_.reset();
        viewport()->update();
        QListWidget::leaveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        const auto press = std::exchange(press_, std::nullopt);
        QListWidget::mouseReleaseEvent(event);
        if (!press || event->button() != Qt::LeftButton) {
            return;
        }
        const auto position = event->position().toPoint();
        const auto row = rowOf(itemAt(position));
        if (!row || *row != press->row) {
            return;
        }
        if (press->on_file) {
            // A file clicked is picked up; clicked again, put back.
            if (held_ && *held_ == *row) {
                drop();
            } else if (hasFile(*row)) {
                held_ = *row;
                aim(position, *row);
                viewport()->update();
            }
            return;
        }
        // A track clicked with a file picked up: the file goes there.
        if (held_ && *held_ != *row && *row < session_->trackCount()) {
            const auto from = *held_;
            drop();
            session_->placeOn(from, *row);
        }
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape && held_) {
            drop();
            event->accept();
            return;
        }
        QListWidget::keyPressEvent(event);
    }

    void dragEnterEvent(QDragEnterEvent* event) override {
        if (sourceRow(event->mimeData())) {
            event->acceptProposedAction();
        } else {
            event->ignore();
        }
    }
    void dragMoveEvent(QDragMoveEvent* event) override {
        const auto source = sourceRow(event->mimeData());
        if (!source) {
            event->ignore();
            return;
        }
        // The list scrolls at its edges while a file is carried.
        QListWidget::dragMoveEvent(event);
        aim(event->position().toPoint(), *source);
        if (target_.kind == Target::Kind::none) {
            event->ignore();
            return;
        }
        event->acceptProposedAction();
    }
    void dragLeaveEvent(QDragLeaveEvent* event) override {
        target_ = {};
        viewport()->update();
        QListWidget::dragLeaveEvent(event);
    }
    void dropEvent(QDropEvent* event) override {
        const auto source = sourceRow(event->mimeData());
        if (!source) {
            event->ignore();
            return;
        }
        aim(event->position().toPoint(), *source);
        const auto target = std::exchange(target_, Target{});
        held_.reset();
        if (target.kind == Target::Kind::none) {
            event->ignore();
            tell();
            return;
        }
        event->setDropAction(Qt::MoveAction);
        event->accept();
        switch (target.kind) {
        case Target::Kind::track:
            session_->placeOn(*source, target.row);
            break;
        case Target::Kind::between:
            session_->moveFile(*source, target.row > *source ? target.row - 1U : target.row);
            break;
        case Target::Kind::unplace:
            session_->unmatch(static_cast<int>(*source));
            break;
        case Target::Kind::none:
            break;
        }
    }

  private:
    struct Press {
        std::size_t row{0};
        QPoint at;
        bool on_file{false};
    };

    [[nodiscard]] bool hasFile(const std::size_t row) const {
        const auto& rows = session_->rows();
        return row < rows.size() && !rows[row].gap;
    }
    [[nodiscard]] bool onFile(const QListWidgetItem& at, const QPoint& position) const {
        return position.x() < zonesOf(visualItemRect(&at)).line.center().x();
    }

    // Where a file from `source` would go, the pointer at `position`.
    void aim(const QPoint& position, const std::size_t source) {
        Target target;
        auto* at = itemAt(position);
        const auto kind = at != nullptr ? static_cast<Kind>(at->data(kind_role).toInt()) : Kind::pair;
        const bool can_unplace = session_->canUnmatch(static_cast<int>(source));
        if (at == nullptr || kind == Kind::unplaced) {
            // Below everything, or on the files on no track: left on none.
            if (can_unplace && (at != nullptr || count() == 0 ||
                                position.y() > visualItemRect(item(count() - 1)).bottom())) {
                target.kind = Target::Kind::unplace;
            }
        } else if (const auto row = rowOf(at)) {
            const auto zones = zonesOf(visualItemRect(at));
            if (position.x() >= zones.line.center().x()) {
                if (*row < session_->trackCount() && *row != source) {
                    target = {.kind = Target::Kind::track, .row = *row};
                } else if (*row >= session_->trackCount() && can_unplace) {
                    target.kind = Target::Kind::unplace;
                }
            } else {
                auto boundary = *row;
                if (position.y() > visualItemRect(at).center().y()) {
                    ++boundary;
                }
                if (boundary != source && boundary != source + 1U) {
                    target = {.kind = Target::Kind::between, .row = boundary};
                }
            }
        }
        if (target.kind != target_.kind || target.row != target_.row) {
            target_ = target;
            viewport()->update();
        }
        tell(source);
    }

    // What the board would do now, or how it is used.
    void tell(const std::optional<std::size_t> source = std::nullopt) {
        if (!said) {
            return;
        }
        const auto& rows = session_->rows();
        const auto moving = source ? source : held_;
        if (!moving || *moving >= rows.size()) {
            said(tr("Drag a file onto a track — or click it, then the track. Between files it "
                    "moves the files below down a track; below the tracks it gets no tags."));
            return;
        }
        const auto file = rows[*moving].file;
        switch (target_.kind) {
        case Target::Kind::track: {
            const auto& there = rows[target_.row];
            said(there.gap ? tr("%1 → %2").arg(file, there.track)
                           : tr("%1 → %2, swapping with %3").arg(file, there.track, there.file));
            return;
        }
        case Target::Kind::between:
            said(tr("%1 goes here; the files below move down a track").arg(file));
            return;
        case Target::Kind::unplace:
            said(tr("%1 goes on no track: it gets no tags").arg(file));
            return;
        case Target::Kind::none:
            break;
        }
        said(held_ ? tr("%1 picked up — click the track it belongs on, or press Esc").arg(file)
                   : tr("Carrying %1").arg(file));
    }

    void carry(const std::size_t row) {
        press_.reset();
        if (!hasFile(row)) {
            return;
        }
        held_.reset();
        QDrag drag{this};
        drag.setMimeData(payload(row));
        {
            // Only the file's name is carried, see-through, so the track it
            // is carried over shows beneath it.
            const auto name = session_->rows()[row].file;
            const auto metrics = fontMetrics();
            const auto ratio = devicePixelRatioF();
            const QSize size{metrics.horizontalAdvance(name) + 20, metrics.height() + 10};
            QPixmap carried{size * ratio};
            carried.setDevicePixelRatio(ratio);
            carried.fill(Qt::transparent);
            {
                QPainter painter{&carried};
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setOpacity(0.8);
                QPainterPath shape;
                shape.addRoundedRect(QRectF{QPointF{0.5, 0.5}, QSizeF{size}}.adjusted(0, 0, -1, -1),
                                     4, 4);
                painter.fillPath(shape, palette().color(QPalette::Base));
                painter.setPen(QPen{palette().color(QPalette::Highlight), 1.0});
                painter.drawPath(shape);
                painter.setFont(font());
                painter.setPen(palette().color(QPalette::Text));
                painter.drawText(QRect{QPoint{0, 0}, size}, Qt::AlignCenter, name);
            }
            drag.setPixmap(carried);
            drag.setHotSpot(QPoint{-12, size.height() / 2});
        }
        viewport()->setCursor(Qt::ClosedHandCursor);
        drag.exec(Qt::MoveAction);
        viewport()->setCursor(Qt::ArrowCursor);
        target_ = {};
        viewport()->update();
        tell();
    }

    void offer(const std::size_t disc, const QPoint& where) {
        std::vector<std::size_t> others;
        for (const auto with : session_->discsWithFiles()) {
            if (with != disc) {
                others.push_back(with);
            }
        }
        if (others.size() == 1U) {
            session_->moveDisc(others.front(), disc);
            return;
        }
        QMenu menu{this};
        for (const auto other : others) {
            menu.addAction(tr("The files of disc %1").arg(other), this,
                           [this, other, disc] { session_->moveDisc(other, disc); });
        }
        menu.exec(where);
    }

    TrackMatchSession* session_;
    QString identity_{QUuid::createUuid().toString()};
    std::optional<std::size_t> held_;
    std::optional<std::size_t> hover_row_;
    std::optional<Press> press_;
    Target target_;
};

void MatchDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                          const QModelIndex& index) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    if (static_cast<Kind>(index.data(kind_role).toInt()) == Kind::pair) {
        paintPair(painter, option, index);
    } else {
        paintHeading(painter, option, index);
    }
    painter->restore();
}

void MatchDelegate::paintHeading(QPainter* painter, const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const {
    const auto& palette = option.palette;
    const auto kind = static_cast<Kind>(index.data(kind_role).toInt());
    const auto zones = zonesOf(option.rect);
    // A disc heads its tracks, on their side; the files on no track are
    // headed on theirs.
    const auto area = (kind == Kind::disc ? zones.track : zones.file).adjusted(4, 0, 0, 0);
    auto font = option.font;
    font.setWeight(QFont::DemiBold);
    painter->setFont(font);
    painter->setPen(palette.color(QPalette::PlaceholderText));
    painter->drawText(QRect{area.left(), option.rect.top() + 6, area.width(),
                            option.rect.height() - 8},
                      Qt::AlignLeft | Qt::AlignVCenter, index.data().toString());
    painter->setPen(
        QPen{blend(palette.color(QPalette::Base), palette.color(QPalette::Text), 18), 1.0});
    painter->drawLine(area.left(), option.rect.bottom() - 1, area.right(),
                      option.rect.bottom() - 1);
    if (kind == Kind::disc) {
        const auto* item = board_->item(index.row());
        if (const auto offer = board_->offerRect(item); !offer.isEmpty()) {
            painter->setFont(option.font);
            painter->setPen(palette.color(QPalette::Link));
            painter->drawText(
                offer.adjusted(0, 6, -4, -2), Qt::AlignRight | Qt::AlignVCenter,
                board_->offerText(static_cast<std::size_t>(index.data(disc_role).toULongLong())));
        }
    }
}

void MatchDelegate::paintPair(QPainter* painter, const QStyleOptionViewItem& option,
                              const QModelIndex& index) const {
    const auto row = static_cast<std::size_t>(index.data(row_role).toULongLong());
    const auto& rows = session_->rows();
    if (row >= rows.size()) {
        return;
    }
    const auto& entry = rows[row];
    const auto& palette = option.palette;
    const auto base = palette.color(QPalette::Base);
    const auto text = palette.color(QPalette::Text);
    const auto quiet = palette.color(QPalette::PlaceholderText);
    const auto accent = palette.color(QPalette::Highlight);
    const auto amber = warm(palette);
    const auto zones = zonesOf(option.rect);
    const QFontMetrics metrics{option.font};
    const auto& target = board_->target();
    const bool held = board_->held() && *board_->held() == row;
    const auto length_width = metrics.horizontalAdvance(QStringLiteral("00:00")) + 10;

    // The row chosen, quietly; one worth a look, warmly.
    if ((option.state & QStyle::State_Selected) != 0) {
        painter->fillRect(option.rect, blend(base, accent, 14));
    } else if (entry.doubtful) {
        painter->fillRect(option.rect, blend(base, amber, 8));
    }

    // The file: a raised tile with a grip -- or, where the track has none,
    // an empty slot.
    const auto tile = zones.file;
    QPainterPath tile_shape;
    tile_shape.addRoundedRect(QRectF{tile}.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    if (!entry.gap) {
        painter->fillPath(tile_shape, entry.other ? base
                                                  : blend(base, text,
                                                          held || board_->hovering(row) ? 10 : 5));
        painter->setPen(QPen{held ? accent : blend(base, text, 24), held ? 2.0 : 1.0});
        painter->drawPath(tile_shape);
        painter->setPen(Qt::NoPen);
        painter->setBrush(blend(base, text, 42));
        const auto middle = static_cast<double>(tile.center().y());
        for (int column = 0; column < 2; ++column) {
            for (int dot = -1; dot <= 1; ++dot) {
                painter->drawEllipse(QPointF{tile.left() + 9.0 + column * 4.0, middle + dot * 4.0},
                                     1.2, 1.2);
            }
        }
        painter->setBrush(Qt::NoBrush);
        auto file_font = option.font;
        file_font.setItalic(entry.other);
        painter->setFont(file_font);
        const auto name_rect = tile.adjusted(24, 0, -length_width, 0);
        painter->setPen(entry.other ? quiet : text);
        painter->drawText(name_rect, Qt::AlignLeft | Qt::AlignVCenter,
                          metrics.elidedText(entry.file, Qt::ElideMiddle, name_rect.width()));
        painter->setFont(option.font);
        painter->setPen(quiet);
        painter->drawText(tile.adjusted(0, 0, -8, 0), Qt::AlignRight | Qt::AlignVCenter,
                          entry.length);
    } else {
        painter->setPen(QPen{blend(base, text, 28), 1.0, Qt::DashLine});
        painter->drawPath(tile_shape);
        painter->setPen(quiet);
        painter->drawText(tile.adjusted(12, 0, -8, 0), Qt::AlignLeft | Qt::AlignVCenter,
                          QObject::tr("no file"));
    }

    // Where a carried file would go between files: a line across them.
    if (target.kind == MatchBoard::Target::Kind::between &&
        (target.row == row || target.row == row + 1U)) {
        const auto y = target.row == row ? option.rect.top() + 1 : option.rect.bottom();
        painter->setPen(QPen{accent, 2.0});
        painter->drawLine(zones.file.left(), y, zones.file.right(), y);
    }

    // The line between: how well the pair agrees.
    const bool on_track = row < session_->trackCount();
    if (on_track) {
        const auto y = zones.line.center().y();
        const auto left = zones.line.left() + 5;
        const auto right = zones.line.right() - 5;
        if (entry.gap) {
            painter->setPen(QPen{blend(base, text, 25), 1.0, Qt::DotLine});
            painter->drawLine(left, y, right, y);
        } else {
            const auto colour = entry.doubtful ? amber : blend(base, text, 45);
            painter->setPen(QPen{colour, entry.doubtful ? 2.0 : 1.5});
            painter->drawLine(left, y, right, y);
            painter->setPen(Qt::NoPen);
            painter->setBrush(colour);
            for (const auto x : {left, right}) {
                painter->drawEllipse(QPointF{static_cast<double>(x), static_cast<double>(y)}, 2.5,
                                     2.5);
            }
            painter->setBrush(Qt::NoBrush);
            if (entry.doubtful && entry.delta_seconds) {
                auto small = option.font;
                if (small.pointSizeF() > 0) {
                    small.setPointSizeF(small.pointSizeF() * 0.85);
                }
                painter->setFont(small);
                painter->setPen(amber);
                painter->drawText(QRect{zones.line.left(), option.rect.top(), zones.line.width(),
                                        y - option.rect.top() - 2},
                                  Qt::AlignHCenter | Qt::AlignBottom,
                                  QStringLiteral("%1%2 s")
                                      .arg(*entry.delta_seconds > 0 ? QStringLiteral("+")
                                                                    : QString{})
                                      .arg(*entry.delta_seconds));
                painter->setFont(option.font);
            }
        }
    }

    // The track: recessed, where it stays.
    const auto slot = zones.track;
    if (!on_track) {
        auto quiet_font = option.font;
        quiet_font.setItalic(true);
        painter->setFont(quiet_font);
        painter->setPen(quiet);
        painter->drawText(slot.adjusted(10, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter,
                          metrics.elidedText(entry.other ? entry.track.mid(2)
                                                         : QObject::tr("no tags"),
                                             Qt::ElideRight, slot.width() - 10));
        return;
    }
    QPainterPath slot_shape;
    slot_shape.addRoundedRect(QRectF{slot}.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    if (target.kind == MatchBoard::Target::Kind::track && target.row == row) {
        painter->fillPath(slot_shape, blend(base, accent, 20));
        painter->setPen(QPen{accent, 2.0});
    } else {
        painter->fillPath(slot_shape, blend(base, QColor{Qt::black}, 3));
        painter->setPen(QPen{blend(base, text, entry.gap ? 30 : 14), 1.0,
                             entry.gap ? Qt::DashLine : Qt::SolidLine});
    }
    painter->drawPath(slot_shape);
    const auto number_width = metrics.horizontalAdvance(QStringLiteral("00")) + 10;
    painter->setPen(quiet);
    painter->drawText(QRect{slot.left(), slot.top(), number_width, slot.height()},
                      Qt::AlignRight | Qt::AlignVCenter, QString::number(entry.number));
    const auto title_rect = slot.adjusted(number_width + 10, 0, -length_width, 0);
    painter->setPen(text);
    painter->drawText(title_rect, Qt::AlignLeft | Qt::AlignVCenter,
                      metrics.elidedText(entry.title, Qt::ElideRight, title_rect.width()));
    painter->setPen(entry.doubtful ? amber : quiet);
    painter->drawText(slot.adjusted(0, 0, -8, 0), Qt::AlignRight | Qt::AlignVCenter,
                      entry.track_length);
}

// A view over a TrackMatchSession: the board, what it would do, and the
// buttons.
class TrackMatchWidget final : public QWidget {
  public:
    // Embedded, it is the board and its buttons only: the window around it
    // names the release, says how far the pairing is, and stages.
    TrackMatchWidget(TrackMatchSession* session, std::function<void()> back, QWidget* parent,
                     const bool embedded = false)
        : QWidget(parent), session_(session) {
        setObjectName(QStringLiteral("bench-musicbrainz-track-match"));
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        if (!embedded) {
            auto* heading = new QLabel(session_->heading(), this);
            heading->setTextFormat(Qt::PlainText);
            heading->setWordWrap(true);
            layout->addWidget(heading);
        }
        board_ = new MatchBoard(session_, this);
        layout->addWidget(board_, 1);
        hint_ = new QLabel(this);
        hint_->setObjectName(QStringLiteral("bench-musicbrainz-match-hint"));
        hint_->setTextFormat(Qt::PlainText);
        hint_->setForegroundRole(QPalette::PlaceholderText);
        hint_->setWordWrap(true);
        board_->said = [this](const QString& text) { hint_->setText(text); };
        layout->addWidget(hint_);

        auto* actions = new QHBoxLayout;
        const auto button = [this, actions](const QString& label, const QString& name) {
            auto* result = new QPushButton(label, this);
            result->setObjectName(name);
            result->setAutoDefault(false);
            actions->addWidget(result);
            return result;
        };
        sort_ = button(tr("Match by filename"), QStringLiteral("bench-musicbrainz-match-sort"));
        sort_->setToolTip(tr("Pair files in natural filename order with the album tracks. This "
                             "replaces the current pairings."));
        order_ = button(tr("Reset file order"), QStringLiteral("bench-musicbrainz-match-order"));
        order_->setToolTip(
            tr("Pair files in their original selection order with the album tracks."));
        actions->addStretch(1);
        up_ = button(tr("Move file up"), QStringLiteral("bench-musicbrainz-match-up"));
        down_ = button(tr("Move file down"), QStringLiteral("bench-musicbrainz-match-down"));
        up_->setToolTip(tr("Swap the chosen file with the one a track above (Alt+Up)"));
        down_->setToolTip(tr("Swap the chosen file with the one a track below (Alt+Down)"));
        unmatch_ = button(tr("Leave unmatched"), QStringLiteral("bench-musicbrainz-match-unmatch"));
        unmatch_->setToolTip(
            tr("Take this file off its track, leaving a gap: it gets no tags (U)"));
        layout->addLayout(actions);
        status_ = new QLabel(this);
        status_->setObjectName(QStringLiteral("bench-musicbrainz-match-status"));
        status_->setWordWrap(true);
        layout->addWidget(status_);
        auto* footer = new QHBoxLayout;
        auto* back_button = new QPushButton(tr("Back to releases"), this);
        back_button->setObjectName(QStringLiteral("bench-musicbrainz-match-back"));
        connect(back_button, &QPushButton::clicked, this, [back = std::move(back)] {
            if (back) {
                back();
            }
        });
        footer->addWidget(back_button);
        footer->addStretch();
        stage_ = new QPushButton(tr("Stage matches"), this);
        stage_->setObjectName(QStringLiteral("bench-musicbrainz-match-stage"));
        stage_->setToolTip(tr("Confirm the displayed assignments and stage tags for matched files. "
                              "Unmatched files are untouched; Apply writes the draft later."));
        footer->addWidget(stage_);
        layout->addLayout(footer);
        if (embedded) {
            status_->hide();
            back_button->hide();
            stage_->hide();
            embedded_ = true;
        }

        connect(board_, &QListWidget::currentItemChanged, this, [this] { updateButtons(); });
        connect(up_, &QPushButton::clicked, this,
                [this] { session_->move(board_->selectedRow(), -1); });
        connect(down_, &QPushButton::clicked, this,
                [this] { session_->move(board_->selectedRow(), 1); });
        for (const auto& entry : {std::pair{QKeySequence{Qt::ALT | Qt::Key_Up}, -1},
                                  std::pair{QKeySequence{Qt::ALT | Qt::Key_Down}, 1}}) {
            auto* shortcut = new QShortcut(entry.first, board_);
            shortcut->setContext(Qt::WidgetWithChildrenShortcut);
            connect(shortcut, &QShortcut::activated, this, [this, direction = entry.second] {
                session_->move(board_->selectedRow(), direction);
            });
        }
        connect(unmatch_, &QPushButton::clicked, this,
                [this] { session_->unmatch(board_->selectedRow()); });
        // ADR-0261: U leaves the file unmatched, from the keyboard.
        auto* unmatch_key = new QShortcut(QKeySequence{Qt::Key_U}, board_);
        unmatch_key->setContext(Qt::WidgetWithChildrenShortcut);
        connect(unmatch_key, &QShortcut::activated, this, [this] {
            if (session_->canUnmatch(board_->selectedRow())) {
                session_->unmatch(board_->selectedRow());
            }
        });
        connect(sort_, &QPushButton::clicked, this, [this] { session_->resetOrder(true); });
        connect(order_, &QPushButton::clicked, this, [this] { session_->resetOrder(false); });
        connect(stage_, &QPushButton::clicked, session_, &TrackMatchSession::stage);
        connect(session_, &TrackMatchSession::changed, this, &TrackMatchWidget::refresh);
        refresh(-1);
    }

  private:
    void updateButtons() {
        const auto row = board_->selectedRow();
        up_->setEnabled(session_->canMoveUp(row));
        down_->setEnabled(session_->canMoveDown(row));
        unmatch_->setEnabled(session_->canUnmatch(row));
        if (embedded_) {
            for (auto* file_button : {up_, down_, unmatch_}) {
                file_button->setVisible(row >= 0);
            }
        }
        sort_->setEnabled(session_->ready());
        order_->setEnabled(session_->ready());
        stage_->setEnabled(session_->canStage());
    }
    void refresh(const int selected) {
        board_->rebuild(selected);
        status_->setText(session_->status());
        updateButtons();
    }

    TrackMatchSession* session_;
    MatchBoard* board_;
    QLabel* hint_;
    QPushButton* up_;
    QPushButton* down_;
    QPushButton* sort_;
    QPushButton* unmatch_;
    QPushButton* order_;
    QPushButton* stage_;
    QLabel* status_;
    bool embedded_{false};
};

} // namespace

QWidget* createMusicBrainzTrackMatchWidget(
    musicbrainz::Release release, std::vector<musicbrainz::LocalTrackDescriptor> local_tracks,
    std::vector<QString> local_paths, std::vector<std::size_t> item_indexes,
    std::function<void(metadata::MetadataProposalSet)> accepted, std::function<void()> back,
    QWidget* parent) {
    auto* session = new TrackMatchSession(std::move(release), std::move(local_tracks),
                                          std::move(local_paths), std::move(item_indexes));
    auto* widget = new TrackMatchWidget(session, std::move(back), parent);
    session->setParent(widget);
    QObject::connect(session, &TrackMatchSession::accepted, widget,
                     [accepted = std::move(accepted)](metadata::MetadataProposalSet proposals) {
                         if (accepted) {
                             accepted(std::move(proposals));
                         }
                     });
    return widget;
}

QWidget* createMusicBrainzTrackMatchView(TrackMatchSession* session, std::function<void()> back,
                                         QWidget* parent) {
    return new TrackMatchWidget(session, std::move(back), parent);
}

QWidget* createMusicBrainzTrackMatchPanes(TrackMatchSession* session, QWidget* parent) {
    return new TrackMatchWidget(session, {}, parent, true);
}

} // namespace trackknife::bench
