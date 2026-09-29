// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/metadata_services.hpp"

#include "trackknife/core/posix.hpp"
#include "trackknife/engine/file_work_wire.hpp"
#include "trackknife/musicbrainz/acoustid.hpp"
#include "trackknife/musicbrainz/web_service.hpp"
#include "trackknife/persistence/musicbrainz_cache.hpp"

#include <curl/curl.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

extern char** environ;

namespace trackknife::engine {
namespace {

using protocol::Json;

// MusicBrainz requires an identifying User-Agent naming the application --
// the one Trackknife has always sent.
constexpr auto user_agent = "Trackknife/0.0.1 ( https://github.com/carnager/trackknife )";
// Covers can be large; nothing a lookup needs is larger.
constexpr std::size_t response_limit = 64U * 1024U * 1024U;
constexpr long request_timeout_ms = 30'000;
constexpr auto fingerprint_timeout = std::chrono::seconds{120};

[[nodiscard]] core::Error service_error(const core::ErrorCode code, std::string message) {
    return core::Error{.code = code, .message = std::move(message), .context = {}};
}

std::size_t collect(char* data, const std::size_t size, const std::size_t count, void* target) {
    auto& body = *static_cast<std::string*>(target);
    const auto bytes = size * count;
    if (body.size() + bytes > response_limit) {
        return 0; // aborts the transfer
    }
    body.append(data, bytes);
    return bytes;
}

struct Transfer {
    const core::CancellationToken* cancellation;
};

int stop_when_cancelled(void* data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto* transfer = static_cast<Transfer*>(data);
    return transfer->cancellation->is_cancellation_requested() ? 1 : 0;
}

// One HTTPS request: GET, or POST with a form body. Redirects are followed,
// over HTTPS only -- the Cover Art Archive hands images out from archive.org.
[[nodiscard]] core::Result<std::string> perform(const std::string& url,
                                                const std::optional<std::string>& form,
                                                const core::CancellationToken& cancellation) {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl{curl_easy_init(), curl_easy_cleanup};
    if (!curl) {
        return std::unexpected(service_error(core::ErrorCode::backend, "curl did not start"));
    }
    std::string body;
    Transfer transfer{.cancellation = &cancellation};
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, user_agent);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, request_timeout_ms);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, stop_when_cancelled);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &transfer);
    if (form) {
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, form->c_str());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(form->size()));
    }
    const auto performed = curl_easy_perform(curl.get());
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(
            service_error(core::ErrorCode::cancelled, "the lookup was cancelled"));
    }
    if (performed != CURLE_OK) {
        return std::unexpected(
            service_error(core::ErrorCode::io, std::string{"the service is unreachable: "} +
                                                   curl_easy_strerror(performed)));
    }
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    // A client error still carries the service's own message, which the
    // tagger shows; only a server that said nothing is an error here.
    if (status >= 500 && body.empty()) {
        return std::unexpected(
            service_error(core::ErrorCode::io, "the service answered " + std::to_string(status)));
    }
    return body;
}

} // namespace

MetadataServices::MetadataServices(std::filesystem::path database, std::filesystem::path state)
    : database_(std::move(database)), key_file_(std::move(state) / "acoustid.key") {}

bool MetadataServices::fetchable(const std::string_view url) {
    return url.starts_with("https://musicbrainz.org/ws/2/") ||
           url.starts_with("https://coverartarchive.org/");
}

void MetadataServices::pace(std::chrono::steady_clock::time_point& last,
                            const std::chrono::milliseconds interval) {
    std::chrono::steady_clock::time_point dispatch;
    {
        const std::lock_guard guard{pacing_};
        const auto now = std::chrono::steady_clock::now();
        dispatch = std::max(now, last + interval);
        last = dispatch;
    }
    std::this_thread::sleep_until(dispatch);
}

core::Result<std::string> MetadataServices::fetch(const std::string& url,
                                                  const core::CancellationToken& cancellation) {
    if (!fetchable(url)) {
        return std::unexpected(service_error(
            core::ErrorCode::invalid_argument,
            "an engine fetches only MusicBrainz and the Cover Art Archive, over HTTPS"));
    }
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    // Cache first: a lookup already made is not made again, and not paced.
    if (auto cache = persistence::SqliteMusicBrainzResponseCache::open(database_)) {
        if (auto loaded = cache->load(url, now, musicbrainz::response_cache_ttl_seconds);
            loaded && *loaded) {
            const auto& bytes = **loaded;
            return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
        }
    }
    pace(last_musicbrainz_, std::chrono::milliseconds{musicbrainz::minimum_request_interval_ms});
    auto body = perform(url, std::nullopt, cancellation);
    if (!body) {
        return body;
    }
    if (auto cache = persistence::SqliteMusicBrainzResponseCache::open(database_)) {
        static_cast<void>(cache->store(url, *body, now, musicbrainz::response_cache_ttl_seconds,
                                       musicbrainz::response_cache_maximum_entries));
    }
    return body;
}

