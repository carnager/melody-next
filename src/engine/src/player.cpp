// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/player.hpp"

#include <algorithm>
#include <utility>

namespace trackknife::engine {

// The engine's queue, seen the way the advance rules need it.
class Player::QueueView final : public audio::PlaybackList {
  public:
    explicit QueueView(const std::vector<QueueEntry>& entries) : entries_(&entries) {}

    [[nodiscard]] int row_count() const override { return static_cast<int>(entries_->size()); }
    [[nodiscard]] int row_of_entry(const core::StableId& entry, const int hint_row) const override {
        if (entry.is_nil()) {
            return -1;
        }
        if (hint_row >= 0 && hint_row < row_count() &&
            (*entries_)[static_cast<std::size_t>(hint_row)].entry_id == entry) {
            return hint_row;
        }
        const auto found = std::ranges::find_if(*entries_, [&entry](const QueueEntry& candidate) {
            return candidate.entry_id == entry;
        });
        return found == entries_->end() ? -1
                                        : static_cast<int>(std::distance(entries_->begin(), found));
    }
    [[nodiscard]] audio::TrackSource source_at(const int row) const override {
        return (*entries_)[static_cast<std::size_t>(row)].source;
    }

  private:
    const std::vector<QueueEntry>* entries_;
};

Player::Player(std::unique_ptr<audio::LocalAuditionService> audition)
    : audition_(std::move(audition)) {}

Player::~Player() = default;

core::Result<std::unique_ptr<Player>> Player::create() {
    auto audition = audio::LocalAuditionService::create();
    if (!audition) {
        return std::unexpected(std::move(audition.error()));
    }
    // Watch the devices from the start, so the first client to ask sees them
    // and a sink appearing later is noticed without being asked.
    static_cast<void>((*audition)->refresh_output_devices());
    return std::unique_ptr<Player>{new Player{std::move(*audition)}};
}

void Player::reset_order_locked() {
    const QueueView view{queue_};
    row_ = view.row_of_entry(anchors_.current, row_);
    if (modes_.album_random && !queue_.empty() && reset_album_order_locked()) {
        return;
    }
    order_.reset(view.row_count(), row_, modes_.random);
}

bool Player::reset_album_order_locked() {
    audio::AlbumGrouper grouper;
    if (!grouper.admits(queue_.size())) {
        modes_.album_random = false;
        return false;
    }
    for (std::size_t index = 0; index < queue_.size(); ++index) {
        if (!grouper.add(queue_[index].group, static_cast<int>(index))) {
            // The budget is exhausted. Turning the mode off rather than
            // playing a truncated album order is the same choice the window
            // makes, and clients see it because modes travel in the state.
            modes_.album_random = false;
            return false;
        }
    }
    order_.resetAlbums(grouper.take(), row_);
    return true;
}

audio::RequestQueueState Player::request_state_locked() const {
    return {.active = playing_request_, .pending_empty = requests_.empty()};
}

core::Result<void> Player::start_locked(const std::size_t row, const bool from_request) {
    return start_entry_locked(queue_[row], from_request);
}

const QueueEntry* Player::find_locked(const core::StableId& entry_id) const {
    if (entry_id.is_nil()) {
        return nullptr;
    }
    for (const auto* pool : {&queue_, &asks_}) {
        const auto found = std::ranges::find(*pool, entry_id, &QueueEntry::entry_id);
        if (found != pool->end()) {
            return &*found;
        }
    }
    return nullptr;
}

void Player::prune_asks_locked() {
    std::erase_if(asks_, [this](const QueueEntry& ask) {
        return ask.entry_id != anchors_.current && !std::ranges::contains(requests_, ask.entry_id);
    });
}

std::optional<core::Result<void>> Player::start_next_request_locked() {
    while (!requests_.empty()) {
        const auto wanted = requests_.front();
        requests_.erase(requests_.begin());
        // A request whose entry has gone is dropped rather than stopping
        // playback: the user asked for something that is no longer there.
        if (const auto* entry = find_locked(wanted); entry != nullptr) {
            return start_entry_locked(*entry, true);
        }
    }
    return std::nullopt;
}

core::Result<void> Player::start_entry_locked(QueueEntry entry, const bool from_request) {
    if (from_request && !playing_request_) {
        // Where the list was when the ask interrupted it. Recorded before the
        // ask starts, because afterwards the anchors name the request.
        const QueueView view{queue_};
        const auto resume_at = audio::adjacent_playback_row(view, anchors_, modes_, order_,
                                                            request_state_locked(), 1, row_);
        anchors_.request_return = resume_at
                                      ? queue_[static_cast<std::size_t>(resume_at->row)].entry_id
                                      : core::StableId{};
    }
    // A segment is optional, and the audition service spells the two cases as
    // separate calls rather than an optional parameter.
    auto started =
        entry.source.segment
            ? audition_->load_selected_segment_and_play(entry.source.raw_path,
                                                        entry.source.selection,
                                                        *entry.source.segment, entry.replay_gain)
            : audition_->load_selected_and_play(entry.source.raw_path, entry.source.selection,
                                                entry.replay_gain);
    if (!started) {
        return std::unexpected(std::move(started.error()));
    }
    const auto left = anchors_.current;
    anchors_.current = entry.entry_id;
    anchors_.source = entry.source;
    // An ask from outside the list has no row; the list is where the return
    // point leads back to.
    row_ = QueueView{queue_}.row_of_entry(entry.entry_id, -1);
    playing_request_ = from_request;
    ++revision_;
    // Consume drops the entry playback just left. Done after the new one is
    // anchored, because erasing moves rows and the anchor is what survives
    // that (ADR-0221).
    if (modes_.consume_active() && !left.is_nil() && left != anchors_.current) {
        consume_locked(left);
    }
    if (!from_request) {
        // Ordinary playback resumed, so there is nothing to return to.
        anchors_.request_return = core::StableId{};
    }
    if (row_ >= 0) {
        order_.advance(row_, 1);
    }
    std::erase(requests_, anchors_.current);
    prune_asks_locked();
    seen_transitions_ = audition_->snapshot().chain_transitions;
    gapless_entry_.reset();
    refresh_gapless_locked();
    return {};
}

void Player::consume_locked(const core::StableId& entry_id) {
    const QueueView view{queue_};
    const auto row = view.row_of_entry(entry_id, -1);
    if (row < 0) {
        return;
    }
    queue_.erase(queue_.begin() + row);
    consumed_ = entry_id;
    ++revision_;
    std::erase(requests_, entry_id);
    // Rows moved, so the playing row is re-derived from its identity rather
    // than adjusted by hand.
    const QueueView remaining{queue_};
    row_ = remaining.row_of_entry(anchors_.current, -1);
    reset_order_locked();
    if (modes_.expire_consume()) {
        // A one-shot fires once. Clients learn the new mode from the state
        // document rather than being told separately.
        reset_order_locked();
    }
}

void Player::refresh_gapless_locked() {
    const QueueView view{queue_};
    // What plays next if nothing interrupts: a request first, then the order.
    const QueueEntry* next = nullptr;
    bool next_is_request = false;
    // Single stops rather than playing an ask, as advancing does.
    if (!modes_.single_active()) {
        for (const auto& wanted : requests_) {
            if ((next = find_locked(wanted)) != nullptr) {
                next_is_request = true;
                break;
            }
        }
    }
    if (next == nullptr) {
        // Asking the order what is next does not consume it: PlaybackOrder
        // holds its draw in `pending_` and returns the same answer until
        // advance() commits it, precisely so a status refresh can ask
        // repeatedly. A defensive copy here would be waste -- and I wrote one
        // before reading that, then could not make a test fail without it.
        if (const auto choice = audio::automatic_playback_row(view, anchors_, modes_, order_,
                                                              request_state_locked(), row_)) {
            next = &queue_[static_cast<std::size_t>(choice->row)];
        }
    }
    if (next == nullptr) {
        if (gapless_entry_) {
            static_cast<void>(audition_->clear_gapless_next());
            gapless_entry_.reset();
        }
        return;
    }
    const auto& entry = *next;
    gapless_from_request_ = next_is_request;
    // Checked against what the audition service is actually holding, not
    // against what this class remembers offering. The two disagree whenever
    // something drops the queued continuation -- a seek does -- and trusting
    // the memory means the rest of that track plays with nothing armed, so
    // gapless silently stops working for every track the user seeks in.
    const auto snapshot = audition_->snapshot();
    const bool holding = !snapshot.next_raw_path.empty() &&
                         snapshot.next_raw_path == entry.source.raw_path &&
                         snapshot.next_segment == entry.source.segment;
    if (gapless_entry_ == entry.entry_id && holding) {
        return;
    }
    const auto queued = entry.source.segment
                            ? audition_->queue_gapless_next_selected_segment(
                                  entry.source.raw_path, entry.source.selection,
                                  *entry.source.segment, entry.replay_gain)
                            : audition_->queue_gapless_next_selected(
                                  entry.source.raw_path, entry.source.selection, entry.replay_gain);
    // A rejected continuation is not an error: the formats may differ, and the
    // engine simply plays the next track the ordinary way.
    gapless_entry_ = queued ? std::optional{entry.entry_id} : std::nullopt;
}

void Player::follow_gapless_locked(const audio::LocalAuditionSnapshot& snapshot) {
    if (snapshot.chain_transitions == seen_transitions_) {
        return;
    }
    seen_transitions_ = snapshot.chain_transitions;
    if (!gapless_entry_) {
        return;
    }
    const auto* entry = find_locked(*gapless_entry_);
    gapless_entry_.reset();
    if (entry == nullptr) {
        return;
    }
    // The same books a start keeps: a request records where the list was,
    // and ordinary playback forgets any return point.
    if (gapless_from_request_ && !playing_request_) {
        const QueueView view{queue_};
        const auto resume_at = audio::adjacent_playback_row(view, anchors_, modes_, order_,
                                                            request_state_locked(), 1, row_);
        anchors_.request_return = resume_at
                                      ? queue_[static_cast<std::size_t>(resume_at->row)].entry_id
                                      : core::StableId{};
    } else if (!gapless_from_request_) {
        anchors_.request_return = core::StableId{};
    }
    playing_request_ = gapless_from_request_;
    // The engine moved on without being told to, so the anchors follow it
    // rather than the other way round.
    const auto left = anchors_.current;
    anchors_.current = entry->entry_id;
    anchors_.source = entry->source;
    row_ = QueueView{queue_}.row_of_entry(anchors_.current, -1);
    if (row_ >= 0) {
        order_.advance(row_, 1);
    }
    std::erase(requests_, anchors_.current);
    ++revision_;
    // A gapless handover finishes a track as surely as a load does, so
    // consume applies here too. It once did not: a list played gaplessly in
    // consume mode kept every entry.
    if (modes_.consume_active() && !left.is_nil() && left != anchors_.current) {
        consume_locked(left);
    }
    prune_asks_locked();
}

void Player::replace_queue(std::vector<QueueEntry> entries) {
    const std::lock_guard guard{mutex_};
    queue_ = std::move(entries);
    ++revision_;
    // ADR-0221: the playing entry is followed by identity. If it has gone,
    // playback is not silently handed to whatever now sits at its old row.
    const QueueView view{queue_};
    std::erase_if(requests_,
                  [this](const core::StableId& wanted) { return find_locked(wanted) == nullptr; });
    row_ = view.row_of_entry(anchors_.current, -1);
    // An ask that is playing has no row and is still playing.
    if (row_ < 0 && !anchors_.current.is_nil() &&
        !std::ranges::contains(asks_, anchors_.current, &QueueEntry::entry_id)) {
        anchors_.current = core::StableId{};
    }
    reset_order_locked();
    refresh_gapless_locked();
}

std::vector<QueueEntry> Player::queue() const {
    const std::lock_guard guard{mutex_};
    return queue_;
}

core::Result<void> Player::request(const core::StableId& entry_id) {
    const std::lock_guard guard{mutex_};
    if (find_locked(entry_id) == nullptr) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::not_found,
                        .message = "no such entry in the queue",
                        .context = {{.key = "entry", .value = entry_id.to_string()}}});
    }
    requests_.push_back(entry_id);
    ++revision_;
    return {};
}

