// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/remote_file_work.hpp"

#include "trackknife/engine/file_work_wire.hpp"
#include "trackknife/formats/probe.hpp"

#include <algorithm>

#include <utility>
#include <vector>

namespace trackknife::engine {

using protocol::Json;

namespace {

[[nodiscard]] core::Error unexpected_answer(const std::string& what) {
    return core::Error{.code = core::ErrorCode::backend,
                       .message = "the engine answered " + what + " with something unexpected",
                       .context = {}};
}

// A job's outcome: its {result} decoded, or its {error}.
template <typename T, typename Decode>
[[nodiscard]] core::Result<T> outcome_of(const Json& outcome, Decode decode) {
    if (const auto error = outcome.find("error"); error != outcome.end()) {
        auto decoded = wire::decode_error(*error);
        return std::unexpected(decoded ? std::move(*decoded) : unexpected_answer("a job"));
    }
    const auto result = outcome.find("result");
    if (result == outcome.end()) {
        return std::unexpected(unexpected_answer("a job"));
    }
    return decode(*result);
}

} // namespace

core::Result<FileTechnicals> probe_local_technicals(const std::string& raw_path,
                                                    const core::CancellationToken& cancellation) {
    auto probe = formats::probe_local_media(raw_path, cancellation);
    if (!probe) {
        return std::unexpected(std::move(probe.error()));
    }
    const auto best = probe->best_audio_stream
                          ? std::ranges::find(probe->audio_streams, *probe->best_audio_stream,
                                              &formats::AudioStreamInfo::stream_index)
                          : probe->audio_streams.end();
    if (best == probe->audio_streams.end()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::unsupported, .message = "no audio stream", .context = {}});
    }
    return FileTechnicals{.codec = best->codec_name,
                          .sample_rate = best->sample_rate,
                          .bits = formats::bits_per_sample_hint(best->sample_format),
                          .channels = best->channels,
                          .bit_rate = best->bit_rate > 0 ? best->bit_rate : probe->bit_rate,
                          .duration_ms = probe->duration_ms.value_or(-1)};
}

RemoteFileWork::RemoteFileWork(protocol::Endpoint endpoint) : endpoint_(std::move(endpoint)) {}

RemoteFileWork::~RemoteFileWork() = default;

core::Result<std::shared_ptr<protocol::Client>> RemoteFileWork::client() {
    const std::lock_guard guard{mutex_};
    if (client_ && client_->connected()) {
        return client_;
    }
    auto connected = protocol::Client::connect(endpoint_);
    if (!connected) {
        return std::unexpected(std::move(connected.error()));
    }
    client_ = std::shared_ptr<protocol::Client>{std::move(*connected)};
    return client_;
}

core::Result<Json> RemoteFileWork::read_one(const std::string& raw_path,
                                            const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::cancelled, .message = "reading cancelled", .context = {}});
    }
    auto connection = client();
    if (!connection) {
        return std::unexpected(std::move(connection.error()));
    }
    auto answer = (*connection)
                      ->call("metadata.read",
                             Json{{"paths", Json::array({protocol::encode_raw_path(raw_path)})}});
    if (!answer) {
        return std::unexpected(std::move(answer.error()));
    }
    const auto files = answer->find("files");
    if (files == answer->end() || !files->is_array() || files->size() != 1U) {
        return std::unexpected(unexpected_answer("metadata.read"));
    }
    return files->front();
}

metadata::MetadataFileAccess RemoteFileWork::access() {
    return metadata::MetadataFileAccess{
        .read = [this](const std::string& raw_path, const core::CancellationToken& cancellation)
            -> core::Result<metadata::LocalMetadataRead> {
            auto file = read_one(raw_path, cancellation);
            if (!file) {
                return std::unexpected(std::move(file.error()));
            }
            if (const auto read = file->find("read"); read != file->end()) {
                return wire::decode_metadata_read(*read);
            }
            const auto error = file->find("error");
            auto decoded = error != file->end() ? wire::decode_error(*error)
                                                : core::Result<core::Error>{std::unexpected(
                                                      unexpected_answer("metadata.read"))};
            return std::unexpected(decoded ? std::move(*decoded) : std::move(decoded.error()));
        },
        .revision = [this](const std::string& raw_path) -> core::Result<core::LocalSourceRevision> {
            auto file = read_one(raw_path, {});
            if (!file) {
                return std::unexpected(std::move(file.error()));
            }
            if (const auto read = file->find("read"); read != file->end()) {
                const auto revision = read->find("revision");
                return revision != read->end()
                           ? wire::decode_revision(*revision)
                           : std::unexpected(unexpected_answer("metadata.read"));
            }
            if (const auto revision = file->find("revision");
                revision != file->end() && !revision->is_null()) {
                return wire::decode_revision(*revision);
            }
            // Not there at all: the engine's error says why.
            const auto error = file->find("error");
            auto decoded = error != file->end() ? wire::decode_error(*error)
                                                : core::Result<core::Error>{std::unexpected(
                                                      unexpected_answer("metadata.read"))};
            return std::unexpected(decoded ? std::move(*decoded) : std::move(decoded.error()));
        }};
}

