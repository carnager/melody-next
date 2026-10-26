// SPDX-License-Identifier: GPL-3.0-only

// ADR-0336: conversions on the wire -- the items, options and outcome of a
// conversion the asker's engine carries out.

#include "trackknife/engine/file_work_wire.hpp"

#include "file_work_wire_internal.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace trackknife::engine::wire {
namespace {

using namespace detail;

constexpr std::array<std::pair<convert::ConversionChannelPolicy, std::string_view>, 3> channels{{
    {convert::ConversionChannelPolicy::keep, "keep"},
    {convert::ConversionChannelPolicy::mono, "mono"},
    {convert::ConversionChannelPolicy::stereo, "stereo"},
}};

constexpr std::array<std::pair<convert::ConversionGainMode, std::string_view>, 3> gains{{
    {convert::ConversionGainMode::none, "none"},
    {convert::ConversionGainMode::track, "track"},
    {convert::ConversionGainMode::album, "album"},
}};

constexpr std::array<std::pair<convert::ConversionScanState, std::string_view>, 5> states{{
    {convert::ConversionScanState::pending, "pending"},
    {convert::ConversionScanState::running, "running"},
    {convert::ConversionScanState::converted, "converted"},
    {convert::ConversionScanState::failed, "failed"},
    {convert::ConversionScanState::cancelled, "cancelled"},
}};

// A whole number of the member, absent or null: none.
template <typename Number>
[[nodiscard]] std::optional<Number> optional_number(Reader& in, const std::string_view name) {
    const auto* value = in.find(name);
    if (value == nullptr || value->is_null()) {
        return std::nullopt;
    }
    if (!value->is_number_integer()) {
        in.fail(std::string{name} + " must be a whole number");
        return std::nullopt;
    }
    const auto number = value->get<std::int64_t>();
    if (number < static_cast<std::int64_t>(std::numeric_limits<Number>::min()) ||
        (number > 0 && static_cast<std::uint64_t>(number) >
                           static_cast<std::uint64_t>(std::numeric_limits<Number>::max()))) {
        in.fail(std::string{name} + " is out of range");
        return std::nullopt;
    }
    return static_cast<Number>(number);
}

template <typename Number> [[nodiscard]] Json optional_json(const std::optional<Number>& value) {
    return value ? Json(*value) : Json();
}

[[nodiscard]] convert::EncoderPreset read_preset(Reader& in) {
    return convert::EncoderPreset{
        .id = in.text("id"),
        .version = optional_number<int>(in, "version").value_or(1),
        .display_name = in.text("display_name"),
        .codec_name = in.text("codec"),
        .container_name = in.text("container"),
        .file_extension = in.text("extension"),
        .lossless = in.flag("lossless"),
        .bit_rate = optional_number<std::int64_t>(in, "bit_rate"),
        .vbr_quality = optional_number<int>(in, "vbr_quality"),
        .sample_format_hint = in.optional_text("sample_format").value_or(std::string{}),
        .carries_artwork = in.find("carries_artwork") == nullptr || in.flag("carries_artwork"),
    };
}

} // namespace

Json encode(const convert::EncoderPreset& preset) {
    return Json{{"id", encode_text(preset.id)},
                {"version", preset.version},
                {"display_name", encode_text(preset.display_name)},
                {"codec", encode_text(preset.codec_name)},
                {"container", encode_text(preset.container_name)},
                {"extension", encode_text(preset.file_extension)},
                {"lossless", preset.lossless},
                {"bit_rate", optional_json(preset.bit_rate)},
                {"vbr_quality", optional_json(preset.vbr_quality)},
                {"sample_format", encode_text(preset.sample_format_hint)},
                {"carries_artwork", preset.carries_artwork}};
}

core::Result<convert::EncoderPreset> decode_preset(const Json& value) {
    Reader in{value};
    auto preset = read_preset(in);
    if (!in.ok()) {
        return std::unexpected(in.error());
    }
    return preset;
}

Json encode(const convert::ConversionScanItem& item) {
    return Json{{"item_index", item.item_index},
                {"source", protocol::encode_raw_path(item.source_raw_path)},
                {"selection", encode(item.selection)},
                {"range", item.range ? encode(*item.range) : Json()},
                {"destination", protocol::encode_raw_path(item.destination_raw_path)},
                {"metadata", encode(item.metadata)}};
}