std::vector<core::StableId> Player::requests() const {
    const std::lock_guard guard{mutex_};
    return requests_;
}

core::Result<void> Player::set_requests(const std::vector<core::StableId>& entries) {
    const std::lock_guard guard{mutex_};
    for (const auto& wanted : entries) {
        if (find_locked(wanted) == nullptr) {
            return std::unexpected(
                core::Error{.code = core::ErrorCode::not_found,
                            .message = "no such entry in the queue",
                            .context = {{.key = "entry", .value = wanted.to_string()}}});
        }
    }
    requests_ = entries;
    ++revision_;
    prune_asks_locked();
    refresh_gapless_locked();
    return {};
}

void Player::enqueue(std::vector<QueueEntry> entries) {
    const std::lock_guard guard{mutex_};
    for (auto& entry : entries) {
        if (find_locked(entry.entry_id) != nullptr) {
            continue;
        }
        asks_.push_back(std::move(entry));
    }
    // Held but not yet requested: set_requests, which follows, names them.
    // The list, its order and its revision are untouched.
}

void Player::clear_requests() {
    const std::lock_guard guard{mutex_};
    requests_.clear();
    ++revision_;
    prune_asks_locked();
    refresh_gapless_locked();
}

core::Result<void> Player::play_entry(const core::StableId& entry_id) {
    const std::lock_guard guard{mutex_};
    const QueueView view{queue_};
    const auto row = view.row_of_entry(entry_id, row_);
    if (row < 0) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::not_found,
                        .message = "no such entry in the queue",
                        .context = {{.key = "entry", .value = entry_id.to_string()}}});
    }
    return start_locked(static_cast<std::size_t>(row));
}

