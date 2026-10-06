// SPDX-License-Identifier: GPL-3.0-only

// A file renamed over while it is decoded -- a tag write does that -- is
// reopened and decoded on from the next sample. Through another machine's
// NFS mount the file that was opened is gone with the rename and reading it
// fails mid-track; here the old file is emptied through a second name to the
// same effect.

#include "trackknife/formats/decoder.hpp"

#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

namespace formats = trackknife::formats;

int failures = 0;

void check(const bool condition, const char* expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

constexpr std::uint32_t rate = 8'000;
constexpr std::int64_t frames = rate * 10;

// A ramp, so every sample says where it is.
[[nodiscard]] std::int16_t ramp(const std::int64_t sample) {
    return static_cast<std::int16_t>(sample % 30'000);
}

bool write_ramp(const std::filesystem::path& path) {
    std::ofstream file{path, std::ios::binary};
    const auto u32 = [&file](const std::uint32_t value) {
        for (unsigned shift = 0U; shift < 32U; shift += 8U) {
            file.put(static_cast<char>((value >> shift) & 0xFFU));
        }
    };
    const auto u16 = [&file](const std::uint16_t value) {
        file.put(static_cast<char>(value & 0xFFU));
        file.put(static_cast<char>((value >> 8U) & 0xFFU));
    };
    const auto data_bytes = static_cast<std::uint32_t>(frames * 2);
    file.write("RIFF", 4);
    u32(36U + data_bytes);
    file.write("WAVEfmt ", 8);
    u32(16U);
    u16(1U);
    u16(1U);
    u32(rate);
    u32(rate * 2U);
    u16(2U);
    u16(16U);
    file.write("data", 4);
    u32(data_bytes);
    for (std::int64_t sample = 0; sample < frames; ++sample) {
        u16(static_cast<std::uint16_t>(ramp(sample)));
    }
    return file.good();
}

// Every chunk from here on: where it starts, and that its samples are the
// ramp's at that place. Answers the samples read, or -1 on an error.
std::int64_t read_on(formats::AudioDecoder& decoder, std::int64_t next, const std::int64_t until) {
    while (next < until) {
        auto chunk = decoder.next_chunk();
        if (!chunk) {
            std::cerr << "decoding failed: " << chunk.error().message << '\n';
            return -1;
        }
        if (!*chunk) {
            break;
        }
        CHECK((*chunk)->start_sample == next);
        const auto& samples = (*chunk)->interleaved_samples;
        for (std::size_t index = 0; index < samples.size(); ++index) {
            const auto expected = static_cast<float>(ramp(next + static_cast<std::int64_t>(index))) / 32768.0F;
            if (std::abs(samples[index] - expected) > 1e-4F) {
                std::cerr << "sample " << next + static_cast<std::int64_t>(index) << " is " << samples[index]
                          << ", not " << expected << '\n';
                ++failures;
                return -1;
            }
        }
        next += static_cast<std::int64_t>(samples.size());
    }
    return next;
}

} // namespace

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("trackknife-replaced-" + std::to_string(::getpid()));
    std::filesystem::create_directories(directory);
    const auto path = directory / "track.wav";
    CHECK(write_ramp(path));

    auto decoder = formats::AudioDecoder::open(path.string());
    CHECK(decoder.has_value());
    if (!decoder) {
        return EXIT_FAILURE;
    }
    auto reached = read_on(*decoder, 0, frames / 4);
    CHECK(reached >= frames / 4);

    // A tag write: the same audio in a new file, renamed over this one.
    const auto prepared = directory / ".track.prepared";
    const auto opened = directory / "opened";
    std::filesystem::copy_file(path, prepared);
    std::filesystem::create_hard_link(path, opened);
    std::filesystem::rename(prepared, path);
    // What another machine's NFS mount does to the file it had open: gone.
    std::filesystem::resize_file(opened, 0);

    reached = read_on(*decoder, reached, frames);
    CHECK(reached == frames);

    // A file only emptied, not replaced, is not chased: what can be read is,
    // then it ends.
    {
        const auto alone = directory / "alone.wav";
        CHECK(write_ramp(alone));
        auto other = formats::AudioDecoder::open(alone.string());
        CHECK(other.has_value());
        auto partway = read_on(*other, 0, frames / 4);
        std::filesystem::resize_file(alone, 0);
        partway = read_on(*other, partway, frames);
        CHECK(partway < frames);
    }

    std::filesystem::remove_all(directory);
    if (failures != 0) {
        std::cerr << failures << " failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
