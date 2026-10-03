// SPDX-License-Identifier: GPL-3.0-only
#include "trackknife/output/upnp_audition.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>

namespace trackknife::output {
namespace {
using Values = discovery::UpnpValues;
using State = audio::LocalAuditionState;
core::Error unsupported(std::string message) {
    return {.code = core::ErrorCode::unsupported, .message = std::move(message), .context = {}};
}
std::string value(const Values& values, const std::string& key) {
    auto it = values.find(key);
    return it == values.end() ? std::string{} : it->second;
}
std::string time_text(std::int64_t ms) {
    const auto seconds = std::max<std::int64_t>(0, ms / 1000);
    std::ostringstream out;
    out << seconds / 3600 << ':' << std::setfill('0') << std::setw(2) << seconds / 60 % 60 << ':'
        << std::setw(2) << seconds % 60;
    return out.str();
}
bool accepts(const std::string& sink, const std::string& mime) {
    std::istringstream list{sink};
    std::string protocol;
    while (std::getline(list, protocol, ',')) {
        const auto first = protocol.find(':');
        const auto second = first == std::string::npos ? first : protocol.find(':', first + 1);
        const auto third = second == std::string::npos ? second : protocol.find(':', second + 1);
        if (third == std::string::npos) {
            continue;
        }
        if (protocol.substr(0, first) == "http-get" &&
            protocol.substr(second + 1, third - second - 1) == mime) {
            return true;
        }
    }
    return false;
}
} // namespace
core::Result<std::optional<StreamFormat>>
renderer_stream_format(const std::string& sink, const std::string& mime, bool part) {
    if (!part && mime != "application/octet-stream" && accepts(sink, mime)) {
        return std::nullopt;
    }
    if (accepts(sink, "audio/flac") || accepts(sink, "audio/x-flac")) {
        return StreamFormat{.codec = StreamCodec::flac, .sample_rate_cap = {}};
    }
    if (accepts(sink, "audio/wav") || accepts(sink, "audio/x-wav") || accepts(sink, "audio/wave")) {
        return StreamFormat{.codec = StreamCodec::wav, .sample_rate_cap = {}};
    }
    return std::unexpected(
        unsupported("renderer advertises no compatible original, FLAC or WAV format"));
}
std::string renderer_didl(const std::string& url, const std::string& mime, std::int64_t duration_ms,
                          const std::string& title, const std::string& artist,
                          const std::string& album, const std::string& artwork) {
    const auto esc = discovery::upnp_xml_escape;
    return "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
           "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
           "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">"
           "<item id=\"0\" parentID=\"-1\" restricted=\"1\"><dc:title>" +
           esc(title) +
           "</dc:title><upnp:class>object.item.audioItem.musicTrack</upnp:class><upnp:artist>" +
           esc(artist) + "</upnp:artist><upnp:album>" + esc(album) + "</upnp:album>" +
           (artwork.empty() ? "" : "<upnp:albumArtURI>" + esc(artwork) + "</upnp:albumArtURI>") +
           "<res protocolInfo=\"http-get:*:" + esc(mime) + ":*\" duration=\"" +
           time_text(duration_ms) + "\">" + esc(url) + "</res></item></DIDL-Lite>";
}
std::optional<std::int64_t> renderer_time_ms(const std::string& text) {
    std::istringstream stream{text};
    std::string hours, minutes, seconds;
    if (!std::getline(stream, hours, ':') || !std::getline(stream, minutes, ':') ||
        !std::getline(stream, seconds)) {
        return {};
    }
    auto number = [](const std::string& s) -> std::optional<std::int64_t> {
        std::int64_t n = 0;
        auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
        if (ec != std::errc{} || end != s.data() + s.size() || n < 0) {
            return {};
        }
        return n;
    };
    const auto h = number(hours), m = number(minutes);
    auto dot = seconds.find('.');
    const auto s = number(seconds.substr(0, dot));
    if (!h || !m || !s || *h > 1000000 || *m >= 60 || *s >= 60) {
        return {};
    }
    std::int64_t fraction = 0;
    if (dot != std::string::npos) {
        auto digits = seconds.substr(dot + 1);
        if (digits.empty() || digits.size() > 3) {
            return {};
        }
        digits.append(3 - digits.size(), '0');
        auto n = number(digits);
        if (!n) {
            return {};
        }
        fraction = *n;
    }
    return ((*h * 60 + *m) * 60 + *s) * 1000 + fraction;
}
StreamRequest renderer_compatible_request(StreamRequest request,
                                          const discovery::UpnpRenderer& renderer,
                                          const int source_sample_rate) {
    constexpr int sonos_maximum_sample_rate = 48'000;
    if (renderer.manufacturer == "Sonos, Inc." && source_sample_rate > sonos_maximum_sample_rate) {
        if (!request.format) {
            request.format = StreamFormat{.codec = StreamCodec::flac, .sample_rate_cap = {}};
        }
        request.format->sample_rate_cap = sonos_maximum_sample_rate;
    }
    return request;
}
UpnpAudition::UpnpAudition(std::string udn, std::shared_ptr<discovery::UpnpControl> control,
                           Prepare prepare, bool background)
    : control_(std::move(control)), prepare_(std::move(prepare)), background_(background) {
    renderer_.udn = std::move(udn);
    renderer_.name = renderer_.udn;
    renderer_.online = false;
    device_ = renderer_;
    if (background_) {
        worker_ = std::thread{[this] { run(); }};
    }
}
UpnpAudition::~UpnpAudition() {
    {
        const std::lock_guard lock{state_mutex_};
        stopping_ = true;
        jobs_.clear();
        wake_.notify_one();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}
core::Result<void> UpnpAudition::submit(Job job, const Topic topic, const bool replaces_track) {
    if (!background_) {
        const std::lock_guard io{io_mutex_};
        return job();
    }
    const std::lock_guard lock{state_mutex_};
    // A new track makes what was queued for the old one moot.
    if (replaces_track) {
        std::erase_if(jobs_, [](const Queued& queued) { return queued.topic == Topic::track; });
    }
    jobs_.push_back({.job = std::move(job), .topic = topic});
    wake_.notify_one();
    return {};
}
void UpnpAudition::failed(const std::uint64_t generation, core::Error error,
                          const bool stop_playing) {
    const std::lock_guard lock{state_mutex_};
    if (generation != generation_) {
        return;
    }
    state_.error = std::move(error);
    if (stop_playing) {
        state_.state = State::paused;
        playing_requested_ = false;
    }
}
void UpnpAudition::update(discovery::UpnpRenderer renderer) {
    {
        const std::lock_guard lock{state_mutex_};
        renderer_ = renderer;
        if (!renderer_.online && state_.state == State::playing) {
            state_.state = State::paused;
        }
    }
    // The worker's own copy, in order with the commands that use it.
    static_cast<void>(submit([this, renderer = std::move(renderer)]() mutable {
        device_ = std::move(renderer);
        sink_.clear();
        gapless_supported_ = true;
        return core::Result<void>{};
    }, Topic::device));
}
bool UpnpAudition::online() const {
    const std::lock_guard lock{state_mutex_};
    return renderer_.online;
}
bool UpnpAudition::wants_playing() const {
    const std::lock_guard lock{state_mutex_};
    return playing_requested_;
}
std::string UpnpAudition::name() const {
    const std::lock_guard lock{state_mutex_};
    return renderer_.name;
}
audio::LocalAuditionSnapshot UpnpAudition::snapshot() const {
    const std::lock_guard lock{state_mutex_};
    return state_;
}
core::Result<Values> UpnpAudition::call(const std::string& action, Values arguments) {
    if (!device_.online || !control_) {
        return std::unexpected(unsupported("UPnP renderer is offline"));
    }
    arguments["InstanceID"] = "0";
    return control_->action(device_.transport, action, arguments);
}
core::Result<RendererTrack> UpnpAudition::prepare(StreamRequest request) {
    if (!device_.online || !control_ || !prepare_) {
        return std::unexpected(unsupported("UPnP renderer is offline"));
    }
    if (sink_.empty()) {
        auto protocols = control_->action(device_.connection, "GetProtocolInfo", {});
        if (!protocols) {
            return std::unexpected(protocols.error());
        }
        sink_ = value(*protocols, "Sink");
    }
    auto format = renderer_stream_format(sink_, stream_content_type(request.raw_path),
                                         request.segment || request.selection.stream_index ||
                                             request.selection.subsong_index);
    if (!format) {
        return std::unexpected(format.error());
    }
    request.format = *format;
    return prepare_(request, device_);
}
bool UpnpAudition::expiring(const RendererTrack& track) const {
    // Ten minutes is a renderer's slowest re-fetch, with room to spare.
    return track.expires &&
           *track.expires - std::chrono::system_clock::now() < std::chrono::minutes{10};
}
void UpnpAudition::adopt_locked(const StreamRequest& source,
                                const std::optional<std::int64_t> duration_ms,
                                const std::uint64_t token, const bool transition) {
    state_.raw_path = source.raw_path;
    state_.selection = source.selection;
    state_.segment = source.segment;
    state_.source_revision =
        core::observe_local_source_revision(source.raw_path).value_or(core::LocalSourceRevision{});
    // Renderer positions are track-relative milliseconds, including converted CUE parts.
    state_.format =
        formats::PcmFormat{.sample_rate = 1000, .channels = 2, .channel_layout = "stereo"};
    state_.position_sample = 0;
    state_.end_sample =
        duration_ms && *duration_ms > 0 ? std::optional{*duration_ms} : std::nullopt;
    state_.occurrence_token = token;
    ++state_.playback_instance;
    if (transition) {
        ++state_.chain_transitions;
    }
    state_.next_raw_path.clear();
    state_.next_segment.reset();
    state_.next_occurrence_token = 0;
    state_.error.reset();
}
core::Result<void> UpnpAudition::load_now(StreamRequest request, const bool playing,
                                          const std::int64_t position_ms,
                                          const std::uint64_t generation) {
    auto track = prepare(std::move(request));
    if (!track) {
        failed(generation, track.error(), true);
        return std::unexpected(track.error());
    }
    // Clear any continuation from a previous queue before replacing its URI.
    static_cast<void>(call("Stop"));
    if (gapless_supported_) {
        static_cast<void>(
            call("SetNextAVTransportURI", {{"NextURI", ""}, {"NextURIMetaData", ""}}));
    }
    auto set = call("SetAVTransportURI",
                    {{"CurrentURI", track->url}, {"CurrentURIMetaData", track->metadata}});
    if (!set) {
        failed(generation, set.error(), true);
        return std::unexpected(set.error());
    }
    current_ = std::move(*track);
    next_.reset();
    deferred_.reset();
    saw_playing_ = false;
    stopped_while_starting_ = 0;
    startup_rejected_ = false;
    pending_seek_ms_ = position_ms > 0 ? std::optional{position_ms} : std::nullopt;
    {
        const std::lock_guard lock{state_mutex_};
        if (generation == generation_ && current_->duration_ms > 0) {
            state_.end_sample = current_->duration_ms;
        }
    }
    if (!playing) {
        return {};
    }
    return start_now(generation);
}
core::Result<void> UpnpAudition::start_now(const std::uint64_t generation) {
    auto started = call("Play", {{"Speed", "1"}});
    if (!started) {
        failed(generation, started.error(), true);
        return std::unexpected(started.error());
    }
    if (pending_seek_ms_) {
        // PLAYING can lag behind the Play response; poll() retries the seek
        // while keeping the requested position visible.
        if (call("Seek", {{"Unit", "REL_TIME"}, {"Target", time_text(*pending_seek_ms_)}})) {
            pending_seek_ms_.reset();
        }
    }
    return {};
}
core::Result<void> UpnpAudition::arm_now(StreamRequest request, const std::uint64_t token) {
    const auto refuse = [this, &request](core::Error error) -> core::Result<void> {
        const std::lock_guard lock{state_mutex_};
        refused_next_ = std::pair{request.raw_path, request.segment};
        if (state_.next_raw_path == request.raw_path && state_.next_segment == request.segment) {
            state_.next_raw_path.clear();
            state_.next_segment.reset();
            state_.next_occurrence_token = 0;
        }
        return std::unexpected(std::move(error));
    };
    if (!gapless_supported_) {
        return refuse(unsupported("renderer does not support gapless continuation"));
    }
    auto track = prepare(request);
    if (!track) {
        return refuse(track.error());
    }
    auto armed = call("SetNextAVTransportURI",
                      {{"NextURI", track->url}, {"NextURIMetaData", track->metadata}});
    if (!armed) {
        if (armed.error().code == core::ErrorCode::unsupported) {
            gapless_supported_ = false;
        }
        return refuse(armed.error());
    }
    next_ = std::move(*track);
    next_token_ = token;
    return {};
}
void UpnpAudition::poll() {
    const std::lock_guard io{io_mutex_};
    std::uint64_t generation = 0;
    {
        const std::lock_guard lock{state_mutex_};
        generation = generation_;
    }
    if (!current_ || !device_.online) {
        return;
    }
    // A fetch that would fail soon is renewed where the renderer is.
    if (expiring(*current_)) {
        bool playing = false;
        std::int64_t position = 0;
        {
            const std::lock_guard lock{state_mutex_};
            playing = playing_requested_;
            position = state_.position_sample;
        }
        static_cast<void>(load_now(current_->source, playing, position, generation));
        return;
    }
    auto transport = call("GetTransportInfo");
    auto position = call("GetPositionInfo");
    const auto event = control_->take_events(device_.udn);
    const auto event_status = value(event, "TransportState");
    const auto status = event_status.empty() && transport
                            ? value(*transport, "CurrentTransportState")
                            : event_status;
    std::optional<core::Error> pending_seek_error;
    std::optional<std::int64_t> completed_seek;
    if (transport && position && status == "PLAYING" && pending_seek_ms_) {
        auto sought =
            call("Seek", {{"Unit", "REL_TIME"}, {"Target", time_text(*pending_seek_ms_)}});
        if (sought) {
            completed_seek = pending_seek_ms_;
            pending_seek_ms_.reset();
        } else {
            pending_seek_error = std::move(sought.error());
        }
    }
    const std::lock_guard lock{state_mutex_};
    // A command came in while the renderer was asked: what it said describes
    // the time before the command, and the next poll asks again.
    if (generation != generation_) {
        return;
    }
    if (!transport || !position) {
        // A network fault never looks like end-of-track.
        state_.state = State::paused;
        state_.error = !transport ? transport.error() : position.error();
        return;
    }
    const auto uri = value(*position, "TrackURI");
    if (next_ && uri == next_->url) {
        current_ = std::move(next_);
        next_.reset();
        adopt_locked(current_->source, current_->duration_ms, next_token_, true);
        saw_playing_ = false;
    } else if (!uri.empty() && uri != current_->url) {
        // Another control point took over. Do not credit or advance its music.
        state_.state = State::paused;
        playing_requested_ = false;
        return;
    }
    if (status == "PLAYING") {
        state_.state = State::playing;
        saw_playing_ = true;
        stopped_while_starting_ = 0;
    } else if (status == "TRANSITIONING") {
        state_.state = State::buffering;
        stopped_while_starting_ = 0;
    } else if (status == "PAUSED_PLAYBACK") {
        state_.state = State::paused;
    } else if ((status == "STOPPED" || status == "NO_MEDIA_PRESENT") && playing_requested_ &&
               saw_playing_) {
        // Also handles devices accepting SetNext but silently ignoring it.
        state_.state = State::ended;
        playing_requested_ = false;
    } else if ((status == "STOPPED" || status == "NO_MEDIA_PRESENT") && playing_requested_ &&
               !saw_playing_ && ++stopped_while_starting_ >= 10) {
        // A few renderers acknowledge Play even when they cannot decode the
        // resource. Do not leave the engine buffering forever in that case.
        state_.state = State::paused;
        state_.error = core::Error{.code = core::ErrorCode::backend,
                                   .message = "UPnP renderer accepted Play but did not start",
                                   .context = {}};
        playing_requested_ = false;
        startup_rejected_ = true;
    }
    if (completed_seek) {
        state_.position_sample = *completed_seek;
    } else if (auto position_ms = renderer_time_ms(value(*position, "RelTime"));
               !pending_seek_ms_ && position_ms) {
        state_.position_sample = *position_ms;
    }
    if (auto duration = renderer_time_ms(value(*position, "TrackDuration"));
        duration && *duration > 0) {
        state_.end_sample = *duration;
    }
    const auto volume = value(event, "Volume");
    int percent = 0;
    const auto [end, ec] = std::from_chars(volume.data(), volume.data() + volume.size(), percent);
    if (ec == std::errc{} && end == volume.data() + volume.size()) {
        state_.volume_percent = std::clamp(percent, 0, 100);
    }
    if (pending_seek_error) {
        state_.error = std::move(pending_seek_error);
    } else if (!startup_rejected_) {
        state_.error.reset();
    }
}
void UpnpAudition::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock{state_mutex_};
            wake_.wait_for(lock, std::chrono::seconds{1},
                           [&] { return stopping_ || !jobs_.empty(); });
            if (stopping_) {
                return;
            }
            if (!jobs_.empty()) {
                job = std::move(jobs_.front().job);
                jobs_.pop_front();
            }
        }
        if (job) {
            const std::lock_guard io{io_mutex_};
            // Failures are in the snapshot by now; nothing waits for them.
            static_cast<void>(job());
            continue;
        }
        poll();
    }
}
core::Result<void> UpnpAudition::load_selected_and_play(std::string path,
                                                        formats::AudioSourceSelection selection,
                                                        std::optional<formats::ReplayGainInfo>) {
    StreamRequest request{.raw_path = std::move(path), .format = {}, .selection = selection,
                          .segment = {}};
    std::uint64_t generation = 0;
    {
        const std::lock_guard lock{state_mutex_};
        generation = ++generation_;
        adopt_locked(request, std::nullopt, 0, false);
        state_.state = State::buffering;
        playing_requested_ = true;
        refused_next_.reset();
    }
    return submit([this, request = std::move(request), generation]() mutable {
        return load_now(std::move(request), true, 0, generation);
    }, Topic::track, true);
}
core::Result<void> UpnpAudition::load_selected_segment_and_play(
    std::string path, formats::AudioSourceSelection selection, formats::SampleRange segment,
    std::optional<formats::ReplayGainInfo>) {
    StreamRequest request{.raw_path = std::move(path), .format = {}, .selection = selection,
                          .segment = segment};
    std::uint64_t generation = 0;
    {
        const std::lock_guard lock{state_mutex_};
        generation = ++generation_;
        adopt_locked(request, std::nullopt, 0, false);
        state_.state = State::buffering;
        playing_requested_ = true;
        refused_next_.reset();
    }
    return submit([this, request = std::move(request), generation]() mutable {
        return load_now(std::move(request), true, 0, generation);
    }, Topic::track, true);
}
core::Result<void> UpnpAudition::restore_paused(std::string path, core::LocalSourceRevision,
                                                formats::AudioSourceSelection selection,
                                                std::optional<formats::SampleRange> segment,
                                                std::int64_t position_ms,
                                                std::optional<formats::ReplayGainInfo>) {
    StreamRequest request{.raw_path = std::move(path), .format = {}, .selection = selection,
                          .segment = segment};
    {
        const std::lock_guard lock{state_mutex_};
        ++generation_;
        adopt_locked(request, std::nullopt, 0, false);
        state_.position_sample = position_ms;
        state_.state = State::paused;
        playing_requested_ = false;
        refused_next_.reset();
    }
    // Nothing is sent: the speaker may be playing for someone else, and the
    // track goes to it when it is played here.
    return submit([this, request = std::move(request), position_ms]() mutable {
        current_.reset();
        next_.reset();
        deferred_ = std::move(request);
        pending_seek_ms_ = position_ms > 0 ? std::optional{position_ms} : std::nullopt;
        return core::Result<void>{};
    }, Topic::track, true);
}
core::Result<void>
UpnpAudition::queue_gapless_next_selected(std::string path, formats::AudioSourceSelection selection,
                                          std::optional<formats::ReplayGainInfo>,
                                          std::uint64_t token) {
    return queue_next(
        {.raw_path = std::move(path), .format = {}, .selection = selection, .segment = {}}, token);
}
core::Result<void> UpnpAudition::queue_gapless_next_selected_segment(
    std::string path, formats::AudioSourceSelection selection, formats::SampleRange segment,
    std::optional<formats::ReplayGainInfo>, std::uint64_t token) {
    return queue_next(
        {.raw_path = std::move(path), .format = {}, .selection = selection, .segment = segment},
        token);
}
core::Result<void> UpnpAudition::queue_next(StreamRequest request, const std::uint64_t token) {
    {
        const std::lock_guard lock{state_mutex_};
        if (refused_next_ && refused_next_->first == request.raw_path &&
            refused_next_->second == request.segment) {
            return std::unexpected(unsupported("the renderer refused this continuation"));
        }
        state_.next_raw_path = request.raw_path;
        state_.next_selection = request.selection;
        state_.next_segment = request.segment;
        state_.next_occurrence_token = token;
    }
    return submit([this, request = std::move(request), token]() mutable {
        return arm_now(std::move(request), token);
    });
}
core::Result<void> UpnpAudition::clear_gapless_next() {
    {
        const std::lock_guard lock{state_mutex_};
        state_.next_raw_path.clear();
        state_.next_segment.reset();
        state_.next_occurrence_token = 0;
    }
    return submit([this] {
        const bool held = next_.has_value();
        next_.reset();
        auto cleared = call("SetNextAVTransportURI", {{"NextURI", ""}, {"NextURIMetaData", ""}});
        return cleared || !held ? core::Result<void>{} : std::unexpected(cleared.error());
    });
}
core::Result<void> UpnpAudition::play() {
    std::uint64_t generation = 0;
    {
        const std::lock_guard lock{state_mutex_};
        generation = ++generation_;
        playing_requested_ = true;
        state_.error.reset();
        state_.state = State::buffering;
    }
    return submit([this, generation]() -> core::Result<void> {
        saw_playing_ = false;
        stopped_while_starting_ = 0;
        startup_rejected_ = false;
        // A restored track goes to the speaker now, or a stale ticket is
        // renewed, where it was left.
        if (deferred_ || (current_ && expiring(*current_))) {
            auto source = deferred_ ? *deferred_ : current_->source;
            std::int64_t position = 0;
            {
                const std::lock_guard lock{state_mutex_};
                position = state_.position_sample;
            }
            return load_now(std::move(source), true, position, generation);
        }
        return start_now(generation);
    });
}
core::Result<void> UpnpAudition::pause() {
    std::uint64_t generation = 0;
    {
        const std::lock_guard lock{state_mutex_};
        generation = ++generation_;
        playing_requested_ = false;
        state_.state = State::paused;
    }
    return submit([this, generation]() -> core::Result<void> {
        if (!current_) {
            return {};
        }
        auto paused = call("Pause");
        if (!paused) {
            failed(generation, paused.error(), false);
            return std::unexpected(paused.error());
        }
        return {};
    });
}
core::Result<void> UpnpAudition::stop() {
    {
        const std::lock_guard lock{state_mutex_};
        ++generation_;
        state_.raw_path.clear();
        state_.next_raw_path.clear();
        state_.state = State::empty;
        playing_requested_ = false;
    }
    return submit([this]() -> core::Result<void> {
        const bool sent = current_.has_value();
        current_.reset();
        next_.reset();
        deferred_.reset();
        pending_seek_ms_.reset();
        if (!sent) {
            return {};
        }
        auto stopped = call("Stop");
        return stopped ? core::Result<void>{} : std::unexpected(stopped.error());
    }, Topic::track, true);
}
core::Result<void> UpnpAudition::seek_to_seconds(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 3600000000.0) {
        return std::unexpected(unsupported("invalid seek time"));
    }
    const auto ms = static_cast<std::int64_t>(seconds * 1000);
    std::uint64_t generation = 0;
    bool playing = false;
    {
        const std::lock_guard lock{state_mutex_};
        generation = ++generation_;
        state_.position_sample = ms;
        playing = playing_requested_;
    }
    return submit([this, ms, generation, playing]() -> core::Result<void> {
        if (deferred_) {
            pending_seek_ms_ = ms;
            return {};
        }
        // A renderer fetches again to seek: a stale ticket is renewed there.
        if (current_ && expiring(*current_)) {
            return load_now(current_->source, playing, ms, generation);
        }
        auto sought = call("Seek", {{"Unit", "REL_TIME"}, {"Target", time_text(ms)}});
        if (!sought) {
            failed(generation, sought.error(), false);
            return std::unexpected(sought.error());
        }
        return {};
    });
}
core::Result<void> UpnpAudition::set_volume_percent(int percent) {
    percent = std::clamp(percent, 0, 100);
    {
        const std::lock_guard lock{state_mutex_};
        state_.volume_percent = percent;
    }
    return submit([this, percent]() -> core::Result<void> {
        if (!control_ || !device_.online) {
            return std::unexpected(unsupported("UPnP renderer is offline"));
        }
        auto set = control_->action(device_.rendering, "SetVolume",
                                    {{"InstanceID", "0"},
                                     {"Channel", "Master"},
                                     {"DesiredVolume", std::to_string(percent)}});
        return set ? core::Result<void>{} : std::unexpected(set.error());
    }, Topic::device);
}
// The mode is the engine's setting, kept for whatever output plays next; this
// one plays at unity gain whatever it is, as its replay_gain: false says. A
// refusal here was shown as an error every time a client set ReplayGain.
core::Result<void> UpnpAudition::set_replay_gain_mode(audio::ReplayGainMode) { return {}; }
core::Result<void> UpnpAudition::set_replay_gain_preamps(audio::ReplayGainPreamps) { return {}; }
core::Result<void> UpnpAudition::set_buffer_config(audio::PlaybackBufferDurationConfig) {
    return {};
}
core::Result<void> UpnpAudition::refresh_output_devices() { return {}; }
core::Result<void> UpnpAudition::set_output_target(std::optional<std::string>) {
    return std::unexpected(unsupported("select a renderer through outputs.select"));
}
} // namespace trackknife::output