core::Result<void> Player::set_volume_percent(const int percent) {
    const std::lock_guard guard{mutex_};
    return audition_->set_volume_percent(percent);
}

core::Result<void> Player::set_replay_gain_mode(const audio::ReplayGainMode mode) {
    const std::lock_guard guard{mutex_};
    return audition_->set_replay_gain_mode(mode);
}

core::Result<void> Player::set_replay_gain_preamps(const audio::ReplayGainPreamps preamps) {
    const std::lock_guard guard{mutex_};
    return audition_->set_replay_gain_preamps(preamps);
}

core::Result<void> Player::set_output_target(std::optional<std::string> target) {
    const std::lock_guard guard{mutex_};
    return audition_->set_output_target(std::move(target));
}

core::Result<void> Player::refresh_output_devices() {
    const std::lock_guard guard{mutex_};
    return audition_->refresh_output_devices();
}

core::Result<void> Player::set_buffer_config(const audio::PlaybackBufferDurationConfig buffer) {
    const std::lock_guard guard{mutex_};
    return audition_->set_buffer_config(buffer);
}

Player::Output Player::output() const {
    const std::lock_guard guard{mutex_};
    const auto snapshot = audition_->snapshot();
    return Output{.target = snapshot.output_target, .buffer = snapshot.configured_buffer};
}