core::Result<convert::ConversionScanItem> decode_conversion_item(const Json& value) {
    Reader in{value};
    convert::ConversionScanItem item{
        .item_index = in.index("item_index"),
        .source_raw_path = in.bytes("source"),
        .selection = {},
        .range = std::nullopt,
        .destination_raw_path = in.bytes("destination"),
        .metadata = {},
    };
    if (!in.ok()) {
        return std::unexpected(in.error());
    }
    if (const auto* selection = in.find("selection"); selection != nullptr) {
        auto decoded = decode_selection(*selection);
        if (!decoded) {
            return std::unexpected(std::move(decoded.error()));
        }
        item.selection = *decoded;
    }
    if (const auto* range = in.find("range"); range != nullptr && !range->is_null()) {
        auto decoded = decode_range(*range);
        if (!decoded) {
            return std::unexpected(std::move(decoded.error()));
        }
        item.range = *decoded;
    }
    if (const auto* document = in.find("metadata"); document != nullptr) {
        auto decoded = decode_document(*document);
        if (!decoded) {
            return std::unexpected(std::move(decoded.error()));
        }
        item.metadata = std::move(*decoded);
    }
    return item;
}

Json encode(const convert::ConversionScanOptions& options) {
    return Json{{"preset", encode(options.preset)},
                {"maximum_parallelism", options.maximum_parallelism},
                {"target_sample_rate", optional_json(options.target_sample_rate)},
                {"sample_rate_cap", optional_json(options.sample_rate_cap)},
                {"target_bit_depth", optional_json(options.target_bit_depth)},
                {"keep_source_bit_depth", options.keep_source_bit_depth},
                {"channels", name_of(options.channel_policy, channels)},
                {"gain", name_of(options.gain_mode, gains)},
                {"gain_preamp_db", options.gain_preamp_db},
                {"artwork", options.carry_artwork},
                {"artwork_edge", options.lossy_artwork_edge}};
}

core::Result<convert::ConversionScanOptions> decode_conversion_options(const Json& value) {
    Reader in{value};
    convert::ConversionScanOptions options;
    auto preset = in.optional_object<convert::EncoderPreset>("preset", read_preset);
    if (!preset && in.ok()) {
        in.fail("preset is required");
    }
    options.preset = preset.value_or(convert::EncoderPreset{});
    options.maximum_parallelism = in.index("maximum_parallelism");
    options.target_sample_rate = optional_number<int>(in, "target_sample_rate");
    options.sample_rate_cap = optional_number<int>(in, "sample_rate_cap");
    options.target_bit_depth = optional_number<int>(in, "target_bit_depth");
    options.keep_source_bit_depth = in.optional_flag("keep_source_bit_depth");
    options.channel_policy = in.named("channels", channels);
    options.gain_mode = in.named("gain", gains);
    if (const auto* preamp = in.find("gain_preamp_db"); preamp != nullptr) {
        if (!preamp->is_number()) {
            in.fail("gain_preamp_db must be a number");
        } else {
            options.gain_preamp_db = preamp->get<float>();
        }
    }
    options.carry_artwork = in.optional_flag("artwork");
    options.lossy_artwork_edge =
        optional_number<std::uint32_t>(in, "artwork_edge").value_or(options.lossy_artwork_edge);
    if (!in.ok()) {
        return std::unexpected(in.error());
    }
    return options;
}

Json encode(const convert::ConversionScanResult& result) {
    auto items = Json::array();
    for (const auto& item : result.items) {
        items.push_back(Json{{"item_index", item.item_index},
                             {"source", protocol::encode_raw_path(item.source_raw_path)},
                             {"destination", protocol::encode_raw_path(item.destination_raw_path)},
                             {"state", name_of(item.state, states)},
                             {"source_revision", encode_optional_revision(item.source_revision)},
                             {"issue", encode_optional_error(item.issue)}});
    }
    return Json{{"items", std::move(items)},
                {"cancellation_requested", result.cancellation_requested}};
}

core::Result<convert::ConversionScanResult> decode_conversion_result(const Json& value) {
    Reader in{value};
    convert::ConversionScanResult result;
    // What was made is the asker's to look at; the outcome says which.
    result.items = in.list<convert::ConvertedItemScan>("items", [](Reader& item_in) {
        return convert::ConvertedItemScan{
            .item_index = item_in.index("item_index"),
            .source_raw_path = item_in.bytes("source"),
            .destination_raw_path = item_in.bytes("destination"),
            .state = item_in.named("state", states),
            .converted = std::nullopt,
            .source_revision = item_in.optional_revision("source_revision"),
            .issue = item_in.optional_error("issue"),
        };
    });
    result.cancellation_requested = in.optional_flag("cancellation_requested");
    if (!in.ok()) {
        return std::unexpected(in.error());
    }
    return result;
}

} // namespace trackknife::engine::wire
