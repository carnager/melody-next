// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/file_work_wire.hpp"

#include "trackknife/protocol/dispatch.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace trackknife::engine::wire {
namespace {

[[nodiscard]] core::Error malformed(std::string what) {
    return core::Error{.code = core::ErrorCode::invalid_argument,
                       .message = "malformed file-work document: " + std::move(what),
                       .context = {}};
}

[[nodiscard]] core::Result<const Json*> member(const Json& object, const std::string_view name) {
    if (!object.is_object()) {
        return std::unexpected(malformed("expected an object holding " + std::string{name}));
    }
    const auto found = object.find(name);
    if (found == object.end()) {
        return std::unexpected(malformed(std::string{name} + " is missing"));
    }
    return &*found;
}

// A member that may be absent or null.
[[nodiscard]] const Json* optional_member(const Json& object, const std::string_view name) {
    if (!object.is_object()) {
        return nullptr;
    }
    const auto found = object.find(name);
    return found == object.end() || found->is_null() ? nullptr : &*found;
}

[[nodiscard]] core::Result<std::uint64_t> unsigned_of(const Json& value, const std::string_view name) {
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0) {
        return static_cast<std::uint64_t>(value.get<std::int64_t>());
    }
    return std::unexpected(malformed(std::string{name} + " must be a non-negative integer"));
}

[[nodiscard]] core::Result<std::int64_t> signed_of(const Json& value, const std::string_view name) {
    if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }
    return std::unexpected(malformed(std::string{name} + " must be an integer"));
}

[[nodiscard]] core::Result<bool> bool_of(const Json& value, const std::string_view name) {
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    return std::unexpected(malformed(std::string{name} + " must be true or false"));
}

[[nodiscard]] core::Result<std::string> bytes_of(const Json& value, const std::string_view name) {
    if (value.is_string()) {
        if (auto decoded = protocol::decode_raw_path(value.get<std::string>())) {
            return std::move(*decoded);
        }
    }
    return std::unexpected(malformed(std::string{name} + " must be encoded bytes"));
}

[[nodiscard]] core::Result<std::string> text_of(const Json& value, const std::string_view name) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return std::unexpected(malformed(std::string{name} + " must be text"));
}

[[nodiscard]] Json encode_double(const double value) {
    if (std::isnan(value)) {
        return "nan";
    }
    if (std::isinf(value)) {
        return value < 0 ? "-inf" : "inf";
    }
    return value;
}

[[nodiscard]] core::Result<double> double_of(const Json& value, const std::string_view name) {
    if (value.is_number()) {
        return value.get<double>();
    }
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (text == "inf") {
            return std::numeric_limits<double>::infinity();
        }
        if (text == "-inf") {
            return -std::numeric_limits<double>::infinity();
        }
        if (text == "nan") {
            return std::numeric_limits<double>::quiet_NaN();
        }
    }
    return std::unexpected(malformed(std::string{name} + " must be a number"));
}

constexpr std::array<std::pair<loudness::LoudnessScanState, std::string_view>, 5> scan_states{{
    {loudness::LoudnessScanState::pending, "pending"},
    {loudness::LoudnessScanState::running, "running"},
    {loudness::LoudnessScanState::analyzed, "analyzed"},
    {loudness::LoudnessScanState::failed, "failed"},
    {loudness::LoudnessScanState::cancelled, "cancelled"},
}};

[[nodiscard]] std::string_view scan_state_name(const loudness::LoudnessScanState state) {
    for (const auto& [value, name] : scan_states) {
        if (value == state) {
            return name;
        }
    }
    return "failed";
}

[[nodiscard]] core::Result<loudness::LoudnessScanState> scan_state_of(const Json& value) {
    if (value.is_string()) {
        for (const auto& [state, name] : scan_states) {
            if (name == value.get_ref<const std::string&>()) {
                return state;
            }
        }
    }
    return std::unexpected(malformed("state is not a scan state"));
}

[[nodiscard]] Json encode_optional_error(const std::optional<core::Error>& error) {
    return error ? encode(*error) : Json(nullptr);
}

[[nodiscard]] core::Result<std::optional<core::Error>> optional_error_of(const Json& object,
                                                                         std::string_view name) {
    const auto* value = optional_member(object, name);
    if (value == nullptr) {
        return std::optional<core::Error>{};
    }
    auto decoded = decode_error(*value);
    if (!decoded) {
        return std::unexpected(std::move(decoded.error()));
    }
    return std::optional{std::move(*decoded)};
}

} // namespace