core::Result<void> Player::resume() {
    const std::lock_guard guard{mutex_};
    return audition_->play();
}

core::Result<void> Player::pause() {
    const std::lock_guard guard{mutex_};
    return audition_->pause();
}

core::Result<void> Player::stop() {
    const std::lock_guard guard{mutex_};
    auto stopped = audition_->stop();
    anchors_.current = core::StableId{};
    anchors_.source = {};
    row_ = -1;
    return stopped;
}

core::Result<void> Player::seek_ms(const std::int64_t position_ms) {
    const std::lock_guard guard{mutex_};
    if (position_ms < 0) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "a position cannot be negative",
                                           .context = {}});
    }
    return audition_->seek_to_seconds(static_cast<double>(position_ms) / 1000.0);
}

core::Result<void> Player::step(const int direction) {
    const std::lock_guard guard{mutex_};
    const QueueView view{queue_};
    // A request is an explicit ask and outranks the order, but only going
    // forward: stepping back means "the track before this one", not "undo a
    // request nobody has heard yet".
    if (direction > 0) {
        if (auto started = start_next_request_locked()) {
            return std::move(*started);
        }
    }
    const auto choice = audio::adjacent_playback_row(view, anchors_, modes_, order_,
                                                     request_state_locked(), direction, row_);
    if (!choice) {
        return std::unexpected(core::Error{.code = core::ErrorCode::not_found,
                                           .message = "nothing to play in that direction",
                                           .context = {}});
    }
    return start_locked(static_cast<std::size_t>(choice->row));
}

