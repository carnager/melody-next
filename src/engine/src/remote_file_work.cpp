// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/remote_file_work.hpp"

#include "trackknife/engine/file_work_wire.hpp"
#include "trackknife/formats/probe.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>

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

// Writes a download into its file, stopping when cancelled.
struct Download {
    std::FILE* file;
    const core::CancellationToken* cancellation;
};

std::size_t write_download(char* data, std::size_t size, std::size_t count, void* user) {
    auto* download = static_cast<Download*>(user);
    return std::fwrite(data, size, count, download->file) * size;
}

int stop_download(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<Download*>(user)->cancellation->is_cancellation_requested() ? 1 : 0;
}

[[nodiscard]] core::Error download_error(core::ErrorCode code, std::string message) {
    return core::Error{.code = code, .message = std::move(message), .context = {}};
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

namespace {

// A lookup job's {result: {body}}, decoded.
[[nodiscard]] core::Result<std::string> body_of(const core::Result<Json>& outcome) {
    if (!outcome) {
        return std::unexpected(outcome.error());
    }
    return outcome_of<std::string>(*outcome, [](const Json& result) -> core::Result<std::string> {
        const auto body = result.find("body");
        if (body == result.end() || !body->is_string()) {
            return std::unexpected(unexpected_answer("a lookup"));
        }
        return protocol::decode_raw_path(body->get<std::string>());
    });
}

} // namespace

core::Result<std::string> RemoteFileWork::fetch(const std::string& url,
                                                const core::CancellationToken& cancellation) {
    auto connection = client();
    if (!connection) {
        return std::unexpected(std::move(connection.error()));
    }
    return body_of(
        (*connection)->run_job("musicbrainz.fetch", Json{{"url", url}}, {}, cancellation));
}

core::Result<MetadataServices::Fingerprint>
RemoteFileWork::fingerprint(const std::string& raw_path,
                            const core::CancellationToken& cancellation) {
    auto connection = client();
    if (!connection) {
        return std::unexpected(std::move(connection.error()));
    }
    auto outcome =
        (*connection)
            ->run_job("acoustid.fingerprint", Json{{"path", protocol::encode_raw_path(raw_path)}},
                      {}, cancellation);
    if (!outcome) {
        return std::unexpected(std::move(outcome.error()));
    }
    return outcome_of<MetadataServices::Fingerprint>(
        *outcome, [](const Json& result) -> core::Result<MetadataServices::Fingerprint> {
            MetadataServices::Fingerprint fingerprint{
                .duration_seconds = result.value("duration_seconds", std::size_t{0}),
                .fingerprint = result.value("fingerprint", std::string{})};
            if (fingerprint.duration_seconds == 0U || fingerprint.fingerprint.empty()) {
                return std::unexpected(unexpected_answer("acoustid.fingerprint"));
            }
            return fingerprint;
        });
}

core::Result<std::string>
RemoteFileWork::acoustid_lookup(const MetadataServices::Fingerprint& fingerprint,
                                const core::CancellationToken& cancellation) {
    auto connection = client();
    if (!connection) {
        return std::unexpected(std::move(connection.error()));
    }
    return body_of((*connection)
                       ->run_job("acoustid.lookup",
                                 Json{{"duration_seconds", fingerprint.duration_seconds},
                                      {"fingerprint", fingerprint.fingerprint}},
                                 {}, cancellation));
}

core::Result<void> RemoteFileWork::set_acoustid_key(const std::string& key) {
    auto connection = client();
    if (!connection) {
        return std::unexpected(std::move(connection.error()));
    }
    auto answer = (*connection)->call("metadata_services.set", Json{{"acoustid_client_key", key}});
    if (!answer) {
        return std::unexpected(std::move(answer.error()));
    }
    return {};
}

namespace {

// A call's answer, or the connection's failure, in one.
[[nodiscard]] core::Result<Json>
answer_of(core::Result<std::shared_ptr<protocol::Client>> connection, const std::string& method,
          const Json& params) {
    if (!connection) {
        return std::unexpected(std::move(connection.error()));
    }
    return (*connection)->call(method, params, std::chrono::seconds{60});
}

[[nodiscard]] core::Result<metadata::ArtworkImageFile> image_of(const core::Result<Json>& answer,
                                                                const std::string& method) {
    if (!answer) {
        return std::unexpected(answer.error());
    }
    const auto image = answer->find("image");
    if (image == answer->end() || image->is_null()) {
        return std::unexpected(unexpected_answer(method));
    }
    return wire::decode_image_file(*image);
}

} // namespace

operations::ArtworkFileAccess RemoteFileWork::artwork_access() {
    return operations::ArtworkFileAccess{
        .inventory = [this](const std::string& raw_path,
                            const metadata::ArtworkInventoryPolicy& policy,
                            const core::CancellationToken& cancellation)
            -> core::Result<metadata::LocalArtworkInventory> {
            if (cancellation.is_cancellation_requested()) {
                return std::unexpected(core::Error{.code = core::ErrorCode::cancelled,
                                                   .message = "reading cancelled",
                                                   .context = {}});
            }
            auto answer =
                answer_of(client(), "artwork.inventory",
                          Json{{"paths", Json::array({protocol::encode_raw_path(raw_path)})},
                               {"policy", wire::encode(policy)}});
            if (!answer) {
                return std::unexpected(std::move(answer.error()));
            }
            const auto files = answer->find("files");
            if (files == answer->end() || !files->is_array() || files->size() != 1U) {
                return std::unexpected(unexpected_answer("artwork.inventory"));
            }
            const auto& file = files->front();
            if (const auto inventory = file.find("inventory"); inventory != file.end()) {
                return wire::decode_inventory(*inventory);
            }
            const auto error = file.find("error");
            auto decoded = error != file.end() ? wire::decode_error(*error)
                                               : core::Result<core::Error>{std::unexpected(
                                                     unexpected_answer("artwork.inventory"))};
            return std::unexpected(decoded ? std::move(*decoded) : std::move(decoded.error()));
        },
        .image_file =
            [this](const std::string& raw_path, const std::uint64_t maximum_bytes,
                   const core::CancellationToken&) {
                return image_of(answer_of(client(), "artwork.image_file",
                                          Json{{"path", protocol::encode_raw_path(raw_path)},
                                               {"maximum_bytes", maximum_bytes}}),
                                "artwork.image_file");
            },
        .image_bytes =
            [this](const metadata::ArtworkImageFile& image, const std::uint64_t maximum_bytes,
                   const core::CancellationToken&) -> core::Result<std::vector<unsigned char>> {
            auto answer =
                answer_of(client(), "artwork.image_bytes",
                          Json{{"image", wire::encode(image)}, {"maximum_bytes", maximum_bytes}});
            if (!answer) {
                return std::unexpected(std::move(answer.error()));
            }
            const auto bytes = answer->find("bytes");
            if (bytes == answer->end() || !bytes->is_string()) {
                return std::unexpected(unexpected_answer("artwork.image_bytes"));
            }
            auto decoded = protocol::decode_raw_path(bytes->get<std::string>());
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            return std::vector<unsigned char>(decoded->begin(), decoded->end());
        },
        .revision = access().revision,
        .destination = [this](const std::string& raw_path, const core::CancellationToken&)
            -> core::Result<std::optional<metadata::ArtworkImageFile>> {
            auto answer = answer_of(client(), "artwork.destination",
                                    Json{{"path", protocol::encode_raw_path(raw_path)}});
            if (!answer) {
                return std::unexpected(std::move(answer.error()));
            }
            const auto image = answer->find("image");
            if (image == answer->end() || image->is_null()) {
                return std::optional<metadata::ArtworkImageFile>{};
            }
            auto decoded = wire::decode_image_file(*image);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            return std::optional{std::move(*decoded)};
        }};
}

core::Result<metadata::ArtworkImageFile>
RemoteFileWork::stage(const std::span<const unsigned char> bytes) {
    return image_of(
        answer_of(client(), "artwork.stage",
                  Json{{"bytes", protocol::encode_raw_path(std::string{
                                     reinterpret_cast<const char*>(bytes.data()), bytes.size()})}}),
        "artwork.stage");
}

core::Result<operations::ArtworkApplyResult>
RemoteFileWork::artwork_apply(const metadata::ArtworkWritePlan& plan,
                              const operations::ArtworkApplyProgressCallback& progress,
                              const core::CancellationToken& cancellation) {
    auto connection = client();
    if (!connection) {
        return std::unexpected(std::move(connection.error()));
    }
    auto outcome = (*connection)
                       ->run_job(
                           "artwork.apply", Json{{"plan", wire::encode(plan)}},
                           [&progress](const Json& reported) {
                               if (!progress) {
                                   return;
                               }
                               if (auto step = wire::decode_artwork_apply_progress(reported)) {
                                   progress(*step);
                               }
                           },
                           cancellation);
    if (!outcome) {
        return std::unexpected(std::move(outcome.error()));
    }
    return outcome_of<operations::ArtworkApplyResult>(
        *outcome, [](const Json& value) { return wire::decode_artwork_apply_result(value); });
}

core::Result<operations::OutputPathPreflight>
RemoteFileWork::preflight(const operations::OutputPathPlan& plan,
                          const core::CancellationToken& cancellation) {
    auto connection = client();
    if (!connection) {
        return std::unexpected(std::move(connection.error()));
    }
    auto outcome = (*connection)
                       ->run_job(
                           "paths.preflight", Json{{"plan", wire::encode(plan)}},
                           [](const Json&) {}, cancellation);
    if (!outcome) {
        return std::unexpected(std::move(outcome.error()));
    }
    return outcome_of<operations::OutputPathPreflight>(
        *outcome, [](const Json& value) { return wire::decode_path_preflight(value); });
}

core::Result<operations::FilePublicationApplyResult>
RemoteFileWork::publish(const operations::PreparationPlan& plan,
                        const operations::FilePublicationApplyProgressCallback& progress,
                        const core::CancellationToken& cancellation) {
    auto connection = client();
    if (!connection) {
        return std::unexpected(std::move(connection.error()));
    }
    auto outcome = (*connection)
                       ->run_job(
                           "preparation.apply", Json{{"plan", wire::encode(plan)}},
                           [&progress](const Json& reported) {
                               if (!progress) {
                                   return;
                               }
                               if (auto step = wire::decode_publication_apply_progress(reported)) {
                                   progress(*step);
                               }
                           },
                           cancellation);
    if (!outcome) {
        return std::unexpected(std::move(outcome.error()));
    }
    return outcome_of<operations::FilePublicationApplyResult>(
        *outcome, [](const Json& value) { return wire::decode_publication_apply_result(value); });
}

core::Result<void> RemoteFileWork::download_original(const std::string& raw_path,
                                                     const std::filesystem::path& to,
                                                     const core::CancellationToken& cancellation) {
    auto connection = client();
    if (!connection) {
        return std::unexpected(std::move(connection.error()));
    }
    auto ticket = (*connection)
                      ->call("streams.ticket", Json{{"path", protocol::encode_raw_path(raw_path)},
                                                    {"format", "original"}});
    if (!ticket) {
        return std::unexpected(std::move(ticket.error()));
    }
    if (!ticket->contains("port") || !ticket->at("port").is_number_integer() ||
        !ticket->contains("query") || !ticket->at("query").is_string()) {
        return std::unexpected(unexpected_answer("a download ticket"));
    }
    // The stream port is beside the one this connection uses; an engine
    // reached through its socket is this machine's.
    auto host = endpoint_.host.empty() ? std::string{"127.0.0.1"} : endpoint_.host;
    if (host.find(':') != std::string::npos) {
        host = "[" + host + "]";
    }
    const auto url = "http://" + host + ":" + std::to_string(ticket->at("port").get<int>()) +
                     "/stream?" + ticket->at("query").get<std::string>();

    auto part = to;
    part += ".part";
    struct Close {
        void operator()(std::FILE* open) const { static_cast<void>(std::fclose(open)); }
    };
    std::unique_ptr<std::FILE, Close> file{std::fopen(part.c_str(), "wb")};
    if (!file) {
        return std::unexpected(
            download_error(core::ErrorCode::io, "could not create " + part.string()));
    }
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl{curl_easy_init(), curl_easy_cleanup};
    if (!curl) {
        return std::unexpected(download_error(core::ErrorCode::backend, "curl did not start"));
    }
    Download download{.file = file.get(), .cancellation = &cancellation};
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http");
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_download);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &download);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 10'000L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, stop_download);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &download);
    const auto performed = curl_easy_perform(curl.get());
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    const auto closed = std::fclose(file.release()) == 0;
    std::error_code ignored;
    if (cancellation.is_cancellation_requested()) {
        std::filesystem::remove(part, ignored);
        return std::unexpected(
            download_error(core::ErrorCode::cancelled, "the download was cancelled"));
    }
    if (performed != CURLE_OK || status != 200 || !closed) {
        std::filesystem::remove(part, ignored);
        return std::unexpected(download_error(
            core::ErrorCode::io,
            performed != CURLE_OK
                ? std::string{"the engine could not be reached: "} + curl_easy_strerror(performed)
                : "the engine answered " + std::to_string(status)));
    }
    std::error_code renamed;
    std::filesystem::rename(part, to, renamed);
    if (renamed) {
        std::filesystem::remove(part, ignored);
        return std::unexpected(download_error(core::ErrorCode::io, renamed.message()));
    }
    return {};
}