// Error text is for people: it travels as displayable text, so a message
// naming an undecodable path comes back escaped rather than byte for byte.
Json encode(const core::Error& error) {
    auto context = Json::array();
    for (const auto& pair : error.context) {
        context.push_back(Json::array(
            {protocol::displayable_text(pair.key), protocol::displayable_text(pair.value)}));
    }
    return Json{{"code", std::string{protocol::error_code_name(error.code)}},
                {"message", protocol::displayable_text(error.message)},
                {"context", std::move(context)}};
}

core::Result<core::Error> decode_error(const Json& value) {
    auto code = member(value, "code");
    auto message = member(value, "message");
    if (!code || !message) {
        return std::unexpected(std::move(code ? message.error() : code.error()));
    }
    auto code_text = text_of(**code, "code");
    auto message_text = text_of(**message, "message");
    if (!code_text || !message_text) {
        return std::unexpected(std::move(code_text ? message_text.error() : code_text.error()));
    }
    core::Error error{.code = protocol::error_code_from_name(*code_text),
                      .message = std::move(*message_text),
                      .context = {}};
    if (const auto* context = optional_member(value, "context")) {
        if (!context->is_array()) {
            return std::unexpected(malformed("context must be a list of pairs"));
        }
        for (const auto& pair : *context) {
            if (!pair.is_array() || pair.size() != 2U || !pair[0].is_string() ||
                !pair[1].is_string()) {
                return std::unexpected(malformed("context must be a list of pairs"));
            }
            error.context.push_back({pair[0].get<std::string>(), pair[1].get<std::string>()});
        }
    }
    return error;
}

Json encode(const core::LocalSourceRevision& revision) {
    return Json::array({revision.device, revision.inode, revision.size,
                        revision.modification_time_seconds,
                        revision.modification_time_nanoseconds});
}

core::Result<core::LocalSourceRevision> decode_revision(const Json& value) {
    if (!value.is_array() || value.size() != 5U) {
        return std::unexpected(malformed("a revision is five numbers"));
    }
    auto device = unsigned_of(value[0], "revision device");
    auto inode = unsigned_of(value[1], "revision inode");
    auto size = unsigned_of(value[2], "revision size");
    auto seconds = signed_of(value[3], "revision seconds");
    auto nanoseconds = signed_of(value[4], "revision nanoseconds");
    if (!device || !inode || !size || !seconds || !nanoseconds) {
        return std::unexpected(malformed("a revision is five numbers"));
    }
    return core::LocalSourceRevision{.device = *device,
                                     .inode = *inode,
                                     .size = *size,
                                     .modification_time_seconds = *seconds,
                                     .modification_time_nanoseconds = *nanoseconds};
}

Json encode(const formats::AudioSourceSelection& selection) {
    return Json{{"stream_index", selection.stream_index ? Json(*selection.stream_index) : Json()},
                {"subsong_index",
                 selection.subsong_index ? Json(*selection.subsong_index) : Json()}};
}

core::Result<formats::AudioSourceSelection> decode_selection(const Json& value) {
    if (!value.is_object()) {
        return std::unexpected(malformed("a selection is an object"));
    }
    formats::AudioSourceSelection selection;
    for (const auto& [name, target] :
         {std::pair{"stream_index", &selection.stream_index},
          std::pair{"subsong_index", &selection.subsong_index}}) {
        if (const auto* index = optional_member(value, name)) {
            auto number = signed_of(*index, name);
            if (!number || *number < std::numeric_limits<int>::min() ||
                *number > std::numeric_limits<int>::max()) {
                return std::unexpected(malformed(std::string{name} + " must be an index"));
            }
            *target = static_cast<int>(*number);
        }
    }
    return selection;
}

Json encode(const formats::SampleRange& range) {
    return Json{{"start_sample", range.start_sample},
                {"end_sample", range.end_sample ? Json(*range.end_sample) : Json()}};
}

core::Result<formats::SampleRange> decode_range(const Json& value) {
    auto start = member(value, "start_sample");
    if (!start) {
        return std::unexpected(std::move(start.error()));
    }
    auto start_sample = signed_of(**start, "start_sample");
    if (!start_sample) {
        return std::unexpected(std::move(start_sample.error()));
    }
    formats::SampleRange range{.start_sample = *start_sample, .end_sample = std::nullopt};
    if (const auto* end = optional_member(value, "end_sample")) {
        auto end_sample = signed_of(*end, "end_sample");
        if (!end_sample) {
            return std::unexpected(std::move(end_sample.error()));
        }
        range.end_sample = *end_sample;
    }
    return range;
}

