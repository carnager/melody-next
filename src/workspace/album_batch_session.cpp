// SPDX-License-Identifier: GPL-3.0-only
#include "workspace/album_batch_session.hpp"

#include "trackknife/loudness/grouping.hpp"
#include "trackknife/musicbrainz/proposal_bridge.hpp"
#include "workspace/album_lookup_queue.hpp"
#include "workspace/tagger_session.hpp"

#include <QtConcurrent/QtConcurrentRun>

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
        if (albums_[album].state == State::waiting) {
            albums_[album].state = State::searching;
            emit changed();
        }
    });
    connect(queue_, &AlbumLookupQueue::lookedUp, this, &AlbumBatchSession::lookedUp);
    connect(queue_, &AlbumLookupQueue::failed, this,
            [this](const std::size_t album, const QString& why) {
                if (albums_[album].state != State::searching) {
                    return;
                }
                albums_[album].state = State::failed;
                albums_[album].note = why;
                emit changed();
            });
    connect(queue_, &AlbumLookupQueue::idle, this, &AlbumBatchSession::changed);
    connect(&tagger, &TaggerSession::proposalsSettled, this, &AlbumBatchSession::settled);
    // The tagger may still show cached tags: staging waits for the files.
    connect(&tagger, &TaggerSession::changed, this, [this] {
        noteDraft();
        stageNext();
    });
    group();
}

AlbumBatchSession::~AlbumBatchSession() { queue_->stop(); }

void AlbumBatchSession::group() {
    albums_.clear();
    order_.clear();
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
        order_.push_back(albums_.size());
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
        query.paths.push_back(source->raw_path);
    }
    if (album.basis != musicbrainz::AlbumGroupBasis::folder) {
        query.artist = album.artist.toStdString();
        query.album = album.title.toStdString();
    }
    return query;
}

std::size_t AlbumBatchSession::fileCount() const {
    std::size_t files = 0U;
    for (const auto album : order_) {
        files += albums_[album].items.size();
    }
    return files;
}

bool AlbumBatchSession::editable(const std::size_t album) const {
    if (album >= albums_.size()) {
        return false;
    }
    switch (albums_[album].state) {
    case State::staging:
    case State::staged:
    case State::written:
    case State::replaced:
    case State::merged:
        return false;
    default:
        return true;
    }
}

void AlbumBatchSession::queueLookUp(const std::size_t album) {
    albums_[album].state = State::waiting;
    albums_[album].note.clear();
    albums_[album].result.reset();
    albums_[album].version = 0U;
    if (started_) {
        queue_->add(album, queryOf(albums_[album]));
    }
}

std::size_t AlbumBatchSession::count(const State state) const {
    return static_cast<std::size_t>(std::ranges::count(albums_, state, &Album::state));
}

bool AlbumBatchSession::lookingUp() const { return queue_->running(); }

std::size_t AlbumBatchSession::requestsLeft() const { return queue_->requestsLeft(); }

void AlbumBatchSession::splitByFolder(const std::size_t index) {
    if (!editable(index) || albums_[index].folders.size() < 2U || tagger_.isNull()) {
        return;
    }
    queue_->forget(index);
    const auto left_out = albums_[index].state == State::left_out;
    albums_[index].state = State::replaced;
    const auto original = albums_[index];
    std::vector<std::size_t> parts;
    for (const auto& folder : original.folders) {
        Album part{.basis = original.basis,
                   .items = {},
                   .folders = {folder},
                   .artist = {},
                   .title = {},
                   .year = {},
                   .state = State::left_out,
                   .note = {},
                   .result = std::nullopt};
        for (const auto item : original.items) {
            const auto& path = tagger_->itemSource(item)->raw_path;
            if (path.substr(0, path.find_last_of('/')) == folder) {
                part.items.push_back(item);
            }
        }
        describe(part);
        parts.push_back(albums_.size());
        albums_.push_back(std::move(part));
        if (!left_out) {
            queueLookUp(parts.back());
        }
    }
    // The parts where the album was.
    const auto at = std::ranges::find(order_, index);
    const auto place = order_.erase(at);
    order_.insert(place, parts.begin(), parts.end());
    emit changed();
}

