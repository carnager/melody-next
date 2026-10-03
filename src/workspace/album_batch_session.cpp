// SPDX-License-Identifier: GPL-3.0-only
#include "workspace/album_batch_session.hpp"

#include "trackknife/loudness/grouping.hpp"
#include "trackknife/musicbrainz/proposal_bridge.hpp"
#include "workspace/album_lookup_queue.hpp"
#include "workspace/tagger_session.hpp"

#include <algorithm>
#include <charconv>
#include <map>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace trackknife::bench {
namespace {

using State = AlbumBatchSession::State;

// A tag as read, else as the library folds its name in cached rows.
[[nodiscard]] std::string tag(const metadata::MetadataDocument& document,
                              std::initializer_list<const char*> names) {
    for (const auto* name : names) {
        if (auto value = document.first_effective_value(name); value && !value->empty()) {
            return *value;
        }
    }
    return {};
}

// "3", "3/12": the leading number.
[[nodiscard]] std::optional<std::size_t> position_of(const std::string& text) {
    std::size_t value = 0U;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    while (begin != end && *begin == ' ') {
        ++begin;
    }
    const auto [stop, error] = std::from_chars(begin, end, value);
    if (error != std::errc{} || stop == begin) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] QString text(const std::string& value) { return QString::fromStdString(value); }

} // namespace

AlbumBatchSession::AlbumBatchSession(TaggerSession& tagger, MusicBrainzLookupService service,
                                     QObject* parent)
    : QObject(parent), tagger_(&tagger), service_(std::move(service)),
      queue_(new AlbumLookupQueue(service_, this)) {
    connect(queue_, &AlbumLookupQueue::looking, this, [this](const std::size_t album) {
        albums_[album].state = State::searching;
        emit changed();
    });
    connect(queue_, &AlbumLookupQueue::lookedUp, this, &AlbumBatchSession::lookedUp);
    connect(queue_, &AlbumLookupQueue::failed, this,
            [this](const std::size_t album, const QString& why) {
                albums_[album].state = State::failed;
                albums_[album].note = why;
                emit changed();
            });
    connect(queue_, &AlbumLookupQueue::idle, this, &AlbumBatchSession::changed);
    connect(&tagger, &TaggerSession::proposalsSettled, this, &AlbumBatchSession::settled);
    // The tagger may still show cached tags: staging waits for the files.
    connect(&tagger, &TaggerSession::changed, this, &AlbumBatchSession::stageNext);
    group();
}

AlbumBatchSession::~AlbumBatchSession() { queue_->stop(); }

void AlbumBatchSession::group() {
    albums_.clear();
    if (tagger_.isNull()) {
        return;
    }
    std::vector<musicbrainz::AlbumGroupInput> inputs;
    inputs.reserve(tagger_->itemCount());
    for (std::size_t item = 0; item < tagger_->itemCount(); ++item) {
        const auto* source = tagger_->itemSource(item);
        const auto& tags = source->baseline;
        inputs.push_back(musicbrainz::AlbumGroupInput{
            .raw_path = source->raw_path,
            .release_id = tag(tags, {"musicbrainz_albumid", "musicbrainzalbumid"}),
            .album_artist = tag(tags, {"albumartist"}),
            .artist = tag(tags, {"artist"}),
            .album = tag(tags, {"album"}),
            .date = tag(tags, {"date", "originaldate"}),
        });
    }
    for (auto& group : musicbrainz::group_albums(inputs)) {
        Album album{.basis = group.basis,
                    .items = std::move(group.items),
                    .folders = std::move(group.folders),
                    .artist = {},
                    .title = {},
                    .year = {},
                    .state = State::waiting,
                    .note = {},
                    .result = std::nullopt};
        describe(album);
        albums_.push_back(std::move(album));
    }
}