audio::PlaybackModes Player::modes() const {
    const std::lock_guard guard{mutex_};
    return modes_;
}

void Player::set_modes(audio::PlaybackModes modes) {
    const std::lock_guard guard{mutex_};
    modes_ = modes;
    ++revision_;
    // Random and album-random change the traversal, so the order is rebuilt
    // rather than left describing the previous mode -- and with it whatever
    // was queued to follow, which was chosen under the old one.
    reset_order_locked();
    refresh_gapless_locked();
}

bool Player::armed_continuation() const {
    const std::lock_guard guard{mutex_};
    return !audition_->snapshot().next_raw_path.empty();
}

bool Player::advance_if_ended() {
    const std::lock_guard guard{mutex_};
    const auto snapshot = audition_->snapshot();
    // A gapless takeover is already the next track playing, so the anchors
    // follow it before anything decides the output is idle.
    follow_gapless_locked(snapshot);
    if (snapshot.state != audio::LocalAuditionState::ended) {
        advanced_from_.reset();
        refresh_gapless_locked();
        return false;
    }
    // "Ended" persists until the next load, so without this the same finished
    // track would dispatch an advance on every tick and race through the
    // queue. Remembering which entry was advanced from is what makes it once.
    if (advanced_from_ == anchors_.current) {
        return false;
    }
    advanced_from_ = anchors_.current;

    const QueueView view{queue_};
    // An explicit ask outranks the order, exactly as it does when the user
    // presses next -- unless single is active, where the point is to stop.
    if (!modes_.single_active()) {
        if (auto started = start_next_request_locked()) {
            return started->has_value();
        }
    }
    // The request state is the real one: a request that ends by itself
    // returns to where the list was, exactly as pressing next does. Passing
    // "no request" here once meant an ask from outside the list ended in
    // silence.
    const auto choice =
        audio::automatic_playback_row(view, anchors_, modes_, order_, request_state_locked(), row_);
    if (!choice) {
        // Nothing follows: the queue is done, or single mode says stop here.
        // The anchors stay where they are so a client can still see what was
        // playing, which is what the window shows after a list finishes --
        // unless consume removes the entry, since a finished track is
        // consumed whether or not anything plays after it.
        if (modes_.consume_active()) {
            consume_locked(anchors_.current);
        }
        if (modes_.expire_single()) {
            reset_order_locked();
        }
        return false;
    }
    const auto started = start_locked(static_cast<std::size_t>(choice->row));
    if (modes_.expire_single()) {
        reset_order_locked();
    }
    return started.has_value();
}

Player::Observations Player::observe(const std::int64_t monotonic_ms) {
    const std::lock_guard guard{mutex_};
    const auto snapshot = audition_->snapshot();
    Observations observations;
    follow_gapless_locked(snapshot);
    refresh_gapless_locked();

    // ADR-0220 Phase 0's rules, unchanged: a moment counts only while the
    // output is actually carrying audio, and only against a playback instance
    // that distinguishes replaying a file from continuing it.
    const auto observation = audio::listen_observation(snapshot);
    if (listening_.observe(observation.identity, observation.duration_seconds,
                           observation.position_seconds, observation.playing, monotonic_ms) &&
        !anchors_.current.is_nil()) {
        observations.listened_entry = anchors_.current;
        observations.listened_source = anchors_.source;
    }

    if (audio::resumable(snapshot) && !anchors_.current.is_nil()) {
        observations.resume_entry = anchors_.current;
        observations.resume_source = anchors_.source;
        observations.resume_position_ms = audio::resume_position_ms(snapshot);
    }
    return observations;
}

