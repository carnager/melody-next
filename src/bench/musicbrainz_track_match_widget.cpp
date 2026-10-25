// SPDX-License-Identifier: GPL-3.0-only

#include "bench/musicbrainz_track_match_widget.hpp"
#include "workspace/identify_session.hpp"

#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
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
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <optional>
#include <utility>

namespace trackknife::bench {
namespace {

// ADR-0330: pairing as Picard does it. At the right the release's tracks,
// each with the file on it -- coloured by how well it fits -- or an empty
// slot; at the left the files on no track, gone from there once placed. A
// file is dropped on a track, or picked up and its track clicked; dropped
// back at the left it is on none.

// What a list item is.
enum class Kind : std::uint8_t { disc, pair };
constexpr int kind_role = Qt::UserRole + 1;
// A pair's row in the session; a disc heading's disc.
constexpr int row_role = Qt::UserRole + 2;
constexpr int disc_role = Qt::UserRole + 3;
// What the track side says, for whoever reads the board without seeing it.
constexpr int track_role = Qt::UserRole + 4;

QString mimeType() { return QStringLiteral("application/x-trackbench-match-row"); }

QColor blend(const QColor& base, const QColor& over, const int percent) {
    const auto mix = [percent](const int a, const int b) {
        return (a * (100 - percent) + b * percent) / 100;
    };
    return QColor{mix(base.red(), over.red()), mix(base.green(), over.green()),
                  mix(base.blue(), over.blue())};
}

[[nodiscard]] bool dark(const QPalette& palette) {
    return palette.color(QPalette::Base).lightness() <= 128;
}

// Green, orange, red: how well a file fits its track, readable on light and
// dark.
[[nodiscard]] QColor fitColour(const musicbrainz::PairFit fit, const QPalette& palette) {
    switch (fit) {
    case musicbrainz::PairFit::good:
        return dark(palette) ? QColor{0x5c, 0xc3, 0x6b} : QColor{0x2e, 0x8b, 0x3a};
    case musicbrainz::PairFit::doubtful:
        return dark(palette) ? QColor{0xf0, 0xb0, 0x4a} : QColor{0xb4, 0x6a, 0x00};
    case musicbrainz::PairFit::poor:
        return dark(palette) ? QColor{0xef, 0x6b, 0x6b} : QColor{0xc0, 0x39, 0x2b};
    }
    return palette.color(QPalette::Text);
}

// Where a carried or held file would go.
struct Target {
    enum class Kind : std::uint8_t { none, track, loose } kind{Kind::none};
    std::size_t row{0};
    friend bool operator==(const Target&, const Target&) = default;
};

// What both sides share: the session, the file picked up, where it would go,
// and a drag's payload -- refused once the rows have changed under it.
class Pairing final : public QObject {
  public:
    explicit Pairing(TrackMatchSession* session, QObject* parent)
        : QObject(parent), session_(session) {}

    TrackMatchSession* session() const { return session_; }
    std::function<void(QString)> said;
    std::function<void()> repaint;

    [[nodiscard]] std::optional<std::size_t> held() const { return held_; }
    [[nodiscard]] const Target& target() const { return target_; }
    [[nodiscard]] bool hasFile(const std::size_t row) const {
        const auto& rows = session_->rows();
        return row < rows.size() && !rows[row].gap;
    }

    void pickUp(const std::size_t row) {
        held_ = hasFile(row) ? std::optional{row} : std::nullopt;
        target_ = {};
        tell();
        repaint();
    }
    void putBack() {
        held_.reset();
        target_ = {};
        tell();
        repaint();
    }
    void aim(const Target& target, const std::size_t from) {
        if (target != target_) {
            target_ = target;
            repaint();
        }
        tell(from);
    }
    void stopAiming() {
        target_ = {};
        repaint();
        tell();
    }
    // The file at `from` goes where the target says.
    void place(const std::size_t from, const Target& target) {
        held_.reset();
        target_ = {};
        switch (target.kind) {
        case Target::Kind::track:
            session_->placeOn(from, target.row);
            break;
        case Target::Kind::loose:
            if (session_->canUnmatch(static_cast<int>(from))) {
                session_->unmatch(static_cast<int>(from));
            }
            break;
        case Target::Kind::none:
            break;
        }
        tell();
        repaint();
    }

    void renew() {
        identity_ = QUuid::createUuid().toString();
        held_.reset();
        target_ = {};
        tell();
    }
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

