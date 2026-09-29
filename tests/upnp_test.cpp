// SPDX-License-Identifier: GPL-3.0-only
#include "trackknife/discovery/upnp.hpp"
#include "trackknife/output/upnp_audition.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>

using namespace trackknife;
namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
class Renderer final : public discovery::UpnpControl {
  public:
    std::string state{"STOPPED"}, uri, next, position{"0:00:00"};
    std::string sink{"http-get:*:audio/flac:*,http-get:*:audio/wav:*"};
    bool reject_next{false}, reject_seek_while_stopped{false}, fail{false};
    int volume{100};
    std::vector<std::string> actions;
    discovery::UpnpValues report;
    core::Result<discovery::UpnpValues> action(const discovery::UpnpService&,
                                               const std::string& name,
                                               const discovery::UpnpValues& args) override {
        actions.push_back(name);
        if (fail) {
            return std::unexpected(
                core::Error{.code = core::ErrorCode::io, .message = "disconnected", .context = {}});
        }
        if (name == "GetProtocolInfo") {
            return discovery::UpnpValues{{"Sink", sink}};
        }
        require(args.at("InstanceID") == "0", "transport addresses instance zero");
        if (name == "SetAVTransportURI") {
            uri = args.at("CurrentURI");
            state = "STOPPED";
        }
        if (name == "SetNextAVTransportURI") {
            if (reject_next) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::unsupported, .message = "401", .context = {}});
            }
            next = args.at("NextURI");
        }
        if (name == "Play") {
            state = "PLAYING";
        }
        if (name == "Pause") {
            state = "PAUSED_PLAYBACK";
        }
        if (name == "Stop") {
            state = "STOPPED";
        }
        if (name == "Seek") {
            if (reject_seek_while_stopped && state != "PLAYING") {
                return std::unexpected(core::Error{.code = core::ErrorCode::unsupported,
                                                   .message = "seek needs PLAYING",
                                                   .context = {}});
            }
            position = args.at("Target");
        }
        if (name == "SetVolume") {
            volume = std::stoi(args.at("DesiredVolume"));
        }
        if (name == "GetTransportInfo") {
            return discovery::UpnpValues{{"CurrentTransportState", state}};
        }
        if (name == "GetPositionInfo") {
            return discovery::UpnpValues{
                {"TrackURI", uri}, {"RelTime", position}, {"TrackDuration", "0:03:00"}};
        }
        return discovery::UpnpValues{};
    }
    discovery::UpnpValues take_events(const std::string&) override {
        auto event = std::move(report);
        report.clear();
        return event;
    }
};
} // namespace
int main() {
    const std::string description = R"(<root xmlns="urn:schemas-upnp-org:device-1-0"><device>
      <deviceType>urn:schemas-upnp-org:device:MediaRenderer:2</deviceType><UDN>uuid:speaker</UDN>
      <friendlyName>Kitchen &amp; dining</friendlyName><serviceList>
      <service><serviceType>urn:schemas-upnp-org:service:AVTransport:2</serviceType>
      <controlURL>/av/control</controlURL><eventSubURL>events</eventSubURL></service>
      <service><serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>
      <controlURL>/connection</controlURL></service></serviceList></device></root>)";
    auto parsed =
        discovery::parse_upnp_renderer(description, "http://192.0.2.2/device.xml", "192.0.2.1");
    require(parsed && parsed->udn == "uuid:speaker" && parsed->name == "Kitchen & dining",
            "description preserves UDN and decodes XML");
    require(parsed->transport.control_url == "http://192.0.2.2/av/control" &&
                parsed->transport.event_url == "http://192.0.2.2/events",
            "service URLs resolve against description location");
    require(!discovery::parse_upnp_renderer("<!DOCTYPE x><root/>", "http://host/a", "host"),
            "reject document types");
    require(!discovery::parse_upnp_renderer("<root/>", "http://host/a", "host"),
            "missing services rejected");
    const auto change = discovery::parse_upnp_last_change(
        R"(<Event><InstanceID val="3"><TransportState val="STOPPED"/></InstanceID><InstanceID val="0"><TransportState val="PLAYING"/><Volume channel="Master" val="32"/><Volume channel="LF" val="99"/></InstanceID></Event>)");
    require(change.at("TransportState") == "PLAYING" && change.at("Volume") == "32",
            "GENA follows only the correct instance and channel");
    const auto outer = discovery::parse_upnp_last_change(
        "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\"><e:property><LastChange>" +
        discovery::upnp_xml_escape("<Event><InstanceID val=\"0\"><TransportState "
                                   "val=\"PAUSED_PLAYBACK\"/></InstanceID></Event>") +
        "</LastChange></e:property></e:propertyset>");
    require(outer.at("TransportState") == "PAUSED_PLAYBACK",
            "escaped LastChange is decoded exactly once");
    require(output::renderer_time_ms("12:34:56.125") == 45296125, "fractional duration");
    require(!output::renderer_time_ms("NOT_IMPLEMENTED") && !output::renderer_time_ms("0:90:01"),
            "invalid renderer times do not become zero");
    auto format = output::renderer_stream_format("http-get:*:audio/mpeg:*,http-get:*:audio/flac:*",
                                                 "audio/mpeg", false);
    require(format && !*format, "compatible originals stay original");
    format = output::renderer_stream_format("http-get:*:audio/flac:*", "audio/flac", true);
    require(format && *format && (**format).codec == output::StreamCodec::flac,
            "segments are converted even when the codec is supported");
    format = output::renderer_stream_format("http-get:*:audio/x-wav:*", "application/octet-stream",
                                            false);
    require(format && *format && (**format).codec == output::StreamCodec::wav,
            "unknown formats get PCM WAV fallback");
    require(!output::renderer_stream_format("http-get:*:*:*", "audio/flac", false),
            "wildcards do not invent codec support");
    require(!output::renderer_stream_format("rtsp-rtp-udp:*:audio/flac:*", "audio/flac", false),
            "only HTTP sinks qualify");
    const auto didl = output::renderer_didl("http://host/stream?a=1&b=2", "audio/flac", 180000,
                                            "A < B", "Artist", "Album", "http://host/cover");
    require(didl.find("duration=\"0:03:00\"") != std::string::npos &&
                didl.find("A &lt; B") != std::string::npos &&
                didl.find("&amp;b=2") != std::string::npos,
            "DIDL escapes metadata, URL and carries duration");

    auto renderer = std::make_shared<Renderer>();
    unsigned occurrence = 0;
    output::UpnpAudition audition{
        "uuid:speaker", renderer,
        [&occurrence](const output::StreamRequest& source,
                      const discovery::UpnpRenderer&) -> core::Result<output::RendererTrack> {
            return output::RendererTrack{.source = source,
                                         .url = "http://engine/stream?ticket=one&occurrence=" +
                                                std::to_string(++occurrence),
                                         .metadata = "didl",
                                         .duration_ms = 180000};
        },
        false};
    require(!audition.online(), "restored output starts offline");
    audition.update(*parsed);
    require(audition.load_selected_and_play("/track.flac", {}, {}).has_value(), "load renderer");
    audition.poll();
    require(audition.snapshot().state == audio::LocalAuditionState::playing,
            "renderer reports playing");
    renderer->report["TransportState"] = "PAUSED_PLAYBACK";
    audition.poll();
    require(audition.snapshot().state == audio::LocalAuditionState::paused,
            "LastChange transport state takes precedence over polling");
    audition.poll();
    require(audition.snapshot().state == audio::LocalAuditionState::playing,
            "polling resumes after the event is consumed");
    require(audition.queue_gapless_next_selected("/track.flac", {}, {}, 42).has_value(),
            "arm duplicate occurrence");
    const auto first = renderer->uri;
    renderer->uri = renderer->next;
    require(renderer->uri != first, "duplicate occurrences have distinct URLs");
    audition.poll();
    require(audition.snapshot().chain_transitions == 1 &&
                audition.snapshot().occurrence_token == 42 &&
                audition.snapshot().next_raw_path.empty(),
            "one confirmed handoff carries occurrence identity");
    audition.poll();
    require(audition.snapshot().chain_transitions == 1,
            "repeated reports do not count handoff twice");
    require(audition.seek_to_seconds(12).has_value(), "seek works");
    audition.poll();
    require(audition.snapshot().position_sample == 12000, "position reported in milliseconds");
    require(!audition.seek_to_seconds(std::numeric_limits<double>::quiet_NaN()),
            "invalid seek rejected");
    require(audition.pause().has_value() &&
                audition.snapshot().state == audio::LocalAuditionState::paused,
            "pause");
    require(audition.play().has_value(), "resume");
    audition.poll();
    renderer->fail = true;
    audition.poll();
    require(audition.snapshot().state == audio::LocalAuditionState::paused &&
                audition.snapshot().error,
            "network failure pauses, never advances");
    renderer->fail = false;
    audition.poll();
    require(audition.snapshot().state == audio::LocalAuditionState::playing, "polling recovers");
    renderer->reject_next = true;
    require(!audition.queue_gapless_next_selected("/next.flac", {}, {}, 43),
            "unsupported next returns ordinary progression to the player");
    renderer->state = "STOPPED";
    audition.poll();
    require(audition.snapshot().state == audio::LocalAuditionState::ended,
            "renderer end drives normal progression");
    require(!audition.set_replay_gain_mode(audio::ReplayGainMode::track),
            "ReplayGain explicitly unsupported");
    require(renderer->volume == 100 && audition.snapshot().effective_replay_gain_multiplier == 1.0F,
            "ReplayGain never changes device volume");
    require(audition.set_volume_percent(38).has_value() && renderer->volume == 38,
            "user volume works");
    require(audition.restore_paused("/track.flac", {}, {}, {}, 20000, {}).has_value() &&
                renderer->state == "STOPPED",
            "restore seeks without playing audio");
    renderer->state = "STOPPED";
    audition.poll();
    require(audition.snapshot().state == audio::LocalAuditionState::paused,
            "restored stopped transport does not advance queue");
    renderer->position = "0:00:00";
    renderer->reject_seek_while_stopped = true;
    require(audition.restore_paused("/track.flac", {}, {}, {}, 42000, {}).has_value(),
            "restore defers a seek rejected while stopped");
    require(audition.snapshot().state == audio::LocalAuditionState::paused &&
                audition.snapshot().position_sample == 42000,
            "deferred restore keeps its requested position");
    require(audition.play().has_value() && renderer->position == "0:00:42",
            "play makes the renderer seekable before restoring its position");
    audition.poll();
    require(audition.snapshot().state == audio::LocalAuditionState::playing &&
                audition.snapshot().position_sample == 42000,
            "output switching resumes at the saved position");
    parsed->online = false;
    audition.update(*parsed);
    require(!audition.online(), "byebye marks output offline");
    std::cout << "UPnP tests passed\n";
}