core::Result<MetadataServices::Fingerprint>
MetadataServices::fingerprint(const std::string& raw_path,
                              const core::CancellationToken& cancellation) {
    std::array<int, 2> output{-1, -1};
    if (core::pipe_cloexec(output.data()) != 0) {
        return std::unexpected(service_error(core::ErrorCode::backend, "could not run fpcalc"));
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    std::string program = "fpcalc";
    std::string json = "-json";
    auto path = raw_path;
    std::array<char*, 4> arguments{program.data(), json.data(), path.data(), nullptr};
    pid_t child = 0;
    const auto spawned =
        posix_spawnp(&child, program.c_str(), &actions, nullptr, arguments.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(output[1]);
    if (spawned != 0) {
        ::close(output[0]);
        return std::unexpected(service_error(
            core::ErrorCode::unsupported, "fpcalc (chromaprint) is not installed on the engine's "
                                          "machine"));
    }
    std::string text;
    std::array<char, 4096> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + fingerprint_timeout;
    bool stopped = false;
    while (true) {
        if (cancellation.is_cancellation_requested() ||
            std::chrono::steady_clock::now() > deadline) {
            ::kill(child, SIGKILL);
            stopped = true;
            break;
        }
        pollfd readable{.fd = output[0], .events = POLLIN, .revents = 0};
        if (::poll(&readable, 1, 200) <= 0) {
            continue;
        }
        const auto read = ::read(output[0], buffer.data(), buffer.size());
        if (read <= 0) {
            break;
        }
        text.append(buffer.data(), static_cast<std::size_t>(read));
    }
    ::close(output[0]);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (stopped) {
        return std::unexpected(
            service_error(core::ErrorCode::cancelled, "fingerprinting was stopped"));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::unexpected(
            service_error(core::ErrorCode::backend, "fpcalc could not fingerprint the file"));
    }
    const auto parsed = Json::parse(text, nullptr, false);
    const auto duration = parsed.is_object() ? parsed.value("duration", 0.0) : 0.0;
    auto fingerprint = parsed.is_object() ? parsed.value("fingerprint", std::string{}) : "";
    if (duration < 1.0 || fingerprint.empty()) {
        return std::unexpected(
            service_error(core::ErrorCode::backend, "fpcalc produced no usable fingerprint"));
    }
    return Fingerprint{.duration_seconds = static_cast<std::size_t>(duration),
                       .fingerprint = std::move(fingerprint)};
}

core::Result<std::string>
MetadataServices::acoustid_lookup(const Fingerprint& fingerprint,
                                  const core::CancellationToken& cancellation) {
    const auto key = acoustid_key();
    if (key.empty()) {
        return std::unexpected(
            service_error(core::ErrorCode::unsupported,
                          "Set an AcoustID client key in Settings → Metadata services before "
                          "identifying by audio fingerprint"));
    }
    auto body = musicbrainz::build_acoustid_lookup_body(key, fingerprint.duration_seconds,
                                                        fingerprint.fingerprint);
    if (!body) {
        return std::unexpected(std::move(body.error()));
    }
    // Below AcoustID's three requests a second.
    pace(last_acoustid_,
         std::chrono::milliseconds{musicbrainz::acoustid_minimum_request_interval_ms});
    return perform(std::string{musicbrainz::acoustid_lookup_url}, *body, cancellation);
}

std::string MetadataServices::acoustid_key() {
    const std::lock_guard guard{key_mutex_};
    std::ifstream input{key_file_};
    std::string key;
    std::getline(input, key);
    return key;
}

bool MetadataServices::has_acoustid_key() { return !acoustid_key().empty(); }

core::Result<void> MetadataServices::set_acoustid_key(const std::string& key) {
    const std::lock_guard guard{key_mutex_};
    std::error_code ignored;
    if (key.empty()) {
        std::filesystem::remove(key_file_, ignored);
        return {};
    }
    // Readable by its owner only, like the engine's password.
    const auto partial = key_file_.string() + ".partial";
    const auto descriptor = ::open(partial.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return std::unexpected(service_error(core::ErrorCode::io, "could not keep the key"));
    }
    const auto line = key + "\n";
    const auto written = ::write(descriptor, line.data(), line.size());
    ::close(descriptor);
    if (written != static_cast<ssize_t>(line.size())) {
        return std::unexpected(service_error(core::ErrorCode::io, "could not keep the key"));
    }
    std::filesystem::rename(partial, key_file_, ignored);
    if (ignored) {
        return std::unexpected(service_error(core::ErrorCode::io, "could not keep the key"));
    }
    return {};
}

void register_metadata_service_jobs(JobCatalog& jobs, MetadataServices& services) {
    const auto encoded_body = [](const std::string& body) {
        return Json{{"result", {{"body", protocol::encode_raw_path(body)}}}};
    };
    jobs.on("musicbrainz.fetch",
            [&services, encoded_body](const Json& params) -> core::Result<JobRegistry::Work> {
                const auto url = params.value("url", std::string{});
                if (!MetadataServices::fetchable(url)) {
                    return std::unexpected(core::Error{
                        .code = core::ErrorCode::invalid_argument,
                        .message = "an engine fetches only MusicBrainz and the Cover Art "
                                   "Archive, over HTTPS",
                        .context = {{.key = "param", .value = "url"}}});
                }
                return [&services, url, encoded_body](const core::CancellationToken& token,
                                                      const JobRegistry::Reporter&) {
                    auto body = services.fetch(url, token);
                    return body ? encoded_body(*body) : Json{{"error", wire::encode(body.error())}};
                };
            });
    jobs.on(
        "acoustid.fingerprint", [&services](const Json& params) -> core::Result<JobRegistry::Work> {
            const auto encoded = params.value("path", std::string{});
            auto raw_path = protocol::decode_raw_path(encoded);
            if (encoded.empty() || !raw_path) {
                return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                                   .message = "an encoded path is required",
                                                   .context = {{.key = "param", .value = "path"}}});
            }
            return [&services, path = std::move(*raw_path)](const core::CancellationToken& token,
                                                            const JobRegistry::Reporter&) {
                auto fingerprint = services.fingerprint(path, token);
                if (!fingerprint) {
                    return Json{{"error", wire::encode(fingerprint.error())}};
                }
                return Json{{"result",
                             {{"duration_seconds", fingerprint->duration_seconds},
                              {"fingerprint", fingerprint->fingerprint}}}};
            };
        });
    jobs.on("acoustid.lookup",
            [&services, encoded_body](const Json& params) -> core::Result<JobRegistry::Work> {
                MetadataServices::Fingerprint fingerprint{
                    .duration_seconds = params.value("duration_seconds", std::size_t{0}),
                    .fingerprint = params.value("fingerprint", std::string{})};
                if (fingerprint.duration_seconds == 0U || fingerprint.fingerprint.empty()) {
                    return std::unexpected(
                        core::Error{.code = core::ErrorCode::invalid_argument,
                                    .message = "a duration and a fingerprint are required",
                                    .context = {{.key = "param", .value = "fingerprint"}}});
                }
                return [&services, fingerprint = std::move(fingerprint), encoded_body](
                           const core::CancellationToken& token, const JobRegistry::Reporter&) {
                    auto body = services.acoustid_lookup(fingerprint, token);
                    return body ? encoded_body(*body) : Json{{"error", wire::encode(body.error())}};
                };
            });
}

void register_metadata_service_methods(protocol::Dispatcher& dispatcher,
                                       MetadataServices& services) {
    dispatcher.on("metadata_services.set", [&services](const Json& params) -> core::Result<Json> {
        const auto key = params.find("acoustid_client_key");
        if (key == params.end() || !key->is_string()) {
            return std::unexpected(
                core::Error{.code = core::ErrorCode::invalid_argument,
                            .message = "acoustid_client_key is required (empty to forget it)",
                            .context = {{.key = "param", .value = "acoustid_client_key"}}});
        }
        auto kept = services.set_acoustid_key(key->get<std::string>());
        if (!kept) {
            return std::unexpected(std::move(kept.error()));
        }
        return Json{{"acoustid", services.has_acoustid_key()}};
    });
    dispatcher.on("metadata_services.status", [&services](const Json&) -> core::Result<Json> {
        return Json{{"acoustid", services.has_acoustid_key()}};
    });
}

} // namespace trackknife::engine