core::Result<void> RemoteFileWork::set_rating_tags(const bool write_tags) {
    auto connection = client();
    if (!connection) {
        return std::unexpected(std::move(connection.error()));
    }
    auto answer = (*connection)->call("ratings.set_tags", Json{{"write_tags", write_tags}});
    if (!answer) {
        return std::unexpected(std::move(answer.error()));
    }
    return {};
}

core::Result<void> RemoteFileWork::set_rating_scale(const std::string& scale) {
    auto connection = client();
    if (!connection) {
        return std::unexpected(std::move(connection.error()));
    }
    auto answer = (*connection)->call("ratings.set_tags", Json{{"rating_scale", scale}});
    if (!answer) {
        return std::unexpected(std::move(answer.error()));
    }
    return {};
}

core::Result<void>
RemoteFileWork::put_layouts(const std::vector<persistence::SavedOutputLayoutProfile>& layouts,
                            const std::vector<core::StableId>& removed) {
    auto connection = client();
    if (!connection) {
        return std::unexpected(std::move(connection.error()));
    }
    auto list = Json::array();
    for (const auto& layout : layouts) {
        list.push_back(wire::encode(layout));
    }
    auto gone = Json::array();
    for (const auto& id : removed) {
        gone.push_back(id.to_string());
    }
    auto answer =
        (*connection)
            ->call("layouts.put", Json{{"layouts", std::move(list)}, {"removed", std::move(gone)}});
    if (!answer) {
        return std::unexpected(std::move(answer.error()));
    }
    return {};
}

