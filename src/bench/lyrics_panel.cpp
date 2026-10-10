// SPDX-License-Identifier: GPL-3.0-only

#include "bench/lyrics_panel.hpp"

#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QScrollBar>
#include <QSettings>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdlib>

namespace trackknife::bench {
namespace {

[[nodiscard]] QString source_text(const metadata::Lyrics& lyrics) {
    const auto kind = lyrics.synced ? QStringLiteral("Synced") : QStringLiteral("Plain");
    if (lyrics.source == "sidecar") {
        return kind + QStringLiteral(" · .lrc file");
    }
    if (lyrics.source == "sylt") {
        return kind + QStringLiteral(" · ID3 SYLT");
    }
    if (lyrics.source.starts_with("tag:")) {
        return kind + QStringLiteral(" · %1 tag").arg(QString::fromStdString(lyrics.source.substr(4)));
    }
    return kind;
}

} // namespace

LyricsPanel::LyricsPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* heading = new QHBoxLayout;
    heading->setContentsMargins(12, 10, 6, 8);
    auto* titles = new QVBoxLayout;
    titles->setSpacing(1);
    auto* title = new QLabel(QStringLiteral("Lyrics"), this);
    auto font = title->font();
    font.setWeight(QFont::DemiBold);
    font.setPointSizeF(font.pointSizeF() * 1.08);
    title->setFont(font);
    titles->addWidget(title);
    source_ = new QLabel(this);
    source_->setObjectName(QStringLiteral("lyrics-source"));
    source_->setForegroundRole(QPalette::PlaceholderText);
    titles->addWidget(source_);
    heading->addLayout(titles, 1);
    auto* close = new QToolButton(this);
    close->setObjectName(QStringLiteral("lyrics-close"));
    close->setAutoRaise(true);
    close->setIcon(QIcon::fromTheme(QStringLiteral("window-close")));
    close->setToolTip(QStringLiteral("Close Lyrics"));
    close->setAccessibleName(close->toolTip());
    connect(close, &QToolButton::clicked, this, &LyricsPanel::closeRequested);
    heading->addWidget(close);
    layout->addLayout(heading);

    lines_ = new QListWidget(this);
    lines_->setObjectName(QStringLiteral("lyrics-lines"));
    lines_->setFrameShape(QFrame::NoFrame);
    lines_->setSelectionMode(QAbstractItemView::NoSelection);
    lines_->setFocusPolicy(Qt::NoFocus);
    lines_->setWordWrap(true);
    lines_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    lines_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    lines_->setSpacing(3);
    connect(lines_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        const auto row = lines_->row(item);
        if (synced_ && row >= 0 && row < static_cast<int>(times_.size())) {
            emit seekRequested(times_[static_cast<std::size_t>(row)]);
        }
    });
    layout->addWidget(lines_, 1);
    scroll_ = new QVariantAnimation(this);
    scroll_->setDuration(450);
    scroll_->setEasingCurve(QEasingCurve::InOutCubic);
    connect(scroll_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        lines_->verticalScrollBar()->setValue(value.toInt());
    });
    lines_->viewport()->installEventFilter(this);
    showMessage(QStringLiteral("Nothing is playing"));
}

void LyricsPanel::showLyrics(const metadata::Lyrics& lyrics) {
    scroll_->stop();
    lines_->clear();
    times_.clear();
    current_ = -1;
    synced_ = lyrics.synced;
    message_ = false;
    source_->setText(source_text(lyrics));
    for (const auto& line : lyrics.lines) {
        // An empty synced line is a pause in the singing: kept, so the last
        // line sung stops being highlighted.
        auto* item = new QListWidgetItem(QString::fromStdString(line.text), lines_);
        item->setTextAlignment(Qt::AlignCenter);
        item->setForeground(palette().brush(synced_ ? QPalette::PlaceholderText : QPalette::Text));
        times_.push_back(line.time_ms.value_or(0));
    }
    lines_->scrollToTop();
    lines_->setCursor(synced_ ? Qt::PointingHandCursor : Qt::ArrowCursor);
    if (synced_ && reported_ms_) {
        setPosition(*reported_ms_, playing_);
    }
}

void LyricsPanel::showMessage(const QString& message) {
    scroll_->stop();
    lines_->clear();
    times_.clear();
    current_ = -1;
    synced_ = false;
    message_ = true;
    source_->clear();
    auto* item = new QListWidgetItem(message, lines_);
    item->setTextAlignment(Qt::AlignCenter);
    item->setForeground(palette().brush(QPalette::PlaceholderText));
    lines_->setCursor(Qt::ArrowCursor);
}