Player::Persisted Player::persisted() const {
    const std::lock_guard guard{mutex_};
    const auto snapshot = audition_->snapshot();
    Persisted stored;
    stored.queue = queue_;
    stored.asks = asks_;
    stored.entry = anchors_.current;
    stored.request_return = anchors_.request_return;
    stored.playing_request = playing_request_;
    stored.requests = requests_;
    stored.modes = modes_;
    if (snapshot.format && snapshot.format->sample_rate > 0) {
        const auto rate = static_cast<std::int64_t>(snapshot.format->sample_rate);
        stored.position_ms =
            snapshot.position_sample / rate * 1000 + snapshot.position_sample % rate * 1000 / rate;
    }
    stored.revision = snapshot.source_revision;
    return stored;
}

bool Player::restore(Persisted state) {
    const std::lock_guard guard{mutex_};
    queue_ = std::move(state.queue);
    asks_ = std::move(state.asks);
    modes_ = state.modes;
    requests_ = std::move(state.requests);
    anchors_.request_return = state.request_return;
    playing_request_ = state.playing_request;
    anchors_.current = state.entry;
    consumed_ = core::StableId{};
    ++revision_;

    const QueueView view{queue_};
    row_ = view.row_of_entry(anchors_.current, -1);
    const auto* anchored = find_locked(anchors_.current);
    if (anchored == nullptr) {
        // The entry is gone from the queue it was in. The queue still comes
        // back; nothing is anchored in it.
        anchors_.current = core::StableId{};
        anchors_.source = {};
        prune_asks_locked();
        reset_order_locked();
        return false;
    }
    const auto entry = *anchored;
    anchors_.source = entry.source;
    reset_order_locked();
    if (!state.revision) {
        // Nothing was playing when this was written, or the source had no
        // revision to check. The queue is restored and nothing is loaded.
        return false;
    }
    // Paused rather than playing: coming back from a restart and starting to
    // make noise unasked is not a restore, it is a surprise.
    auto restored =
        audition_->restore_paused(entry.source.raw_path, *state.revision, entry.source.selection,
                                  entry.source.segment, state.position_ms, entry.replay_gain);
    if (!restored) {
        return false;
    }
    seen_transitions_ = audition_->snapshot().chain_transitions;
    refresh_gapless_locked();
    return true;
}

std::uint64_t Player::revision() const {
    const std::lock_guard guard{mutex_};
    return revision_;
}

Player::State Player::state() const {
    const std::lock_guard guard{mutex_};
    const auto snapshot = audition_->snapshot();
    State current;
    switch (snapshot.state) {
    case audio::LocalAuditionState::playing:
    case audio::LocalAuditionState::draining:
        current.status = "playing";
        break;
    case audio::LocalAuditionState::paused:
        current.status = "paused";
        break;
    case audio::LocalAuditionState::loading:
    case audio::LocalAuditionState::buffering:
        current.status = "loading";
        break;
    default:
        current.status = "stopped";
        break;
    }
    current.entry = anchors_.current;
    current.source = anchors_.source;
    if (snapshot.format && snapshot.format->sample_rate > 0) {
        const auto rate = static_cast<std::int64_t>(snapshot.format->sample_rate);
        current.position_ms =
            snapshot.position_sample / rate * 1000 + snapshot.position_sample % rate * 1000 / rate;
    }
    if (const auto* playing = find_locked(anchors_.current); playing != nullptr) {
        current.duration_ms = playing->duration_ms.value_or(-1);
    }
    current.queue_size = queue_.size();
    current.requests = requests_.size();
    current.modes = modes_;
    current.volume_percent = snapshot.volume_percent;
    current.gapless_entry = gapless_entry_.value_or(core::StableId{});
    current.instance = snapshot.playback_instance;
    current.consumed = consumed_;
    current.queue_revision = revision_;
    current.replay_gain_mode = snapshot.replay_gain_mode;
    current.replay_gain_preamps = snapshot.replay_gain_preamps;
    current.output_target = snapshot.output_target;
    current.default_output = snapshot.default_output_target;
    current.output_available = snapshot.output_target_available;
    current.devices = snapshot.devices;
    current.output_suspended = snapshot.output_suspended;
    current.buffer = snapshot.configured_buffer;
    current.buffer_pending =
        snapshot.active_buffer && *snapshot.active_buffer != snapshot.configured_buffer;
    current.underruns = snapshot.underrun_count;
    return current;
}

} // namespace trackknife::engine