namespace {

[[nodiscard]] core::Result<std::vector<persistence::SavedDestinationProfile>>
destinations_of(const core::Result<Json>& answer) {
    if (!answer) {
        return std::unexpected(answer.error());
    }
    const auto listed = answer->find("destinations");
    if (listed == answer->end() || !listed->is_array()) {
        return std::unexpected(unexpected_answer("the destinations"));
    }
    std::vector<persistence::SavedDestinationProfile> result;
    for (const auto& value : *listed) {
        auto destination = wire::decode_saved_destination(value);
        if (!destination) {
            return std::unexpected(std::move(destination.error()));
        }
        result.push_back(std::move(*destination));
    }
    return result;
}

} // namespace

core::Result<std::vector<persistence::SavedDestinationProfile>> RemoteFileWork::destinations() {
    auto connection = client();
    if (!connection) {
        return std::unexpected(std::move(connection.error()));
    }
    return destinations_of((*connection)->call("destinations.list"));
}

core::Result<void>
RemoteFileWork::save_destination(const persistence::SavedDestinationProfile& destination) {
    auto connection = client();
    if (!connection) {
        return std::unexpected(std::move(connection.error()));
    }
    auto saved = destinations_of(
        (*connection)->call("destinations.save", Json{{"destination", wire::encode(destination)}}));
    if (!saved) {
        return std::unexpected(std::move(saved.error()));
    }
    return {};
}