// Its name and its files in album order, from the first file that says.
void AlbumBatchSession::describe(Album& album) const {
    if (tagger_.isNull()) {
        return;
    }
    using Key = std::tuple<std::size_t, std::size_t, std::string>;
    std::map<std::size_t, Key> keys;
    for (const auto item : album.items) {
        const auto* source = tagger_->itemSource(item);
        const auto& tags = source->baseline;
        keys[item] = Key{position_of(tag(tags, {"discnumber"})).value_or(1U),
                         position_of(tag(tags, {"tracknumber"})).value_or(0U), source->raw_path};
        if (album.title.isEmpty()) {
            album.title = text(loudness::strip_disc_designator(tag(tags, {"album"})));
        }
        if (album.artist.isEmpty()) {
            album.artist = text(tag(tags, {"albumartist", "artist"}));
        }
        if (album.year.isEmpty()) {
            album.year = text(tag(tags, {"date", "originaldate"}).substr(0, 4));
        }
    }
    std::ranges::sort(album.items, {}, [&keys](const std::size_t item) { return keys[item]; });
    if (album.title.isEmpty() && !album.folders.empty()) {
        const auto& folder = album.folders.front();
        album.title = text(folder.substr(folder.find_last_of('/') + 1U));
    }
}

musicbrainz::AlbumQuery AlbumBatchSession::queryOf(const Album& album) const {
    musicbrainz::AlbumQuery query;
    for (const auto item : album.items) {
        const auto* source = tagger_->itemSource(item);
        const auto& tags = source->baseline;
        if (query.release_id.empty()) {
            query.release_id = tag(tags, {"musicbrainz_albumid", "musicbrainzalbumid"});
        }
        query.tracks.push_back(musicbrainz::LocalTrackDescriptor{
            .title = tag(tags, {"title"}),
            .artist = tag(tags, {"artist"}),
            .album = tag(tags, {"album"}),
            .track_number = position_of(tag(tags, {"tracknumber"})),
            .disc_number = position_of(tag(tags, {"discnumber"})),
            .duration_ms = tagger_->durationOf(item),
        });
    }
    if (album.basis != musicbrainz::AlbumGroupBasis::folder) {
        query.artist = album.artist.toStdString();
        query.album = album.title.toStdString();
    }
    return query;
}

std::size_t AlbumBatchSession::fileCount() const {
    std::size_t files = 0U;
    for (const auto& album : albums_) {
        files += album.items.size();
    }
    return files;
}

std::size_t AlbumBatchSession::count(const State state) const {
    return static_cast<std::size_t>(std::ranges::count(albums_, state, &Album::state));
}

bool AlbumBatchSession::lookingUp() const { return queue_->running(); }

std::size_t AlbumBatchSession::requestsLeft() const { return queue_->requestsLeft(); }

void AlbumBatchSession::splitByFolder(const std::size_t index) {
    if (started_ || index >= albums_.size() || albums_[index].folders.size() < 2U) {
        return;
    }
    const auto original = std::move(albums_[index]);
    albums_.erase(albums_.begin() + static_cast<std::ptrdiff_t>(index));
    std::vector<Album> parts;
    for (const auto& folder : original.folders) {
        Album part{.basis = original.basis,
                   .items = {},
                   .folders = {folder},
                   .artist = {},
                   .title = {},
                   .year = {},
                   .state = original.state,
                   .note = {},
                   .result = std::nullopt};
        for (const auto item : original.items) {
            const auto& path = tagger_->itemSource(item)->raw_path;
            if (path.substr(0, path.find_last_of('/')) == folder) {
                part.items.push_back(item);
            }
        }
        describe(part);
        parts.push_back(std::move(part));
    }
    albums_.insert(albums_.begin() + static_cast<std::ptrdiff_t>(index),
                   std::make_move_iterator(parts.begin()), std::make_move_iterator(parts.end()));
    emit changed();
}

void AlbumBatchSession::merge(const std::size_t index, const std::size_t into) {
    if (started_ || index == into || index >= albums_.size() || into >= albums_.size()) {
        return;
    }
    auto& target = albums_[into];
    auto& joining = albums_[index];
    target.items.insert(target.items.end(), joining.items.begin(), joining.items.end());
    for (const auto& folder : joining.folders) {
        if (std::ranges::find(target.folders, folder) == target.folders.end()) {
            target.folders.push_back(folder);
        }
    }
    describe(target);
    albums_.erase(albums_.begin() + static_cast<std::ptrdiff_t>(index));
    emit changed();
}

void AlbumBatchSession::setIncluded(const std::size_t index, const bool included) {
    if (started_ || index >= albums_.size()) {
        return;
    }
    albums_[index].state = included ? State::waiting : State::left_out;
    emit changed();
}

