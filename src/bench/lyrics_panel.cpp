// SPDX-License-Identifier: GPL-3.0-only

#include "bench/lyrics_panel.hpp"

#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

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
    showMessage(QStringLiteral("Nothing is playing"));
}

void LyricsPanel::showLyrics(const metadata::Lyrics& lyrics) {
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
    if (reported_ms_ != position_ms || playing_ != playing) {
        reported_ms_ = position_ms;
        playing_ = playing;
        since_report_.start();
    }
    if (!synced_) {
        return;
    }
    const auto now = position_ms + (playing && since_report_.isValid() ? since_report_.elapsed() : 0);
    highlight(lineAt(now));
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
        lines_->scrollToItem(item, QAbstractItemView::PositionAtCenter);
    }
}

} // namespace trackknife::bench