core::Result<void> RemoteFileWork::remove_destination(const core::StableId& id) {
    auto connection = client();
    if (!connection) {
        return std::unexpected(std::move(connection.error()));
    }
    auto removed =
        destinations_of((*connection)->call("destinations.remove", Json{{"id", id.to_string()}}));
    if (!removed) {
        return std::unexpected(std::move(removed.error()));
    }
    return {};
}

core::Result<RemoteFileWork::FolderListing> RemoteFileWork::folders(const std::string& path) {
    auto connection = client();
    if (!connection) {
        return std::unexpected(std::move(connection.error()));
    }
    auto answer =
        (*connection)
            ->call("folders.list",
                   path.empty() ? Json::object() : Json{{"path", protocol::encode_raw_path(path)}});
    if (!answer) {
        return std::unexpected(std::move(answer.error()));
    }
    const auto decoded = [](const Json& value) -> std::optional<std::string> {
        if (!value.is_string()) {
            return std::nullopt;
        }
        auto raw = protocol::decode_raw_path(value.get<std::string>());
        return raw ? std::optional{std::move(*raw)} : std::nullopt;
    };
    FolderListing listing;
    auto here = decoded(answer->value("path", Json()));
    if (!here || !answer->contains("folders") || !answer->at("folders").is_array()) {
        return std::unexpected(unexpected_answer("a folder listing"));
    }
    listing.path = std::move(*here);
    listing.parent = decoded(answer->value("parent", Json()));
    for (const auto& name : answer->at("folders")) {
        if (auto folder = decoded(name)) {
            listing.folders.push_back(std::move(*folder));
        }
    }
    return listing;
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