void LyricsPanel::setPosition(const qint64 position_ms, const bool playing) {
    // Reports come twice a second and a moment late; the clock between them
    // is this one. A report moves it only when it is far off -- a seek made
    // elsewhere, a stall -- or when playing starts or stops.
    constexpr qint64 tolerance_ms = 400;
    const bool fresh = reported_ms_ != position_ms || playing_ != playing;
    const bool started_or_stopped = playing_ != playing;
    reported_ms_ = position_ms;
    if (fresh) {
        const auto predicted = position();
        playing_ = playing;
        if (seek_ms_) {
            // Until a report agrees with the seek, or a moment has passed,
            // the reports are from before it.
            const auto expected = *seek_ms_ + (playing ? seek_clock_.elapsed() : 0);
            if (std::llabs(position_ms - expected) <= 1'500) {
                seek_ms_.reset();
            } else if (seek_clock_.elapsed() < 2'000) {
                if (synced_) {
                    highlight(lineAt(position()));
                }
                return;
            } else {
                seek_ms_.reset();
            }
        }
        if (started_or_stopped || !anchor_clock_.isValid() ||
            std::llabs(position_ms - predicted) > tolerance_ms) {
            anchor_ms_ = position_ms;
            anchor_clock_.start();
        }
    }
    if (synced_) {
        highlight(lineAt(position()));
    }
}

void LyricsPanel::seekTo(const qint64 position_ms) {
    seek_ms_ = position_ms;
    seek_clock_.start();
    anchor_ms_ = position_ms;
    anchor_clock_.start();
    // Asked for here: the person wants to see it, not what they scrolled to.
    manual_scroll_.invalidate();
    if (synced_) {
        highlight(lineAt(position()));
    }
}

qint64 LyricsPanel::position() const {
    if (!anchor_clock_.isValid()) {
        return reported_ms_.value_or(0);
    }
    return anchor_ms_ + (playing_ ? anchor_clock_.elapsed() : 0);
}

int LyricsPanel::lineAt(const qint64 position_ms) const {
    // The last line begun by now.
    const auto after = std::upper_bound(times_.begin(), times_.end(), position_ms);
    return static_cast<int>(after - times_.begin()) - 1;
}

void LyricsPanel::highlight(const int line) {
    if (line == current_) {
        return;
    }
    if (auto* previous = lines_->item(current_)) {
        auto font = previous->font();
        font.setBold(false);
        previous->setFont(font);
        previous->setForeground(palette().brush(QPalette::PlaceholderText));
    }
    current_ = line;
    if (auto* item = lines_->item(current_)) {
        auto font = item->font();
        font.setBold(true);
        item->setFont(font);
        item->setForeground(palette().brush(QPalette::Text));
    }
    follow(current_ < 0 ? 0 : current_);
}

void LyricsPanel::follow(const int line) {
    // Scrolled by hand a moment ago: the person is reading elsewhere.
    if (manual_scroll_.isValid() && manual_scroll_.elapsed() < 4'000) {
        return;
    }
    auto* item = lines_->item(line);
    if (item == nullptr) {
        return;
    }
    auto* bar = lines_->verticalScrollBar();
    const auto rect = lines_->visualItemRect(item);
    const auto target =
        std::clamp(bar->value() + rect.center().y() - lines_->viewport()->height() / 2,
                   bar->minimum(), bar->maximum());
    scroll_->stop();
    if (!isVisible() || std::abs(target - bar->value()) > lines_->viewport()->height() * 2 ||
        !QSettings{}.value(QStringLiteral("appearance/panel-animations"), true).toBool()) {
        // Far away -- a seek -- or not seen: there at once.
        bar->setValue(target);
        return;
    }
    scroll_->setStartValue(bar->value());
    scroll_->setEndValue(target);
    scroll_->start();
}

bool LyricsPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == lines_->viewport() && event->type() == QEvent::Wheel) {
        scroll_->stop();
        manual_scroll_.start();
    }
    return QWidget::eventFilter(watched, event);
}

void LyricsPanel::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) {
        recolor();
    }
}

void LyricsPanel::recolor() {
    for (int row = 0; row < lines_->count(); ++row) {
        const bool dim = message_ || (synced_ && row != current_);
        lines_->item(row)->setForeground(
            palette().brush(dim ? QPalette::PlaceholderText : QPalette::Text));
    }
}

} // namespace trackknife::bench