Json encode(const loudness::LoudnessScanItem& item) {
    return Json{{"item_index", item.item_index},
                {"path", protocol::encode_raw_path(item.raw_path)},
                {"selection", encode(item.selection)},
                {"range", item.range ? encode(*item.range) : Json()},
                {"album_key",
                 item.album_key ? Json(protocol::encode_raw_path(*item.album_key)) : Json()}};
}

core::Result<loudness::LoudnessScanItem> decode_scan_item(const Json& value) {
    auto index = member(value, "item_index");
    auto path = member(value, "path");
    auto selection = member(value, "selection");
    if (!index || !path || !selection) {
        return std::unexpected(
            std::move(!index ? index.error() : !path ? path.error() : selection.error()));
    }
    auto item_index = unsigned_of(**index, "item_index");
    auto raw_path = bytes_of(**path, "path");
    auto chosen = decode_selection(**selection);
    if (!item_index || !raw_path || !chosen) {
        return std::unexpected(std::move(!item_index ? item_index.error()
                                         : !raw_path ? raw_path.error()
                                                     : chosen.error()));
    }
    loudness::LoudnessScanItem item{.item_index = static_cast<std::size_t>(*item_index),
                                    .raw_path = std::move(*raw_path),
                                    .selection = *chosen,
                                    .range = std::nullopt,
                                    .album_key = std::nullopt};
    if (const auto* range = optional_member(value, "range")) {
        auto decoded = decode_range(*range);
        if (!decoded) {
            return std::unexpected(std::move(decoded.error()));
        }
        item.range = *decoded;
    }
    if (const auto* key = optional_member(value, "album_key")) {
        auto decoded = bytes_of(*key, "album_key");
        if (!decoded) {
            return std::unexpected(std::move(decoded.error()));
        }
        item.album_key = std::move(*decoded);
    }
    return item;
}

Json encode(const loudness::LoudnessScanOptions& options) {
    return Json{{"measure_true_peak", options.measure_true_peak},
                {"maximum_parallelism", options.maximum_parallelism}};
}

core::Result<loudness::LoudnessScanOptions> decode_scan_options(const Json& value) {
    loudness::LoudnessScanOptions options;
    if (!value.is_object()) {
        return std::unexpected(malformed("options are an object"));
    }
    if (const auto* true_peak = optional_member(value, "measure_true_peak")) {
        auto decoded = bool_of(*true_peak, "measure_true_peak");
        if (!decoded) {
            return std::unexpected(std::move(decoded.error()));
        }
        options.measure_true_peak = *decoded;
    }
    if (const auto* parallelism = optional_member(value, "maximum_parallelism")) {
        auto decoded = unsigned_of(*parallelism, "maximum_parallelism");
        if (!decoded) {
            return std::unexpected(std::move(decoded.error()));
        }
        options.maximum_parallelism = static_cast<std::size_t>(*decoded);
    }
    return options;
}

Json encode(const loudness::LoudnessScanResult& result) {
    auto tracks = Json::array();
    for (const auto& track : result.tracks) {
        Json loudness = nullptr;
        if (track.loudness) {
            loudness = Json{{"integrated_lufs", encode_double(track.loudness->integrated_lufs)},
                            {"sample_peak", encode_double(track.loudness->sample_peak)},
                            {"true_peak", track.loudness->true_peak
                                              ? encode_double(*track.loudness->true_peak)
                                              : Json()}};
        }
        tracks.push_back(
            Json{{"item_index", track.item_index},
                 {"path", protocol::encode_raw_path(track.raw_path)},
                 {"state", std::string{scan_state_name(track.state)}},
                 {"loudness", std::move(loudness)},
                 {"opus", track.opus},
                 {"source_revision",
                  track.source_revision ? encode(*track.source_revision) : Json()},
                 {"issue", encode_optional_error(track.issue)}});
    }
    auto albums = Json::array();
    for (const auto& album : result.albums) {
        albums.push_back(Json{
            {"album_key", protocol::encode_raw_path(album.album_key)},
            {"item_indexes", album.item_indexes},
            {"integrated_lufs",
             album.integrated_lufs ? encode_double(*album.integrated_lufs) : Json()},
            {"sample_peak", encode_double(album.sample_peak)},
            {"true_peak", album.true_peak ? encode_double(*album.true_peak) : Json()},
            {"issue", encode_optional_error(album.issue)}});
    }
    return Json{{"tracks", std::move(tracks)},
                {"albums", std::move(albums)},
                {"cancellation_requested", result.cancellation_requested}};
}

