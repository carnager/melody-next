// SPDX-License-Identifier: GPL-3.0-only
#include "workspace/album_lookup_queue.hpp"

#include <QPointer>

#include <string_view>
#include <utility>

namespace trackknife::bench {
namespace {

[[nodiscard]] std::string_view view(const QByteArray& body) {
    return {body.constData(), static_cast<std::size_t>(body.size())};
}

[[nodiscard]] QString text(const std::string& message) { return QString::fromStdString(message); }

} // namespace

AlbumLookupQueue::AlbumLookupQueue(MusicBrainzLookupService service, QObject* parent)
    : QObject(parent), service_(std::move(service)) {}

void AlbumLookupQueue::add(const std::size_t id, musicbrainz::AlbumQuery query) {
    waiting_.push_back(Album{.id = id, .query = std::move(query), .to_examine = {}, .examined = {}});
    if (!current_) {
        next();
    }
}

void AlbumLookupQueue::stop() {
    ++generation_;
    waiting_.clear();
    const bool was = current_.has_value();
    current_.reset();
    if (was) {
        emit idle();
    }
}

void AlbumLookupQueue::forget(const std::size_t id) {
    std::erase_if(waiting_, [id](const Album& album) { return album.id == id; });
    if (current_ && current_->id == id) {
        ++generation_;
        current_.reset();
        next();
    }
}

std::size_t AlbumLookupQueue::requestsLeft() const {
    const auto of = [](const Album& album) {
        return album.query.release_id.empty() ? 1U + releases_per_search : 1U;
    };
    std::size_t left = 0U;
    for (const auto& album : waiting_) {
        left += of(album);
    }
    if (current_) {
        left += current_->to_examine.empty() && current_->examined.empty() ? of(*current_)
                                                                           : current_->to_examine.size();
    }
    return left;
}

void AlbumLookupQueue::fetch(const std::string& url,
                             std::function<void(core::Result<QByteArray>)> then) {
    const QPointer self{this};
    const auto generation = generation_;
    service_.fetch(QString::fromStdString(url),
                   [self, generation, then = std::move(then)](core::Result<QByteArray> body) {
                       if (self.isNull() || generation != self->generation_ || !self->current_) {
                           return;
                       }
                       then(std::move(body));
                   });
}

void AlbumLookupQueue::next() {
    if (waiting_.empty() || !service_.fetch) {
        current_.reset();
        emit idle();
        return;
    }
    current_ = std::move(waiting_.front());
    waiting_.pop_front();
    emit looking(current_->id);
    if (!current_->query.release_id.empty()) {
        lookUpRelease(current_->query.release_id, true);
    } else {
        search();
    }
}

void AlbumLookupQueue::lookUpRelease(const std::string& id, const bool fall_back_to_search) {
    const auto url = musicbrainz::build_release_lookup_url(id);
    if (!url) {
        if (fall_back_to_search) {
            search();
        } else {
            examineNext();
        }
        return;
    }
    fetch(*url, [this, fall_back_to_search](core::Result<QByteArray> body) {
        auto release = body ? musicbrainz::parse_release_lookup(view(*body))
                            : core::Result<musicbrainz::Release>{std::unexpected(body.error())};
        if (release) {
            current_->examined.push_back(std::move(*release));
        } else if (fall_back_to_search) {
            // An id MusicBrainz does not know -- merged away, mistyped: found
            // as any other album is.
            search();
            return;
        }
        if (fall_back_to_search) {
            finish();
        } else {
            examineNext();
        }
    });
}

void AlbumLookupQueue::search() {
    const auto& query = current_->query;
    const auto url = musicbrainz::build_release_search_url(musicbrainz::ReleaseSearchQuery{
        .artist = query.artist, .release = query.album, .track_count = std::nullopt, .limit = 25U});
    if (!url) {
        // Nothing to search by: no tags, and no fingerprints asked for.
        finish();
        return;
    }
    fetch(*url, [this](core::Result<QByteArray> body) {
        if (!body) {
            fail(text(body.error().message));
            return;
        }
        auto found = musicbrainz::parse_release_search(view(*body));
        if (!found) {
            fail(text(found.error().message));
            return;
        }
        current_->to_examine =
            musicbrainz::releases_to_examine(current_->query, *found, releases_per_search);
        examineNext();
    });
}

void AlbumLookupQueue::examineNext() {
    if (current_->to_examine.empty()) {
        finish();
        return;
    }
    auto id = std::move(current_->to_examine.front());
    current_->to_examine.erase(current_->to_examine.begin());
    lookUpRelease(id, false);
}

void AlbumLookupQueue::finish() {
    auto album = std::move(*current_);
    current_.reset();
    auto result = musicbrainz::judge_album(album.query, std::move(album.examined));
    emit lookedUp(album.id, result);
    next();
}

void AlbumLookupQueue::fail(const QString& why) {
    const auto id = current_->id;
    current_.reset();
    emit failed(id, why);
    next();
}

} // namespace trackknife::bench