    // Lifts a file: its name, see-through, beside the pointer.
    void carry(QWidget* from, const std::size_t row) {
        if (!hasFile(row)) {
            return;
        }
        held_.reset();
        QDrag drag{from};
        drag.setMimeData(payload(row));
        const auto name = session_->rows()[row].file;
        const auto metrics = from->fontMetrics();
        const auto ratio = from->devicePixelRatioF();
        const QSize size{metrics.horizontalAdvance(name) + 20, metrics.height() + 10};
        QPixmap carried{size * ratio};
        carried.setDevicePixelRatio(ratio);
        carried.fill(Qt::transparent);
        {
            QPainter painter{&carried};
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setOpacity(0.8);
            QPainterPath shape;
            shape.addRoundedRect(QRectF{QPointF{0.5, 0.5}, QSizeF{size}}.adjusted(0, 0, -1, -1), 4,
                                 4);
            painter.fillPath(shape, from->palette().color(QPalette::Base));
            painter.setPen(QPen{from->palette().color(QPalette::Highlight), 1.0});
            painter.drawPath(shape);
            painter.setFont(from->font());
            painter.setPen(from->palette().color(QPalette::Text));
            painter.drawText(QRect{QPoint{0, 0}, size}, Qt::AlignCenter, name);
        }
        drag.setPixmap(carried);
        drag.setHotSpot(QPoint{-12, size.height() / 2});
        drag.exec(Qt::MoveAction);
        stopAiming();
    }

    // What would happen, or how it is done.
    void tell(const std::optional<std::size_t> source = std::nullopt) {
        if (!said) {
            return;
        }
        const auto& rows = session_->rows();
        const auto moving = source ? source : held_;
        if (!moving || *moving >= rows.size()) {
            said(tr("Drag a file onto a track — or click it, then the track. Dropped back at the "
                    "left, it is on no track and gets no tags."));
            return;
        }
        const auto file = rows[*moving].file;
        switch (target_.kind) {
        case Target::Kind::track: {
            const auto& there = rows[target_.row];
            said(there.gap ? tr("%1 → %2").arg(file, there.track)
                           : tr("%1 → %2, and %3 goes back to the unmatched files")
                                 .arg(file, there.track, there.file));
            return;
        }
        case Target::Kind::loose:
            said(tr("%1 goes back to the unmatched files: it gets no tags").arg(file));
            return;
        case Target::Kind::none:
            break;
        }
        said(held_ ? tr("%1 picked up — click the track it belongs on, or press Esc").arg(file)
                   : tr("Carrying %1").arg(file));
    }

