// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/formats/artwork.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

#include <memory>
#include <fstream>
#include <filesystem>
#include <array>
#include <algorithm>

namespace trackknife::formats {
namespace {

struct FormatCloser {
    void operator()(AVFormatContext* context) const noexcept {
        if (context != nullptr) {
            avformat_close_input(&context);
        }
    }
};

struct InterruptState {
    const core::CancellationToken* cancellation{nullptr};
};

int interrupt_callback(void* opaque) {
    const auto* state = static_cast<const InterruptState*>(opaque);
    return state != nullptr && state->cancellation != nullptr &&
                   state->cancellation->is_cancellation_requested()
               ? 1
               : 0;
}

} // namespace

core::Result<std::vector<unsigned char>>
load_embedded_artwork(const std::string& raw_path, const core::CancellationToken& cancellation) {
    if (raw_path.empty()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "local media path is empty",
            .context = {},
        });
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::cancelled,
            .message = "artwork load was cancelled",
            .context = {{.key = "path", .value = raw_path}},
        });
    }

    InterruptState interrupt{.cancellation = &cancellation};
    auto* allocated = avformat_alloc_context();
    if (allocated == nullptr) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::backend,
            .message = "FFmpeg could not allocate a format context",
            .context = {{.key = "path", .value = raw_path}},
        });
    }
    allocated->interrupt_callback = AVIOInterruptCB{
        .callback = interrupt_callback,
        .opaque = &interrupt,
    };
    auto* opened = allocated;
    if (avformat_open_input(&opened, raw_path.c_str(), nullptr, nullptr) < 0) {
        if (opened != nullptr) {
            avformat_free_context(opened);
        }
        return std::unexpected(core::Error{
            .code = core::ErrorCode::io,
            .message = "opening local media for artwork failed",
            .context = {{.key = "path", .value = raw_path}},
        });
    }
    std::unique_ptr<AVFormatContext, FormatCloser> format{opened};
    if (avformat_find_stream_info(format.get(), nullptr) < 0) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::io,
            .message = "reading local media streams for artwork failed",
            .context = {{.key = "path", .value = raw_path}},
        });
    }

    for (unsigned index = 0U; index < format->nb_streams; ++index) {
        const auto* stream = format->streams[index];
        if ((stream->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
            continue;
        }
        const auto& picture = stream->attached_pic;
        if (picture.data == nullptr || picture.size <= 0) {
            continue;
        }
        if (picture.size > 16 * 1024 * 1024) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::limit_exceeded,
                .message = "embedded artwork exceeds the 16 MiB thumbnail input limit",
                .context = {{.key = "path", .value = raw_path}},
            });
        }
        return std::vector<unsigned char>{picture.data, picture.data + picture.size};
    }
    return std::unexpected(core::Error{
        .code = core::ErrorCode::not_found,
        .message = "local media has no attached picture",
        .context = {{.key = "path", .value = raw_path}},
    });
}

namespace {

constexpr std::uintmax_t track_artwork_limit = 16U * 1024U * 1024U;

std::vector<unsigned char> folder_artwork(const std::string& raw_path,
                                         const core::CancellationToken& cancellation) {
    const auto directory = std::filesystem::path{raw_path}.parent_path();
    static constexpr std::array names{"cover.jpg",  "cover.jpeg",  "cover.png",
                                      "folder.jpg", "folder.jpeg", "folder.png",
                                      "front.jpg",  "front.jpeg",  "front.png"};
    std::error_code error;
    std::filesystem::directory_iterator iterator{
        directory, std::filesystem::directory_options::skip_permission_denied, error};
    if (error) {
        return {};
    }
    std::size_t visited = 0;
    for (; iterator != std::filesystem::directory_iterator{}; iterator.increment(error)) {
        if (error || cancellation.is_cancellation_requested() || ++visited > 10'000U) {
            return {};
        }
        auto name = iterator->path().filename().native();
        for (auto& byte : name) {
            if (byte >= 'A' && byte <= 'Z') {
                byte = static_cast<char>(byte - 'A' + 'a');
            }
        }
        if (std::ranges::find(names, name) == names.end() || !iterator->is_regular_file(error)) {
            error.clear();
            continue;
        }
        const auto size = iterator->file_size(error);
        if (error || size == 0U || size > track_artwork_limit) {
            return {};
        }
        std::ifstream input{iterator->path(), std::ios::binary};
        std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        if (input.gcount() != static_cast<std::streamsize>(size)) {
            return {};
        }
        return bytes;
    }
    return {};
}

} // namespace

std::vector<unsigned char> load_track_artwork(const std::string& raw_path,
                                              const core::CancellationToken& cancellation) {
    if (auto embedded = load_embedded_artwork(raw_path, cancellation);
        embedded && !embedded->empty()) {
        return std::move(*embedded);
    }
    if (cancellation.is_cancellation_requested()) {
        return {};
    }
    return folder_artwork(raw_path, cancellation);
}

} // namespace trackknife::formats