core::Result<loudness::LoudnessScanResult>
RemoteFileWork::scan(const std::span<const loudness::LoudnessScanItem> items,
                     const loudness::LoudnessScanOptions& options,
                     const loudness::LoudnessScanProgressCallback& progress,
                     const core::CancellationToken& cancellation) {
    auto connection = client();
    if (!connection) {
        return std::unexpected(std::move(connection.error()));
    }
    auto encoded = Json::array();
    for (const auto& item : items) {
        encoded.push_back(wire::encode(item));
    }
    auto outcome = (*connection)
                       ->run_job(
                           "loudness.scan",
                           Json{{"items", std::move(encoded)}, {"options", wire::encode(options)}},
                           [&items, &progress](const Json& reported) {
                               if (!progress) {
                                   return;
                               }
                               loudness::LoudnessScanProgress step;
                               step.item_index = reported.value("item_index", std::size_t{0});
                               step.completed_items =
                                   reported.value("completed_items", std::size_t{0});
                               step.total_items = reported.value("total_items", items.size());
                               step.state = loudness::LoudnessScanState::analyzed;
                               for (const auto& item : items) {
                                   if (item.item_index == step.item_index) {
                                       step.raw_path = item.raw_path;
                                   }
                               }
                               progress(step);
                           },
                           cancellation);
    if (!outcome) {
        return std::unexpected(std::move(outcome.error()));
    }
    return outcome_of<loudness::LoudnessScanResult>(
        *outcome, [](const Json& value) { return wire::decode_scan_result(value); });
}

core::Result<FileTechnicals> RemoteFileWork::probe(const std::string& raw_path,
                                                   const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::cancelled, .message = "probe cancelled", .context = {}});
    }
    auto connection = client();
    if (!connection) {
        return std::unexpected(std::move(connection.error()));
    }
    auto answer = (*connection)
                      ->call("media.probe",
                             Json{{"paths", Json::array({protocol::encode_raw_path(raw_path)})}});
    if (!answer) {
        return std::unexpected(std::move(answer.error()));
    }
    const auto files = answer->find("files");
    if (files == answer->end() || !files->is_array() || files->size() != 1U) {
        return std::unexpected(unexpected_answer("media.probe"));
    }
    const auto& file = files->front();
    if (const auto facts = file.find("technicals"); facts != file.end() && facts->is_object()) {
        return FileTechnicals{.codec = facts->value("codec", std::string{}),
                              .sample_rate = facts->value("sample_rate", 0),
                              .bits = facts->value("bits", 0),
                              .channels = facts->value("channels", 0),
                              .bit_rate = facts->value("bit_rate", std::int64_t{0}),
                              .duration_ms = facts->value("duration_ms", std::int64_t{-1})};
    }
    const auto error = file.find("error");
    auto decoded =
        error != file.end()
            ? wire::decode_error(*error)
            : core::Result<core::Error>{std::unexpected(unexpected_answer("media.probe"))};
    return std::unexpected(decoded ? std::move(*decoded) : std::move(decoded.error()));
}

core::Result<operations::MetadataApplyResult>
RemoteFileWork::apply(const metadata::MetadataWritePlan& plan,
                      const operations::MetadataApplyProgressCallback& progress,
                      const core::CancellationToken& cancellation) {
    auto connection = client();
    if (!connection) {
        return std::unexpected(std::move(connection.error()));
    }
    auto outcome = (*connection)
                       ->run_job(
                           "metadata.apply", Json{{"plan", wire::encode(plan)}},
                           [&progress](const Json& reported) {
                               if (!progress) {
                                   return;
                               }
                               if (auto step = wire::decode_apply_progress(reported)) {
                                   progress(*step);
                               }
                           },
                           cancellation);
    if (!outcome) {
        return std::unexpected(std::move(outcome.error()));
    }
    return outcome_of<operations::MetadataApplyResult>(
        *outcome, [](const Json& value) { return wire::decode_apply_result(value); });
}

core::Result<Json> RemoteFileWork::interrupted() {
    auto connection = client();
    if (!connection) {
        return std::unexpected(std::move(connection.error()));
    }
    return (*connection)->call("metadata.interrupted");
}

bool RemoteFileWork::supported() {
    {
        const std::lock_guard guard{mutex_};
        if (supported_) {
            return *supported_;
        }
    }
    auto answer = interrupted();
    // Only an answer settles it: an engine that is not reachable now may be
    // later, and is asked again then.
    if (answer || answer.error().code == core::ErrorCode::unsupported) {
        const std::lock_guard guard{mutex_};
        supported_ = answer.has_value();
        return *supported_;
    }
    return false;
}

} // namespace trackknife::engine
