// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/quick_pick_session.hpp"

#include "trackknife/engine/catalogue.hpp"

#include <QDateTime>
#include <QtConcurrent/QtConcurrentRun>

#include <utility>

namespace trackknife::bench {
namespace {

constexpr std::size_t result_limit = 60U;

[[nodiscard]] QString text(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

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

QuickPickSession::QuickPickSession(const QuickPickKind kind,
                                   std::shared_ptr<engine::Catalogue> catalogue, QString scope,
                                   QObject* parent)
    : QObject(parent), kind_(kind), catalogue_(std::move(catalogue)), scope_(std::move(scope)),
      status_(idleText()) {
    debounce_.setSingleShot(true);
    debounce_.setInterval(120);
    connect(&debounce_, &QTimer::timeout, this, &QuickPickSession::search);
    connect(&watcher_, &QFutureWatcher<Found>::finished, this, &QuickPickSession::showResults);
}

QuickPickSession::~QuickPickSession() {
    // The search holds the catalogue it was given; let it finish with it.
    watcher_.waitForFinished();
}

QString QuickPickSession::placeholder() const {
    return kind_ == QuickPickKind::album ? tr("Album, artist or year — e.g. doors 1967")
                                         : tr("Title, artist, album or year — e.g. crystal doors");
}

QString QuickPickSession::keysText() {
    return tr("Enter add · Shift+Enter replace and play · Ctrl+Enter play next · "
              "Ctrl+Shift+Enter add to Up Next · Alt+Enter new tab");
}

void QuickPickSession::setText(const QString& text) {
    text_ = text;
    debounce_.start();
}

void QuickPickSession::search() {
    const auto words = text_.simplified();
    ++generation_;
    // Nothing typed: what came in most recently, newest first.
    const bool newest = words.isEmpty();
    // One search at a time: a newer text waits for it, then runs.
    if (watcher_.isRunning()) {
        pending_ = true;
        return;
    }
    status_ = tr("Searching…");
    emit changed();
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

void QuickPickSession::showResults() {
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
        status_ = found.error;
        emit changed();
        return;
    }
    entries_ = std::move(found.entries);
    rows_.clear();
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
        const auto name =
            text(albums ? entry.album : (entry.title.empty() ? entry.label : entry.title));
        rows_.push_back({name, details.join(QStringLiteral(" · "))});
    }
    const auto count = entries_.size();
    const auto noun = [albums](const std::size_t n) {
        return albums ? (n == 1U ? tr("1 album") : tr("%1 albums").arg(n))
                      : (n == 1U ? tr("1 track") : tr("%1 tracks").arg(n));
    };
    if (found.newest) {
        status_ =
            count == 0U ? idleText() : tr("Recently added — type to search the whole library");
    } else {
        status_ = count == 0U  ? (albums ? tr("No albums match.") : tr("No tracks match."))
                  : found.more ? tr("First %1 — type more to narrow them.").arg(noun(count))
                               : noun(count);
    }
    emit changed();
}

QString QuickPickSession::idleText() const {
    return kind_ == QuickPickKind::album ? tr("Type part of an album, its artist or its year.")
                                         : tr("Type part of a title, its artist, album or year.");
}

void QuickPickSession::choose(const int row, const LocalLibraryAction action) {
    if (row < 0 || row >= static_cast<int>(entries_.size())) {
        return;
    }
    emit chosen({entries_[static_cast<std::size_t>(row)]}, action);
}

LocalLibraryAction QuickPickSession::actionFor(Qt::KeyboardModifiers modifiers) {
    modifiers &= ~Qt::KeypadModifier;
    return modifiers == (Qt::ControlModifier | Qt::ShiftModifier) ? LocalLibraryAction::request_end
           : modifiers == Qt::ControlModifier                     ? LocalLibraryAction::request_next
           : modifiers == Qt::ShiftModifier                       ? LocalLibraryAction::replace
           : modifiers == Qt::AltModifier                         ? LocalLibraryAction::new_list
                                                                  : LocalLibraryAction::append;
}

} // namespace trackknife::bench