core::Result<loudness::LoudnessScanResult> decode_scan_result(const Json& value) {
    auto tracks = member(value, "tracks");
    auto albums = member(value, "albums");
    if (!tracks || !albums) {
        return std::unexpected(std::move(tracks ? albums.error() : tracks.error()));
    }
    if (!(*tracks)->is_array() || !(*albums)->is_array()) {
        return std::unexpected(malformed("tracks and albums are lists"));
    }
    loudness::LoudnessScanResult result;
    for (const auto& entry : **tracks) {
        auto index = member(entry, "item_index");
        auto path = member(entry, "path");
        auto state = member(entry, "state");
        if (!index || !path || !state) {
            return std::unexpected(malformed("a track lacks its index, path or state"));
        }
        auto item_index = unsigned_of(**index, "item_index");
        auto raw_path = bytes_of(**path, "path");
        auto scan_state = scan_state_of(**state);
        if (!item_index || !raw_path || !scan_state) {
            return std::unexpected(malformed("a track's index, path or state is malformed"));
        }
        loudness::LoudnessTrackScan track;
        track.item_index = static_cast<std::size_t>(*item_index);
        track.raw_path = std::move(*raw_path);
        track.state = *scan_state;
        if (const auto* measured = optional_member(entry, "loudness")) {
            auto lufs = member(*measured, "integrated_lufs");
            auto peak = member(*measured, "sample_peak");
            if (!lufs || !peak) {
                return std::unexpected(malformed("a loudness lacks its values"));
            }
            auto integrated = double_of(**lufs, "integrated_lufs");
            auto sample_peak = double_of(**peak, "sample_peak");
            if (!integrated || !sample_peak) {
                return std::unexpected(malformed("a loudness value is not a number"));
            }
            loudness::TrackLoudness loudness{.integrated_lufs = *integrated,
                                             .sample_peak = *sample_peak,
                                             .true_peak = std::nullopt};
            if (const auto* true_peak = optional_member(*measured, "true_peak")) {
                auto decoded = double_of(*true_peak, "true_peak");
                if (!decoded) {
                    return std::unexpected(std::move(decoded.error()));
                }
                loudness.true_peak = *decoded;
            }
            track.loudness = loudness;
        }
        if (const auto* opus = optional_member(entry, "opus")) {
            auto decoded = bool_of(*opus, "opus");
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            track.opus = *decoded;
        }
        if (const auto* revision = optional_member(entry, "source_revision")) {
            auto decoded = decode_revision(*revision);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            track.source_revision = *decoded;
        }
        auto issue = optional_error_of(entry, "issue");
        if (!issue) {
            return std::unexpected(std::move(issue.error()));
        }
        track.issue = std::move(*issue);
        result.tracks.push_back(std::move(track));
    }
    for (const auto& entry : **albums) {
        auto key = member(entry, "album_key");
        auto indexes = member(entry, "item_indexes");
        auto peak = member(entry, "sample_peak");
        if (!key || !indexes || !peak || !(*indexes)->is_array()) {
            return std::unexpected(malformed("an album lacks its key, members or peak"));
        }
        auto album_key = bytes_of(**key, "album_key");
        auto sample_peak = double_of(**peak, "sample_peak");
        if (!album_key || !sample_peak) {
            return std::unexpected(malformed("an album's key or peak is malformed"));
        }
        loudness::LoudnessAlbumScan album;
        album.album_key = std::move(*album_key);
        album.sample_peak = *sample_peak;
        for (const auto& index : **indexes) {
            auto decoded = unsigned_of(index, "item_indexes");
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            album.item_indexes.push_back(static_cast<std::size_t>(*decoded));
        }
        if (const auto* lufs = optional_member(entry, "integrated_lufs")) {
            auto decoded = double_of(*lufs, "integrated_lufs");
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            album.integrated_lufs = *decoded;
        }
        if (const auto* true_peak = optional_member(entry, "true_peak")) {
            auto decoded = double_of(*true_peak, "true_peak");
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            album.true_peak = *decoded;
        }
        auto issue = optional_error_of(entry, "issue");
        if (!issue) {
            return std::unexpected(std::move(issue.error()));
        }
        album.issue = std::move(*issue);
        result.albums.push_back(std::move(album));
    }
    if (const auto* cancelled = optional_member(value, "cancellation_requested")) {
        auto decoded = bool_of(*cancelled, "cancellation_requested");
        if (!decoded) {
            return std::unexpected(std::move(decoded.error()));
        }
        result.cancellation_requested = *decoded;
    }
    return result;
}

} // namespace trackknife::engine::wire
