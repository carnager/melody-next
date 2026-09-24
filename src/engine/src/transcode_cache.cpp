// SPDX-License-Identifier: GPL-3.0-only
#include "trackknife/engine/transcode_cache.hpp"

#include "trackknife/convert/convert.hpp"
#include "trackknife/convert/preset.hpp"
#include "trackknife/core/local_sources.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace trackknife::engine {
namespace {

[[nodiscard]] std::string hex_digest(const std::string& text) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    EVP_Digest(text.data(), text.size(), digest.data(), &length, EVP_sha256(), nullptr);
    static constexpr std::array<char, 16> digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                                 '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string hex;
    for (unsigned int index = 0; index < length; ++index) {
        hex.push_back(digits[digest[index] >> 4U]);
        hex.push_back(digits[digest[index] & 0x0FU]);
    }
    return hex;
}

} // namespace

TranscodeCache::TranscodeCache(std::filesystem::path directory, const std::uint64_t capacity_bytes)
    : directory_(std::move(directory)), capacity_bytes_(capacity_bytes) {
    std::error_code ignored;
    std::filesystem::create_directories(directory_, ignored);
}

core::Result<std::filesystem::path> TranscodeCache::ensure(const TranscodeSource& source,
                                                           const StreamFormat& format) {
    // The file as it is now: a change to it -- new tags, a replacement --
    // is another conversion, not the old one served again.
    auto revision = core::observe_local_source_revision(source.raw_path);
    if (!revision) {
        return std::unexpected(std::move(revision.error()));
    }
    std::ostringstream identity;
    identity << source.raw_path << '\0' << revision->device << ':' << revision->inode << ':'
             << revision->size << ':' << revision->modification_time_seconds << ':'
             << revision->modification_time_nanoseconds << '\0'
             << source.selection.stream_index.value_or(-1) << ':'
             << source.selection.subsong_index.value_or(-1) << '\0'
             << (source.segment ? source.segment->start_sample : -1) << ':'
             << (source.segment ? source.segment->end_sample.value_or(-1) : -1) << '\0' << "opus:"
             << format.bitrate_kbps;
    const auto key = hex_digest(identity.str());
    const auto target = directory_ / (key + ".opus");

    std::shared_future<core::Result<std::filesystem::path>> waiting;
    std::promise<core::Result<std::filesystem::path>> making;
    bool mine = false;
    {
        const std::lock_guard guard{mutex_};
        std::error_code missing;
        if (std::filesystem::is_regular_file(target, missing)) {
            // Used again: last in line to be evicted.
            std::filesystem::last_write_time(target, std::filesystem::file_time_type::clock::now(),
                                             missing);
            return target;
        }
        if (const auto found = converting_.find(key); found != converting_.end()) {
            waiting = found->second;
        } else {
            waiting = making.get_future().share();
            converting_.emplace(key, waiting);
            mine = true;
        }
    }
    if (!mine) {
        return waiting.get();
    }
    auto made = convert(source, format, target);
    {
        const std::lock_guard guard{mutex_};
        converting_.erase(key);
        if (made) {
            evict(target);
        }
    }
    making.set_value(made);
    return made;
}

core::Result<std::filesystem::path> TranscodeCache::convert(const TranscodeSource& source,
                                                            const StreamFormat& format,
                                                            const std::filesystem::path& target) {
    auto preset = convert::find_encoder_preset("opus-192");
    if (!preset) {
        return std::unexpected(core::Error{.code = core::ErrorCode::unsupported,
                                           .message = "this engine has no Opus encoder",
                                           .context = {}});
    }
    preset->id = "opus-stream";
    preset->bit_rate = static_cast<std::int64_t>(std::clamp(format.bitrate_kbps, 16, 512)) * 1000;
    convert::AudioConversionRequest request;
    request.source_raw_path = source.raw_path;
    request.source_selection = source.selection;
    request.source_range = source.segment;
    request.destination_raw_path = target.native();
    request.preset = *preset;
    // Stereo at most: a phone has two ears' worth of output, and a 5.1
    // master at 128 kbps would spend its bits on channels folded away.
    request.channel_policy = convert::ConversionChannelPolicy::stereo;
    // No gain baked in: the player applies ReplayGain as it does to the
    // original, from the values the engine sends with the stream.
    auto converted = convert::convert_audio_file(request);
    if (!converted) {
        return std::unexpected(std::move(converted.error()));
    }
    return target;
}

void TranscodeCache::evict(const std::filesystem::path& keep) {
    struct Held {
        std::filesystem::path path;
        std::filesystem::file_time_type used;
        std::uint64_t size;
    };
    std::vector<Held> held;
    std::uint64_t total = 0;
    std::error_code ignored;
    for (const auto& entry : std::filesystem::directory_iterator{directory_, ignored}) {
        if (!entry.is_regular_file(ignored) || entry.path().extension() != ".opus") {
            continue;
        }
        const auto size = entry.file_size(ignored);
        total += size;
        held.push_back({entry.path(), entry.last_write_time(ignored), size});
    }
    std::ranges::sort(held, {}, &Held::used);
    for (const auto& file : held) {
        if (total <= capacity_bytes_) {
            break;
        }
        if (file.path == keep) {
            continue;
        }
        if (std::filesystem::remove(file.path, ignored)) {
            total -= file.size;
        }
    }
}

} // namespace trackknife::engine
