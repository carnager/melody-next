// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/output/stream_query.hpp"

#include <cstdint>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace trackknife::engine {

using output::StreamFormat;

// A track as the engine plays it: a file, and which part of it.
struct TranscodeSource final {
    std::string raw_path;
    formats::AudioSourceSelection selection;
    std::optional<formats::SampleRange> segment;
};

// Tracks converted for streaming, kept so a seek, a replay or a download
// costs nothing the second time. A finished file is served like any other --
// with a length, so seeking and ranges work -- which a live encode could not
// offer. Keyed by the file's revision, so a retagged or replaced file is
// converted afresh; the least recently used go when the cache is over its
// size.
class TranscodeCache final {
  public:
    TranscodeCache(std::filesystem::path directory, std::uint64_t capacity_bytes);

    // The converted file, made now if it is not there yet. Two asking for
    // the same track at once share one conversion.
    [[nodiscard]] core::Result<std::filesystem::path> ensure(const TranscodeSource& source,
                                                             const StreamFormat& format);

  private:
    [[nodiscard]] core::Result<std::filesystem::path> convert(const TranscodeSource& source,
                                                              const StreamFormat& format,
                                                              const std::filesystem::path& target);
    void evict(const std::filesystem::path& keep);

    std::filesystem::path directory_;
    std::uint64_t capacity_bytes_;
    std::mutex mutex_;
    std::map<std::string, std::shared_future<core::Result<std::filesystem::path>>> converting_;
};

} // namespace trackknife::engine