void AlbumBatchSession::lookUp() {
    if (tagger_.isNull()) {
        return;
    }
    started_ = true;
    for (std::size_t index = 0; index < albums_.size(); ++index) {
        auto& album = albums_[index];
        if (album.state == State::waiting || album.state == State::failed) {
            album.state = State::waiting;
            album.note.clear();
            queue_->add(index, queryOf(album));
        }
    }
    emit changed();
}

void AlbumBatchSession::stop() {
    queue_->stop();
    for (auto& album : albums_) {
        if (album.state == State::searching) {
            album.state = State::waiting;
        }
    }
    emit changed();
}

void AlbumBatchSession::lookedUp(const std::size_t index,
                                 const musicbrainz::AlbumLookupResult& result) {
    auto& album = albums_[index];
    album.result = result;
    switch (result.outcome) {
    case musicbrainz::AlbumLookupOutcome::matched:
        album.state = State::staging;
        to_stage_.push_back(ToStage{.album = index, .proposals = std::nullopt});
        break;
    case musicbrainz::AlbumLookupOutcome::needs_choice:
        album.state = State::needs_choice;
        break;
    case musicbrainz::AlbumLookupOutcome::no_match:
        album.state = State::no_match;
        break;
    }
    emit changed();
    stageNext();
}

void AlbumBatchSession::stageNext() {
    if (staging_ || to_stage_.empty() || tagger_.isNull() || !tagger_->canStageProposals()) {
        return;
    }
    auto next = std::move(to_stage_.front());
    to_stage_.pop_front();
    const auto index = next.album;
    const auto& album = albums_[index];
    auto proposals = [&]() -> core::Result<metadata::MetadataProposalSet> {
        if (next.proposals) {
            return std::move(*next.proposals);
        }
        const auto& best = album.result->candidates.front();
        return musicbrainz::release_metadata_proposals(best.release, best.alignment, album.items);
    }();
    if (!proposals) {
        albums_[index].state = State::needs_choice;
        albums_[index].note = text(proposals.error().message);
        emit changed();
        stageNext();
        return;
    }
    staging_ = index;
    tagger_->applyMusicBrainzProposals(std::move(*proposals));
}

void AlbumBatchSession::settled(const bool staged) {
    if (!staging_) {
        // Staged by the person, not by this batch.
        return;
    }
    auto& album = albums_[*staging_];
    staging_.reset();
    if (staged) {
        album.state = State::staged;
        album.note.clear();
    } else {
        album.state = State::needs_choice;
        album.note = QStringLiteral("Could not be staged by itself");
    }
    emit changed();
    stageNext();
}

std::optional<std::size_t>
AlbumBatchSession::nextNeedingYou(const std::optional<std::size_t> after) const {
    const auto count = albums_.size();
    const auto first = after ? *after + 1U : 0U;
    for (std::size_t step = 0; step < count; ++step) {
        const auto index = (first + step) % count;
        if (after && index == *after) {
            continue;
        }
        const auto& album = albums_[index];
        if (album.state == State::needs_choice && album.result &&
            !album.result->candidates.empty()) {
            return index;
        }
    }
    return std::nullopt;
}

AlbumBatchSession::Files AlbumBatchSession::filesOf(const std::size_t index) const {
    Files files;
    if (tagger_.isNull() || index >= albums_.size()) {
        return files;
    }
    const auto& album = albums_[index];
    files.descriptors = queryOf(album).tracks;
    files.items = album.items;
    for (const auto item : album.items) {
        const auto& path = tagger_->itemSource(item)->raw_path;
        files.paths.push_back(QString::fromLocal8Bit(QByteArray{
            path.data(), static_cast<qsizetype>(path.size())}));
    }
    return files;
}

void AlbumBatchSession::choose(const std::size_t index, const std::size_t version,
                               metadata::MetadataProposalSet proposals) {
    if (index >= albums_.size()) {
        return;
    }
    albums_[index].version = version;
    albums_[index].state = State::staging;
    albums_[index].note.clear();
    to_stage_.push_back(ToStage{.album = index, .proposals = std::move(proposals)});
    emit changed();
    stageNext();
}

void AlbumBatchSession::skip(const std::size_t index) {
    if (index >= albums_.size()) {
        return;
    }
    albums_[index].state = State::skipped;
    emit changed();
}