void AlbumBatchSession::merge(const std::size_t index, const std::size_t into) {
    if (index == into || !editable(index) || !editable(into)) {
        return;
    }
    queue_->forget(index);
    queue_->forget(into);
    auto& target = albums_[into];
    auto& joining = albums_[index];
    target.items.insert(target.items.end(), joining.items.begin(), joining.items.end());
    for (const auto& folder : joining.folders) {
        if (std::ranges::find(target.folders, folder) == target.folders.end()) {
            target.folders.push_back(folder);
        }
    }
    describe(target);
    joining.state = State::merged;
    std::erase(order_, index);
    if (target.state != State::left_out) {
        queueLookUp(into);
    }
    emit changed();
}

void AlbumBatchSession::setIncluded(const std::size_t index, const bool included) {
    if (index >= albums_.size()) {
        return;
    }
    auto& album = albums_[index];
    if (album.state == State::staged || album.state == State::staging) {
        album.excluded = !included;
        emit changed();
        return;
    }
    if (!editable(index) || (album.state == State::left_out) != included) {
        return;
    }
    if (included) {
        queueLookUp(index);
    } else {
        queue_->forget(index);
        album.state = State::left_out;
    }
    emit changed();
}

void AlbumBatchSession::lookUp() {
    if (tagger_.isNull()) {
        return;
    }
    const auto again = started_;
    started_ = true;
    for (const auto index : order_) {
        const auto state = albums_[index].state;
        // Again: those that failed, and those found nothing for.
        if (state == State::waiting || state == State::failed ||
            (again && state == State::no_match)) {
            queueLookUp(index);
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
    if (album.state != State::searching && album.state != State::waiting) {
        // Changed while it was asked about.
        return;
    }
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
        // Untagged, and nothing to hear it by.
        if (album.basis == musicbrainz::AlbumGroupBasis::folder &&
            (!service_.fingerprint || !service_.acoustid_lookup)) {
            album.note = QStringLiteral("No tags to search by. Set up AcoustID in Settings › "
                                        "Metadata services to identify it by its sound.");
        }
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
        // Back to the person, saying why -- not round again unexplained.
        album.state = State::needs_choice;
        const auto why = tagger_.isNull() ? QString{} : tagger_->status();
        album.note = why.isEmpty() ? QStringLiteral("Could not be staged")
                                   : QStringLiteral("Could not be staged: %1").arg(why);
    }
    emit changed();
    stageNext();
}

std::optional<std::size_t>
AlbumBatchSession::nextNeedingYou(const std::optional<std::size_t> after) const {
    // In the order shown, from the one after `after`.
    const auto count = order_.size();
    const auto at = after ? std::ranges::find(order_, *after) : order_.end();
    const auto first = at == order_.end() ? 0U
                                          : static_cast<std::size_t>(at - order_.begin()) + 1U;
    for (std::size_t step = 0; step < count; ++step) {
        const auto index = order_[(first + step) % count];
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
    const auto add = [this, &files](const Album& from) {
        auto descriptors = queryOf(from).tracks;
        files.descriptors.insert(files.descriptors.end(), descriptors.begin(), descriptors.end());
        files.items.insert(files.items.end(), from.items.begin(), from.items.end());
        for (const auto item : from.items) {
            const auto& path = tagger_->itemSource(item)->raw_path;
            files.paths.push_back(QString::fromLocal8Bit(QByteArray{
                path.data(), static_cast<qsizetype>(path.size())}));
        }
    };
    add(album);
    // ADR-0265: the other albums' files, those sharing a folder with this
    // one first -- where a file grouped apart most likely belongs.
    std::vector<std::size_t> others;
    for (const auto other : order_) {
        if (other != index && editable(other)) {
            others.push_back(other);
        }
    }
    std::ranges::stable_partition(others, [this, &album](const std::size_t other) {
        return std::ranges::any_of(albums_[other].folders, [&album](const auto& folder) {
            return std::ranges::find(album.folders, folder) != album.folders.end();
        });
    });
    for (const auto other : others) {
        const auto& from = albums_[other];
        add(from);
        const auto label = from.artist.isEmpty() ? from.title
                                                 : QStringLiteral("%1 — %2").arg(from.artist,
                                                                                 from.title);
        files.other_albums.insert(files.other_albums.end(), from.items.size(), label);
    }
    return files;
}

void AlbumBatchSession::refolder(Album& album) const {
    album.folders.clear();
    for (const auto item : album.items) {
        const auto& path = tagger_->itemSource(item)->raw_path;
        const auto slash = path.find_last_of('/');
        auto folder = slash == std::string::npos ? std::string{} : path.substr(0, slash);
        if (std::ranges::find(album.folders, folder) == album.folders.end()) {
            album.folders.push_back(std::move(folder));
        }
    }
}

void AlbumBatchSession::take(const std::size_t index, const std::size_t item) {
    for (std::size_t other = 0; other < albums_.size(); ++other) {
        auto& from = albums_[other];
        if (other == index || !editable(other) || std::ranges::find(from.items, item) == from.items.end()) {
            continue;
        }
        std::erase(from.items, item);
        queue_->forget(other);
        if (from.items.empty()) {
            from.state = State::merged;
            std::erase(order_, other);
        } else {
            refolder(from);
            from.title.clear();
            from.artist.clear();
            from.year.clear();
            describe(from);
            if (from.state != State::left_out) {
                queueLookUp(other);
            }
        }
        albums_[index].items.push_back(item);
        return;
    }
}

void AlbumBatchSession::choose(const std::size_t index, const std::size_t version,
                               metadata::MetadataProposalSet proposals) {
    if (index >= albums_.size()) {
        return;
    }
    // Files taken from other albums join this one.
    bool took = false;
    for (const auto& proposal : proposals.items) {
        if (std::ranges::find(albums_[index].items, proposal.item_index) ==
            albums_[index].items.end()) {
            take(index, proposal.item_index);
            took = true;
        }
    }
    if (took && !tagger_.isNull()) {
        refolder(albums_[index]);
        describe(albums_[index]);
    }
    albums_[index].version = version;
    albums_[index].chosen = proposals;
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
    return writer_ == nullptr && !measuring_ && !staging_ && to_stage_.empty() &&
           !tagger_.isNull() && tagger_->canWriteElsewhere() && !toWrite().empty();
}

bool AlbumBatchSession::canUndoLastWrite() const {
    return last_write_.has_value() && writer_ == nullptr && !measuring_ && !undoing_ &&
           !tagger_.isNull() && static_cast<bool>(tagger_->services().undo) &&
           tagger_->canWriteElsewhere();
}

void AlbumBatchSession::undoLastWrite() {
    if (!canUndoLastWrite() || !tagger_->beginWriteElsewhere()) {
        return;
    }
    undoing_ = true;
    write_summary_.clear();
    emit changed();
    const QPointer self{this};
    (void)QtConcurrent::run([requests = last_write_->requests, undo = tagger_->services().undo,
                             self] {
        auto outcome = std::make_shared<core::Result<std::vector<operations::UndoOutcome>>>(
            undo(requests, {}));
        QMetaObject::invokeMethod(
            self,
            [self, outcome] {
                if (self) {
                    self->undone(outcome);
                }
            },
            Qt::QueuedConnection);
    });
}

void AlbumBatchSession::undone(
    std::shared_ptr<core::Result<std::vector<operations::UndoOutcome>>> outcome) {
    undoing_ = false;
    if (tagger_.isNull()) {
        return;
    }
    if (!*outcome) {
        write_summary_ =
            QStringLiteral("Nothing undone · %1").arg(text(outcome->error().message));
        tagger_->finishWriteElsewhere({});
        emit changed();
        emit undoFinished();
        return;
    }
    // Followed as a write is: the lists, the library, Up Next.
    operations::MetadataApplyResult restored;
    operations::FilePublicationApplyResult moved_back;
    std::size_t refused = 0U;
    for (const auto& undone : **outcome) {
        if (undone.issue) {
            ++refused;
            continue;
        }
        if (undone.restored) {
            restored.sources.push_back(operations::MetadataApplySourceResult{
                .source_index = restored.sources.size(),
                .raw_path = undone.restored->source_raw_path,
                .state = operations::MetadataApplySourceState::committed,
                .commit = undone.restored,
                .issue = std::nullopt});
        }
        if (undone.moved_back) {
            moved_back.sources.push_back(operations::FilePublicationApplySourceResult{
                .source_index = moved_back.sources.size(),
                .source_raw_path = undone.from_raw_path,
                .target_raw_path = undone.to_raw_path,
                .publication = {},
                .state = operations::FilePublicationApplySourceState::committed,
                .commit = undone.moved_back,
                .metadata_commit = std::nullopt,
                .published_metadata = undone.published_metadata,
                .issue = std::nullopt});
        }
    }
    const auto& services = tagger_->services();
    if (!restored.sources.empty() && services.apply_observer) {
        services.apply_observer(restored);
    }
    if (!moved_back.sources.empty() && services.file_apply_observer) {
        services.file_apply_observer(moved_back);
    }
    // The files read again where they are back.
    std::vector<TaggerSession::Rewritten> rewritten;
    if (const auto selection = tagger_->sharedSelection()) {
        for (const auto& undone : **outcome) {
            if (undone.issue) {
                continue;
            }
            for (std::size_t item = 0U; item < selection->item_count(); ++item) {
                if (selection->source(item).raw_path == undone.from_raw_path) {
                    rewritten.push_back({.item = item, .raw_path = undone.to_raw_path});
                }
            }
        }
    }
    const auto albums = last_write_->albums;
    last_write_.reset();
    write_summary_ =
        refused == 0U
            ? QStringLiteral("Undone · %1 %2 as they were, staged again")
                  .arg(albums.size())
                  .arg(albums.size() == 1U ? QStringLiteral("album") : QStringLiteral("albums"))
            : QStringLiteral("Undone in part · %1 %2 could not be put back")
                  .arg(refused)
                  .arg(refused == 1U ? QStringLiteral("file") : QStringLiteral("files"));
    connect(tagger_, &TaggerSession::writtenElsewhere, this,
            [this, albums] {
                // Staged again, as before the Write.
                for (const auto album : albums) {
                    if (albums_[album].state != State::written &&
                        albums_[album].state != State::staged) {
                        continue;
                    }
                    albums_[album].state = State::staging;
                    albums_[album].note.clear();
                    to_stage_.push_back(
                        ToStage{.album = album, .proposals = albums_[album].chosen});
                }
                emit changed();
                emit undoFinished();
                stageNext();
            },
            Qt::SingleShotConnection);
    emit changed();
    tagger_->finishWriteElsewhere(std::move(rewritten));
}

void AlbumBatchSession::noteDraft() {
    // A staged album's patches can turn out to change nothing a moment after
    // it is staged -- what it said, it already said -- and the window shows
    // it so then, not once something else makes it look again.
    std::vector<std::size_t> tagged;
    for (std::size_t album = 0U; album < albums_.size(); ++album) {
        if (alreadyTagged(album)) {
            tagged.push_back(album);
        }
    }
    if (tagged != already_tagged_) {
        already_tagged_ = std::move(tagged);
        emit changed();
    }
}

bool AlbumBatchSession::alreadyTagged(const std::size_t album) const {
    if (album >= albums_.size() || albums_[album].state != State::staged || tagger_.isNull()) {
        return false;
    }
    const auto* draft = tagger_->draft();
    if (draft == nullptr) {
        return true;
    }
    const auto& items = albums_[album].items;
    return std::ranges::none_of(draft->patches(), [&items](const auto& patch) {
        return std::ranges::contains(items, patch.item_index);
    });
}

std::vector<std::size_t> AlbumBatchSession::toWrite() const {
    // Renamed or moved, an album already tagged may still have to go
    // somewhere; otherwise it has nothing to write.
    const bool placing = !tagger_.isNull() && (tagger_->renameFiles() || tagger_->moveFiles());
    std::vector<std::size_t> albums;
    for (const auto album : order_) {
        if (albums_[album].state == State::staged && !albums_[album].excluded &&
            (placing || !alreadyTagged(album))) {
            albums.push_back(album);
        }
    }
    return albums;
}

void AlbumBatchSession::write() {
    if (!canWrite()) {
        return;
    }
    auto albums = toWrite();
    write_summary_.clear();
    if (tagger_->replayGainOnApply()) {
        std::vector<std::size_t> items;
        for (const auto album : albums) {
            items.insert(items.end(), albums_[album].items.begin(), albums_[album].items.end());
        }
        if (auto needing = tagger_->itemsNeedingGain(std::move(items)); !needing.empty()) {
            // Measured first, into the draft; then all written at once.
            measuring_ = true;
            auto measured = std::make_shared<QMetaObject::Connection>();
            *measured = connect(tagger_, &TaggerSession::gainsMeasured, this,
                                [this, albums, measured](const bool ok) {
                                    disconnect(*measured);
                                    measuring_ = false;
                                    if (ok) {
                                        startWriter(albums);
                                        return;
                                    }
                                    write_summary_ =
                                        QStringLiteral("Nothing written: ReplayGain could not be "
                                                       "measured · %1")
                                            .arg(tagger_->status());
                                    emit changed();
                                    emit writeFinished();
                                });
            if (!tagger_->measureBeforeWrite(std::move(needing))) {
                disconnect(*measured);
                measuring_ = false;
                write_summary_ = QStringLiteral("Nothing written: ReplayGain could not start");
                emit changed();
                emit writeFinished();
                return;
            }
            emit changed();
            return;
        }
    }
    startWriter(std::move(albums));
}

void AlbumBatchSession::startWriter(std::vector<std::size_t> albums) {
    std::vector<AlbumBatchWrite::Album> chosen;
    for (const auto album : albums) {
        if (albums_[album].state == State::staged) {
            chosen.push_back({.album = album, .items = albums_[album].items});
        }
    }
    // As the tagger's Actions say: one naming layout, its file name when
    // renaming, its folders inside the destination when moving.
    AlbumBatchWrite::Options options;
    const auto layout = tagger_->layoutProfile(tagger_->layoutIndex());
    if (tagger_->renameFiles()) {
        options.rename = layout;
    }
    if (tagger_->moveFiles()) {
        options.move = layout;
        options.destination = tagger_->moveDestination();
    }
    writer_ = new AlbumBatchWrite(*tagger_, std::move(chosen), std::move(options), this);
    connect(writer_, &AlbumBatchWrite::progressed, this, &AlbumBatchSession::changed);
    connect(writer_, &AlbumBatchWrite::finished, this, [this] {
        using Outcome = AlbumBatchWrite::Outcome;
        std::size_t written = 0U;
        for (const auto& outcome : writer_->outcomes()) {
            auto& album = albums_[outcome.album];
            switch (outcome.outcome) {
            case Outcome::written:
                album.state = State::written;
                album.note = outcome.note;
                ++written;
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
        const auto total = writer_->outcomes().size();
        // Undone newest first.
        last_write_.reset();
        if (!writer_->written().empty()) {
            LastWrite last;
            last.requests.assign(writer_->written().rbegin(), writer_->written().rend());
            for (const auto& outcome : writer_->outcomes()) {
                if (outcome.outcome == Outcome::written ||
                    outcome.outcome == Outcome::partly_written) {
                    last.albums.push_back(outcome.album);
                }
            }
            last_write_ = std::move(last);
        }
        write_summary_ = written == total
                             ? QStringLiteral("Wrote %1 %2")
                                   .arg(written)
                                   .arg(written == 1U ? QStringLiteral("album")
                                                      : QStringLiteral("albums"))
                             : QStringLiteral("Wrote %1 of %2 albums · the rest stay staged, "
                                              "saying why")
                                   .arg(written)
                                   .arg(total);
        writer_->deleteLater();
        writer_ = nullptr;
        emit changed();
        emit writeFinished();
        stageNext();
    });
    emit changed();
    writer_->start();
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
    case State::replaced:
    case State::merged:
        return {};
    }
    return {};
}

} // namespace trackknife::bench