  private:
    TrackMatchSession* session_;
    QString identity_{QUuid::createUuid().toString()};
    std::optional<std::size_t> held_;
    Target target_;
};

[[nodiscard]] std::optional<std::size_t> rowOf(const QListWidgetItem* item) {
    if (item == nullptr || static_cast<Kind>(item->data(kind_role).toInt()) != Kind::pair) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(item->data(row_role).toULongLong());
}

// A file, drawn as something to pick up: a raised tile with a grip.
void paintFileTile(QPainter* painter, const QRect& tile, const QString& name, const QString& length,
                   const QStyleOptionViewItem& option, const bool held, const bool hovered,
                   const bool other) {
    const auto& palette = option.palette;
    const auto base = palette.color(QPalette::Base);
    const auto text = palette.color(QPalette::Text);
    const auto quiet = palette.color(QPalette::PlaceholderText);
    const QFontMetrics metrics{option.font};
    QPainterPath shape;
    shape.addRoundedRect(QRectF{tile}.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    painter->fillPath(shape, other ? base : blend(base, text, held || hovered ? 10 : 5));
    painter->setPen(QPen{held ? palette.color(QPalette::Highlight) : blend(base, text, 24),
                         held ? 2.0 : 1.0});
    painter->drawPath(shape);
    painter->setPen(Qt::NoPen);
    painter->setBrush(blend(base, text, 42));
    const auto middle = static_cast<double>(tile.center().y());
    for (int column = 0; column < 2; ++column) {
        for (int dot = -1; dot <= 1; ++dot) {
            painter->drawEllipse(QPointF{tile.left() + 9.0 + column * 4.0, middle + dot * 4.0}, 1.2,
                                 1.2);
        }
    }
    painter->setBrush(Qt::NoBrush);
    const auto length_width = metrics.horizontalAdvance(QStringLiteral("00:00")) + 10;
    auto font = option.font;
    font.setItalic(other);
    painter->setFont(font);
    const auto name_rect = tile.adjusted(24, 0, -length_width, 0);
    painter->setPen(other ? quiet : text);
    painter->drawText(name_rect, Qt::AlignLeft | Qt::AlignVCenter,
                      metrics.elidedText(name, Qt::ElideMiddle, name_rect.width()));
    painter->setFont(option.font);
    painter->setPen(quiet);
    painter->drawText(tile.adjusted(0, 0, -8, 0), Qt::AlignRight | Qt::AlignVCenter, length);
}

// The parts of a track's row: the track, the dot, the file on it.
struct Zones {
    QRect track;
    QRect dot;
    QRect file;
};

Zones zonesOf(const QRect& rect) {
    constexpr int dot = 28;
    const auto track = std::max(0, (rect.width() - dot) * 9 / 20);
    return Zones{.track = QRect{rect.left(), rect.top(), track, rect.height()}.adjusted(4, 3, 0, -3),
                 .dot = QRect{rect.left() + track, rect.top(), dot, rect.height()},
                 .file = QRect{rect.left() + track + dot, rect.top(),
                               rect.width() - track - dot, rect.height()}
                             .adjusted(0, 3, -4, -3)};
}

class TrackBoard;

class TrackDelegate final : public QStyledItemDelegate {
  public:
    TrackDelegate(Pairing* pairing, TrackBoard* board, QObject* parent)
        : QStyledItemDelegate(parent), pairing_(pairing), board_(board) {}
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override {
        const auto line = QFontMetrics{option.font}.height();
        const auto kind = static_cast<Kind>(index.data(kind_role).toInt());
        return QSize{200, kind == Kind::pair ? line + 16 : line + 18};
    }

  private:
    void paintTrack(QPainter* painter, const QStyleOptionViewItem& option,
                    const QModelIndex& index) const;
    void paintHeading(QPainter* painter, const QStyleOptionViewItem& option,
                      const QModelIndex& index) const;

    Pairing* pairing_;
    TrackBoard* board_;
};

// The release's tracks, headed by disc, each with its file.
class TrackBoard final : public QListWidget {
  public:
    TrackBoard(Pairing* pairing, QWidget* parent) : QListWidget(parent), pairing_(pairing) {
        setObjectName(QStringLiteral("bench-musicbrainz-match-board"));
        setAccessibleName(QStringLiteral("The release's tracks and the files on them"));
        setItemDelegate(new TrackDelegate(pairing, this, this));
        setSelectionMode(QAbstractItemView::SingleSelection);
        setEditTriggers(QAbstractItemView::NoEditTriggers);
        setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setAcceptDrops(true);
        setDropIndicatorShown(false);
        setAutoScroll(true);
        setMouseTracking(true);
    }

    [[nodiscard]] bool hovering(const std::size_t row) const {
        return hover_row_ && *hover_row_ == row;
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
        for (const auto with : pairing_->session()->discsWithFiles()) {
            if (with == disc) {
                return {};
            }
            ++others;
            other = with;
        }
        if (others == 0U || pairing_->session()->discTrackCount(disc) == 0U) {
            return {};
        }
        return others == 1U ? tr("Take the files of disc %1").arg(other)
                            : tr("Take the files of another disc…");
    }
    [[nodiscard]] QRect offerRect(const QListWidgetItem* heading) const {
        const auto text =
            offerText(static_cast<std::size_t>(heading->data(disc_role).toULongLong()));
        if (text.isEmpty()) {
            return {};
        }
        const auto rect = visualItemRect(heading);
        const auto width = fontMetrics().horizontalAdvance(text) + 12;
        return QRect{rect.right() - 4 - width, rect.top(), width, rect.height()};
    }

    void rebuild(const int select) {
        const auto scrolled = verticalScrollBar()->value();
        clear();
        std::size_t disc = 0;
        const auto& rows = pairing_->session()->rows();
        const auto tracks = pairing_->session()->trackCount();
        for (std::size_t row = 0; row < rows.size() && row < tracks; ++row) {
            const auto& entry = rows[row];
            if (entry.disc != 0U && entry.disc != disc) {
                disc = entry.disc;
                auto* heading = new QListWidgetItem(entry.disc_heading, this);
                heading->setData(kind_role, static_cast<int>(Kind::disc));
                heading->setData(disc_role, static_cast<qulonglong>(disc));
                heading->setFlags(Qt::ItemIsEnabled);
            }
            auto* pair = new QListWidgetItem(entry.gap ? QString{} : entry.file, this);
            pair->setData(kind_role, static_cast<int>(Kind::pair));
            pair->setData(row_role, static_cast<qulonglong>(row));
            pair->setData(track_role, entry.track);
            QStringList tip{entry.track};
            if (!entry.gap) {
                tip << entry.file_tool_tip;
            }
            if (entry.fit) {
                tip << (*entry.fit == musicbrainz::PairFit::good
                            ? tr("Fits")
                            : *entry.fit == musicbrainz::PairFit::doubtful
                                  ? tr("Worth a look")
                                  : tr("Likely the wrong track"));
            }
            if (entry.delta_seconds && std::abs(*entry.delta_seconds) > 3) {
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
    }

  protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override {
        const auto row = items.size() == 1 ? rowOf(items.front()) : std::nullopt;
        return row ? pairing_->payload(*row) : nullptr;
    }
    QStringList mimeTypes() const override { return {mimeType()}; }

    void mousePressEvent(QMouseEvent* event) override {
        press_.reset();
        const auto position = event->position().toPoint();
        auto* at = itemAt(position);
        if (at != nullptr && static_cast<Kind>(at->data(kind_role).toInt()) == Kind::disc &&
            event->button() == Qt::LeftButton && offerRect(at).contains(position)) {
            offer(static_cast<std::size_t>(at->data(disc_role).toULongLong()),
                  event->globalPosition().toPoint());
            return;
        }
        if (const auto row = rowOf(at); row && event->button() == Qt::LeftButton) {
            press_ = Press{.row = *row, .at = position, .on_file = onFile(*at, position)};
        }
        QListWidget::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        const auto position = event->position().toPoint();
        if (press_ && press_->on_file && pairing_->hasFile(press_->row) &&
            (event->buttons() & Qt::LeftButton) != 0 &&
            (position - press_->at).manhattanLength() >= QApplication::startDragDistance()) {
            const auto row = press_->row;
            press_.reset();
            pairing_->carry(this, row);
            return;
        }
        auto* at = itemAt(position);
        const auto row = rowOf(at);
        const bool file = row && pairing_->hasFile(*row) && onFile(*at, position);
        viewport()->setCursor(file ? Qt::OpenHandCursor : Qt::ArrowCursor);
        const auto hovered = file ? row : std::nullopt;
        if (hovered != hover_row_) {
            hover_row_ = hovered;
            viewport()->update();
        }
        if (const auto held = pairing_->held()) {
            pairing_->aim(row && *row != *held ? Target{.kind = Target::Kind::track, .row = *row}
                                               : Target{},
                          *held);
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
        const auto held = pairing_->held();
        // A track clicked with a file picked up: the file goes there.
        if (held && *held != *row) {
            pairing_->place(*held, Target{.kind = Target::Kind::track, .row = *row});
            return;
        }
        // A file clicked is picked up; clicked again, put back.
        if (press->on_file && pairing_->hasFile(*row)) {
            if (held && *held == *row) {
                pairing_->putBack();
            } else {
                pairing_->pickUp(*row);
            }
        }
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape && pairing_->held()) {
            pairing_->putBack();
            event->accept();
            return;
        }
        QListWidget::keyPressEvent(event);
    }
    void dragEnterEvent(QDragEnterEvent* event) override {
        if (pairing_->sourceRow(event->mimeData())) {
            event->acceptProposedAction();
        } else {
            event->ignore();
        }
    }
    void dragMoveEvent(QDragMoveEvent* event) override {
        const auto source = pairing_->sourceRow(event->mimeData());
        if (!source) {
            event->ignore();
            return;
        }
        // The list scrolls at its edges while a file is carried.
        QListWidget::dragMoveEvent(event);
        const auto row = rowOf(itemAt(event->position().toPoint()));
        if (!row || *row == *source) {
            pairing_->aim({}, *source);
            event->ignore();
            return;
        }
        pairing_->aim(Target{.kind = Target::Kind::track, .row = *row}, *source);
        event->acceptProposedAction();
    }
    void dragLeaveEvent(QDragLeaveEvent* event) override {
        pairing_->stopAiming();
        QListWidget::dragLeaveEvent(event);
    }
    void dropEvent(QDropEvent* event) override {
        const auto source = pairing_->sourceRow(event->mimeData());
        const auto row = rowOf(itemAt(event->position().toPoint()));
        if (!source || !row || *row == *source) {
            event->ignore();
            pairing_->stopAiming();
            return;
        }
        event->setDropAction(Qt::MoveAction);
        event->accept();
        pairing_->place(*source, Target{.kind = Target::Kind::track, .row = *row});
    }

  private:
    struct Press {
        std::size_t row{0};
        QPoint at;
        bool on_file{false};
    };

    [[nodiscard]] bool onFile(const QListWidgetItem& at, const QPoint& position) const {
        return position.x() >= zonesOf(visualItemRect(&at)).dot.left();
    }

    void offer(const std::size_t disc, const QPoint& where) {
        auto* session = pairing_->session();
        std::vector<std::size_t> others;
        for (const auto with : session->discsWithFiles()) {
            if (with != disc) {
                others.push_back(with);
            }
        }
        if (others.size() == 1U) {
            session->moveDisc(others.front(), disc);
            return;
        }
        QMenu menu{this};
        for (const auto other : others) {
            menu.addAction(tr("The files of disc %1").arg(other), this,
                           [session, other, disc] { session->moveDisc(other, disc); });
        }
        menu.exec(where);
    }

    Pairing* pairing_;
    std::optional<std::size_t> hover_row_;
    std::optional<Press> press_;
};

void TrackDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                          const QModelIndex& index) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    if (static_cast<Kind>(index.data(kind_role).toInt()) == Kind::pair) {
        paintTrack(painter, option, index);
    } else {
        paintHeading(painter, option, index);
    }
    painter->restore();
}

void TrackDelegate::paintHeading(QPainter* painter, const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const {
    const auto& palette = option.palette;
    const auto area = option.rect.adjusted(8, 0, -4, 0);
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
    const auto* item = board_->item(index.row());
    if (const auto offer = board_->offerRect(item); !offer.isEmpty()) {
        painter->setFont(option.font);
        painter->setPen(palette.color(QPalette::Link));
        painter->drawText(
            offer.adjusted(0, 6, 0, -2), Qt::AlignRight | Qt::AlignVCenter,
            board_->offerText(static_cast<std::size_t>(index.data(disc_role).toULongLong())));
    }
}

void TrackDelegate::paintTrack(QPainter* painter, const QStyleOptionViewItem& option,
                               const QModelIndex& index) const {
    const auto row = static_cast<std::size_t>(index.data(row_role).toULongLong());
    const auto& rows = pairing_->session()->rows();
    if (row >= rows.size()) {
        return;
    }
    const auto& entry = rows[row];
    const auto& palette = option.palette;
    const auto base = palette.color(QPalette::Base);
    const auto text = palette.color(QPalette::Text);
    const auto quiet = palette.color(QPalette::PlaceholderText);
    const auto accent = palette.color(QPalette::Highlight);
    const auto zones = zonesOf(option.rect);
    const QFontMetrics metrics{option.font};
    const auto& target = pairing_->target();
    const bool aimed = target.kind == Target::Kind::track && target.row == row;
    const auto colour = entry.fit ? fitColour(*entry.fit, palette) : quiet;

    if ((option.state & QStyle::State_Selected) != 0) {
        painter->fillRect(option.rect, blend(base, accent, 14));
    } else if (entry.fit && *entry.fit != musicbrainz::PairFit::good) {
        painter->fillRect(option.rect, blend(base, colour, 8));
    }
    // Where a carried file would land: the whole row, outlined.
    if (aimed) {
        QPainterPath landing;
        landing.addRoundedRect(QRectF{option.rect}.adjusted(2.5, 1.5, -2.5, -1.5), 5, 5);
        painter->fillPath(landing, blend(base, accent, 16));
        painter->setPen(QPen{accent, 2.0});
        painter->drawPath(landing);
    }

    // The track: its number, title and length, where it stays.
    const auto number_width = metrics.horizontalAdvance(QStringLiteral("00")) + 6;
    const auto length_width = metrics.horizontalAdvance(QStringLiteral("00:00")) + 10;
    const auto track = zones.track;
    painter->setFont(option.font);
    painter->setPen(quiet);
    painter->drawText(QRect{track.left(), track.top(), number_width, track.height()},
                      Qt::AlignRight | Qt::AlignVCenter, QString::number(entry.number));
    const auto title_rect = track.adjusted(number_width + 10, 0, -length_width, 0);
    painter->setPen(text);
    painter->drawText(title_rect, Qt::AlignLeft | Qt::AlignVCenter,
                      metrics.elidedText(entry.title, Qt::ElideRight, title_rect.width()));
    painter->setPen(entry.delta_seconds && std::abs(*entry.delta_seconds) > 3 ? colour : quiet);
    painter->drawText(track.adjusted(0, 0, -6, 0), Qt::AlignRight | Qt::AlignVCenter,
                      entry.track_length);

    // The dot: how well the file fits -- green, orange, red.
    const QPointF centre{static_cast<double>(zones.dot.center().x()),
                         static_cast<double>(zones.dot.center().y())};
    if (entry.gap) {
        painter->setPen(QPen{blend(base, text, 30), 1.0, Qt::DotLine});
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(centre, 4.0, 4.0);
    } else {
        painter->setPen(Qt::NoPen);
        painter->setBrush(colour);
        painter->drawEllipse(centre, 5.0, 5.0);
        painter->setBrush(Qt::NoBrush);
    }

    // The file on it, to pick up -- or a slot to drop one in.
    const auto file = zones.file;
    if (entry.gap) {
        QPainterPath slot;
        slot.addRoundedRect(QRectF{file}.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
        painter->setPen(QPen{blend(base, text, 28), 1.0, Qt::DashLine});
        painter->drawPath(slot);
        painter->setPen(quiet);
        painter->drawText(file.adjusted(12, 0, -8, 0), Qt::AlignLeft | Qt::AlignVCenter,
                          QObject::tr("drop a file here"));
        return;
    }
    const bool held = pairing_->held() && *pairing_->held() == row;
    paintFileTile(painter, file, entry.file, entry.length, option, held, board_->hovering(row),
                  entry.other);
}

class LooseDelegate final : public QStyledItemDelegate {
  public:
    explicit LooseDelegate(Pairing* pairing, QObject* parent)
        : QStyledItemDelegate(parent), pairing_(pairing) {}
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        // A folder heads its files, as a disc its tracks.
        if (static_cast<Kind>(index.data(kind_role).toInt()) == Kind::disc) {
            const auto& palette = option.palette;
            const auto area = option.rect.adjusted(8, 0, -4, 0);
            painter->save();
            auto font = option.font;
            font.setWeight(QFont::DemiBold);
            painter->setFont(font);
            painter->setPen(palette.color(QPalette::PlaceholderText));
            painter->drawText(QRect{area.left(), option.rect.top() + 6, area.width(),
                                    option.rect.height() - 8},
                              Qt::AlignLeft | Qt::AlignVCenter,
                              QFontMetrics{font}.elidedText(index.data().toString(),
                                                            Qt::ElideMiddle, area.width()));
            painter->setPen(QPen{
                blend(palette.color(QPalette::Base), palette.color(QPalette::Text), 18), 1.0});
            painter->drawLine(area.left(), option.rect.bottom() - 1, area.right(),
                              option.rect.bottom() - 1);
            painter->restore();
            return;
        }
        const auto row = static_cast<std::size_t>(index.data(row_role).toULongLong());
        const auto& rows = pairing_->session()->rows();
        if (row >= rows.size()) {
            return;
        }
        const auto& entry = rows[row];
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        if ((option.state & QStyle::State_Selected) != 0) {
            painter->fillRect(option.rect, blend(option.palette.color(QPalette::Base),
                                                 option.palette.color(QPalette::Highlight), 14));
        }
        const bool held = pairing_->held() && *pairing_->held() == row;
        paintFileTile(painter, option.rect.adjusted(4, 3, -4, -3), entry.file, entry.length,
                      option, held, false, entry.other);
        painter->restore();
    }
    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override {
        const auto heading = static_cast<Kind>(index.data(kind_role).toInt()) == Kind::disc;
        return QSize{160, QFontMetrics{option.font}.height() + (heading ? 18 : 16)};
    }

  private:
    Pairing* pairing_;
};

// The files on no track: picked up from here, dropped back here.
class LooseFiles final : public QListWidget {
  public:
    LooseFiles(Pairing* pairing, QWidget* parent) : QListWidget(parent), pairing_(pairing) {
        setObjectName(QStringLiteral("bench-musicbrainz-match-loose"));
        setAccessibleName(QStringLiteral("Files on no track"));
        setItemDelegate(new LooseDelegate(pairing, this));
        setSelectionMode(QAbstractItemView::SingleSelection);
        setEditTriggers(QAbstractItemView::NoEditTriggers);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setAcceptDrops(true);
        setDropIndicatorShown(false);
        setMouseTracking(true);
    }

    [[nodiscard]] int files() const {
        int counted = 0;
        for (int index = 0; index < count(); ++index) {
            counted += rowOf(item(index)) ? 1 : 0;
        }
        return counted;
    }

    // The files on no track, under their folders -- "CD 2" -- in the order
    // the folders first come.
    void rebuild() {
        clear();
        const auto& rows = pairing_->session()->rows();
        std::vector<std::pair<QString, std::vector<std::size_t>>> folders;
        for (auto row = pairing_->session()->trackCount(); row < rows.size(); ++row) {
            if (rows[row].gap) {
                continue;
            }
            const auto folder = QFileInfo{rows[row].file_tool_tip}.path();
            auto found = std::ranges::find(folders, folder, &decltype(folders)::value_type::first);
            if (found == folders.end()) {
                folders.emplace_back(folder, std::vector<std::size_t>{});
                found = std::prev(folders.end());
            }
            found->second.push_back(row);
        }
        // A folder by its name; two of one name, by their parents' too.
        const auto named = [&folders](const QString& folder) {
            const QFileInfo info{folder};
            const auto name = info.fileName().isEmpty() ? folder : info.fileName();
            const auto alike = std::ranges::count_if(folders, [&name](const auto& other) {
                return QFileInfo{other.first}.fileName() == name;
            });
            return alike > 1 ? QFileInfo{info.path()}.fileName() + QLatin1Char('/') + name : name;
        };
        for (const auto& [folder, members] : folders) {
            auto* heading = new QListWidgetItem(named(folder), this);
            heading->setData(kind_role, static_cast<int>(Kind::disc));
            heading->setToolTip(folder);
            heading->setFlags(Qt::ItemIsEnabled);
            for (const auto row : members) {
                auto* item = new QListWidgetItem(rows[row].file, this);
                item->setData(kind_role, static_cast<int>(Kind::pair));
                item->setData(row_role, static_cast<qulonglong>(row));
                item->setData(track_role, rows[row].track);
                item->setToolTip(rows[row].file_tool_tip + QLatin1Char('\n') + rows[row].track);
            }
        }
    }

  protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override {
        const auto row = items.size() == 1 ? rowOf(items.front()) : std::nullopt;
        return row ? pairing_->payload(*row) : nullptr;
    }
    QStringList mimeTypes() const override { return {mimeType()}; }

    // Where a file carried from a track would land: all of it, outlined.
    void paintEvent(QPaintEvent* event) override {
        QListWidget::paintEvent(event);
        if (pairing_->target().kind != Target::Kind::loose) {
            return;
        }
        QPainter painter{viewport()};
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen{palette().color(QPalette::Highlight), 2.0});
        painter.drawRoundedRect(QRectF{viewport()->rect()}.adjusted(1.5, 1.5, -1.5, -1.5), 5, 5);
    }

    void mousePressEvent(QMouseEvent* event) override {
        press_.reset();
        const auto position = event->position().toPoint();
        // A file picked up from a track, put down here: on no track.
        if (const auto held = pairing_->held();
            held && *held < pairing_->session()->trackCount() &&
            event->button() == Qt::LeftButton) {
            pairing_->place(*held, Target{.kind = Target::Kind::loose, .row = 0});
            return;
        }
        if (const auto row = rowOf(itemAt(position)); row && event->button() == Qt::LeftButton) {
            press_ = Press{.row = *row, .at = position};
        }
        QListWidget::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        const auto position = event->position().toPoint();
        if (press_ && (event->buttons() & Qt::LeftButton) != 0 &&
            (position - press_->at).manhattanLength() >= QApplication::startDragDistance()) {
            const auto row = press_->row;
            press_.reset();
            pairing_->carry(this, row);
            return;
        }
        viewport()->setCursor(rowOf(itemAt(position)) ? Qt::OpenHandCursor : Qt::ArrowCursor);
        QListWidget::mouseMoveEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        const auto press = std::exchange(press_, std::nullopt);
        QListWidget::mouseReleaseEvent(event);
        if (!press || event->button() != Qt::LeftButton) {
            return;
        }
        if (const auto held = pairing_->held(); held && *held == press->row) {
            pairing_->putBack();
        } else {
            pairing_->pickUp(press->row);
        }
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape && pairing_->held()) {
            pairing_->putBack();
            event->accept();
            return;
        }
        QListWidget::keyPressEvent(event);
    }
    void dragEnterEvent(QDragEnterEvent* event) override {
        const auto source = pairing_->sourceRow(event->mimeData());
        // Only a file on a track comes back here.
        if (source && *source < pairing_->session()->trackCount()) {
            event->acceptProposedAction();
            pairing_->aim(Target{.kind = Target::Kind::loose, .row = 0}, *source);
        } else {
            event->ignore();
        }
    }
    void dragMoveEvent(QDragMoveEvent* event) override {
        const auto source = pairing_->sourceRow(event->mimeData());
        if (source && *source < pairing_->session()->trackCount()) {
            event->acceptProposedAction();
        } else {
            event->ignore();
        }
    }
    void dragLeaveEvent(QDragLeaveEvent* event) override {
        pairing_->stopAiming();
        QListWidget::dragLeaveEvent(event);
    }
    void dropEvent(QDropEvent* event) override {
        const auto source = pairing_->sourceRow(event->mimeData());
        if (!source || *source >= pairing_->session()->trackCount()) {
            event->ignore();
            return;
        }
        event->setDropAction(Qt::MoveAction);
        event->accept();
        pairing_->place(*source, Target{.kind = Target::Kind::loose, .row = 0});
    }

  private:
    struct Press {
        std::size_t row{0};
        QPoint at;
    };
    Pairing* pairing_;
    std::optional<Press> press_;
};

// A view over a TrackMatchSession: the files on no track, the release's
// tracks with theirs, what a move would do, and the buttons.
class TrackMatchWidget final : public QWidget {
  public:
    // Embedded, it is the panes and their buttons only: the window around it
    // names the release, says how far the pairing is, and stages.
    TrackMatchWidget(TrackMatchSession* session, std::function<void()> back, QWidget* parent,
                     const bool embedded = false)
        : QWidget(parent), session_(session) {
        setObjectName(QStringLiteral("bench-musicbrainz-track-match"));
        pairing_ = new Pairing(session_, this);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        if (!embedded) {
            heading_ = new QLabel(this);
            heading_->setTextFormat(Qt::PlainText);
            heading_->setWordWrap(true);
            layout->addWidget(heading_);
        }
        auto* splitter = new QSplitter(this);
        splitter->setChildrenCollapsible(false);
        auto* loose_side = new QWidget(splitter);
        auto* loose_layout = new QVBoxLayout(loose_side);
        loose_layout->setContentsMargins(0, 0, 0, 0);
        loose_title_ = new QLabel(loose_side);
        loose_title_->setObjectName(QStringLiteral("bench-musicbrainz-match-loose-title"));
        loose_title_->setForegroundRole(QPalette::PlaceholderText);
        loose_layout->addWidget(loose_title_);
        loose_ = new LooseFiles(pairing_, loose_side);
        loose_layout->addWidget(loose_, 1);
        board_ = new TrackBoard(pairing_, splitter);
        splitter->addWidget(loose_side);
        splitter->addWidget(board_);
        splitter->setStretchFactor(0, 0);
        splitter->setStretchFactor(1, 1);
        splitter->setSizes({260, 900});
        layout->addWidget(splitter, 1);
        hint_ = new QLabel(this);
        hint_->setObjectName(QStringLiteral("bench-musicbrainz-match-hint"));
        hint_->setTextFormat(Qt::PlainText);
        hint_->setForegroundRole(QPalette::PlaceholderText);
        hint_->setWordWrap(true);
        pairing_->said = [this](const QString& text) { hint_->setText(text); };
        pairing_->repaint = [this] {
            board_->viewport()->update();
            loose_->viewport()->update();
        };
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
            tr("Take this file off its track, back to the unmatched files: it gets no tags (U)"));
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
        pairing_->renew();
        board_->rebuild(selected);
        loose_->rebuild();
        const auto loose = loose_->files();
        loose_title_->setText(loose == 0   ? tr("Every file is on a track")
                              : loose == 1 ? tr("1 unmatched file")
                                           : tr("%1 unmatched files").arg(loose));
        if (heading_ != nullptr) {
            const auto fit = session_->fitSummary();
            heading_->setText(fit.isEmpty() ? session_->heading()
                                            : session_->heading() + QStringLiteral(" — ") + fit);
        }
        status_->setText(session_->status());
        updateButtons();
    }

    TrackMatchSession* session_;
    Pairing* pairing_;
    QLabel* heading_{nullptr};
    QLabel* loose_title_;
    LooseFiles* loose_;
    TrackBoard* board_;
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