bool AlbumBatchSession::canWrite() const {
    return writer_ == nullptr && !staging_ && to_stage_.empty() && !tagger_.isNull() &&
           tagger_->canWriteElsewhere() &&
           std::ranges::any_of(albums_, [](const Album& album) { return album.state == State::staged; });
}

void AlbumBatchSession::write(std::vector<std::size_t> albums, AlbumBatchWrite::Options options) {
    if (!canWrite()) {
        return;
    }
    std::vector<AlbumBatchWrite::Album> chosen;
    for (const auto album : albums) {
        if (album < albums_.size() && albums_[album].state == State::staged) {
            chosen.push_back({.album = album, .items = albums_[album].items});
        }
    }
    if (chosen.empty()) {
        return;
    }
    writer_ = new AlbumBatchWrite(*tagger_, std::move(chosen), std::move(options), this);
    connect(writer_, &AlbumBatchWrite::progressed, this, &AlbumBatchSession::changed);
    connect(writer_, &AlbumBatchWrite::finished, this, [this] {
        using Outcome = AlbumBatchWrite::Outcome;
        for (const auto& outcome : writer_->outcomes()) {
            auto& album = albums_[outcome.album];
            switch (outcome.outcome) {
            case Outcome::written:
                album.state = State::written;
                album.note = outcome.note;
                break;
            case Outcome::partly_written:
                // What was not written is still staged.
                album.note = QStringLiteral("%1 of %2 files written · %3")
                                 .arg(outcome.written)
                                 .arg(outcome.files)
                                 .arg(outcome.note);
                break;
            case Outcome::pending:
            case Outcome::left_out:
            case Outcome::failed:
            case Outcome::stopped:
                album.note = QStringLiteral("not written: %1").arg(outcome.note);
                break;
            }
        }
        writer_->deleteLater();
        writer_ = nullptr;
        emit changed();
        emit writeFinished();
        stageNext();
    });
    emit changed();
    writer_->start();
}

std::vector<std::size_t> AlbumBatchSession::changes() const {
    std::vector<std::size_t> counts(albums_.size(), 0U);
    const auto* draft = tagger_.isNull() ? nullptr : tagger_->draft();
    if (draft == nullptr) {
        return counts;
    }
    std::unordered_map<std::size_t, std::size_t> album_of;
    for (std::size_t album = 0U; album < albums_.size(); ++album) {
        for (const auto item : albums_[album].items) {
            album_of.emplace(item, album);
        }
    }
    for (const auto& patch : draft->patches()) {
        if (const auto found = album_of.find(patch.item_index); found != album_of.end()) {
            ++counts[found->second];
        }
    }
    return counts;
}

void AlbumBatchSession::stopWriting() {
    if (writer_ != nullptr) {
        writer_->stop();
    }
}

QString AlbumBatchSession::stateText(const Album& album) {
    const auto confidence = [&album] {
        return album.result && !album.result->candidates.empty()
                   ? QStringLiteral(" %1%").arg(
                         qRound(album.result->candidates.front().alignment.confidence * 100.0))
                   : QString{};
    };
    switch (album.state) {
    case State::waiting:
        return QStringLiteral("Waiting");
    case State::searching:
        return QStringLiteral("Searching…");
    case State::staging:
        return QStringLiteral("Matched%1 · staging").arg(confidence());
    case State::staged:
        return album.note.isEmpty()
                   ? QStringLiteral("Matched%1 · staged").arg(confidence())
                   : QStringLiteral("Staged · %1").arg(album.note);
    case State::needs_choice:
        return album.result && !album.result->candidates.empty()
                   ? QStringLiteral("Pick a version · %1").arg(album.result->candidates.size())
                   : QStringLiteral("Needs you");
    case State::no_match:
        return QStringLiteral("No match");
    case State::failed:
        return QStringLiteral("Failed · %1").arg(album.note);
    case State::left_out:
        return QStringLiteral("Left out");
    case State::skipped:
        return QStringLiteral("Skipped");
    case State::written:
        return album.note.isEmpty() ? QStringLiteral("Written")
                                    : QStringLiteral("Written · %1").arg(album.note);
    }
    return {};
}

} // namespace trackknife::bench
