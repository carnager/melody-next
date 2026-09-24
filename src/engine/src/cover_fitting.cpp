// SPDX-License-Identifier: GPL-3.0-only
#include "trackknife/engine/cover_fitting.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace trackknife::engine {
namespace {

[[nodiscard]] core::Error failure(std::string message) {
    return core::Error{
        .code = core::ErrorCode::unsupported, .message = std::move(message), .context = {}};
}

// What the bytes are, from their first bytes: a tag says nothing reliable
// about the picture it holds, and FFmpeg's image decoders are chosen by id.
[[nodiscard]] std::optional<AVCodecID> sniff(std::span<const std::uint8_t> image) {
    const auto starts = [&image](std::initializer_list<std::uint8_t> magic, std::size_t at = 0) {
        return image.size() >= at + magic.size() &&
               std::equal(magic.begin(), magic.end(), image.begin() + static_cast<long>(at));
    };
    if (starts({0xFF, 0xD8, 0xFF})) {
        return AV_CODEC_ID_MJPEG;
    }
    if (starts({0x89, 'P', 'N', 'G'})) {
        return AV_CODEC_ID_PNG;
    }
    if (starts({'R', 'I', 'F', 'F'}) && starts({'W', 'E', 'B', 'P'}, 8)) {
        return AV_CODEC_ID_WEBP;
    }
    if (starts({'G', 'I', 'F', '8'})) {
        return AV_CODEC_ID_GIF;
    }
    if (starts({'B', 'M'})) {
        return AV_CODEC_ID_BMP;
    }
    return std::nullopt;
}

struct CodecContextDeleter {
    void operator()(AVCodecContext* context) const { avcodec_free_context(&context); }
};
struct FrameDeleter {
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};
struct PacketDeleter {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};
struct ScalerDeleter {
    void operator()(SwsContext* scaler) const { sws_freeContext(scaler); }
};
using CodecContext = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using Frame = std::unique_ptr<AVFrame, FrameDeleter>;
using Packet = std::unique_ptr<AVPacket, PacketDeleter>;
using Scaler = std::unique_ptr<SwsContext, ScalerDeleter>;

[[nodiscard]] core::Result<Frame> decode(std::span<const std::uint8_t> image, AVCodecID id) {
    const auto* codec = avcodec_find_decoder(id);
    if (codec == nullptr) {
        return std::unexpected(failure("no decoder for this cover's format"));
    }
    CodecContext context{avcodec_alloc_context3(codec)};
    Packet packet{av_packet_alloc()};
    Frame frame{av_frame_alloc()};
    if (!context || !packet || !frame || avcodec_open2(context.get(), codec, nullptr) < 0) {
        return std::unexpected(failure("could not open the cover's decoder"));
    }
    // A packet is read with padding past its end, so the bytes are copied
    // into a buffer that has it rather than handed over as they are.
    if (image.size() > static_cast<std::size_t>(INT32_MAX - AV_INPUT_BUFFER_PADDING_SIZE) ||
        av_new_packet(packet.get(), static_cast<int>(image.size())) < 0) {
        return std::unexpected(failure("the cover is too large to decode"));
    }
    std::memcpy(packet->data, image.data(), image.size());
    if (avcodec_send_packet(context.get(), packet.get()) < 0 ||
        avcodec_send_packet(context.get(), nullptr) < 0 ||
        avcodec_receive_frame(context.get(), frame.get()) < 0) {
        return std::unexpected(failure("the cover could not be decoded"));
    }
    return frame;
}

[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_jpeg(const AVFrame& picture) {
    const auto* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (codec == nullptr) {
        return std::unexpected(failure("no JPEG encoder"));
    }
    CodecContext context{avcodec_alloc_context3(codec)};
    Packet packet{av_packet_alloc()};
    if (!context || !packet) {
        return std::unexpected(failure("could not prepare the JPEG encoder"));
    }
    context->width = picture.width;
    context->height = picture.height;
    context->pix_fmt = AV_PIX_FMT_YUVJ420P;
    context->color_range = AVCOL_RANGE_JPEG;
    context->time_base = AVRational{1, 1};
    // A fixed quantiser rather than a bitrate: a still has no rate, and
    // this is what makes the quality the same for every cover. 3 looks like
    // the original at thumbnail sizes.
    context->flags |= AV_CODEC_FLAG_QSCALE;
    context->global_quality = FF_QP2LAMBDA * 3;
    if (avcodec_open2(context.get(), codec, nullptr) < 0) {
        return std::unexpected(failure("could not open the JPEG encoder"));
    }
    Frame frame{av_frame_clone(&picture)};
    if (!frame) {
        return std::unexpected(failure("could not prepare the scaled cover"));
    }
    frame->quality = context->global_quality;
    frame->pts = 0;
    if (avcodec_send_frame(context.get(), frame.get()) < 0 ||
        avcodec_send_frame(context.get(), nullptr) < 0 ||
        avcodec_receive_packet(context.get(), packet.get()) < 0) {
        return std::unexpected(failure("the scaled cover could not be encoded"));
    }
    return std::vector<std::uint8_t>(packet->data, packet->data + packet->size);
}

} // namespace

core::Result<std::vector<std::uint8_t>> fit_cover(std::span<const std::uint8_t> image,
                                                  const int longest_edge) {
    const auto original = [&image] { return std::vector<std::uint8_t>(image.begin(), image.end()); };
    if (longest_edge <= 0 || image.empty()) {
        return original();
    }
    const auto id = sniff(image);
    if (!id) {
        return std::unexpected(failure("the cover is in a format that cannot be scaled"));
    }
    auto decoded = decode(image, *id);
    if (!decoded) {
        return std::unexpected(std::move(decoded.error()));
    }
    const auto& source = **decoded;
    const auto longest = std::max(source.width, source.height);
    if (source.width <= 0 || source.height <= 0) {
        return std::unexpected(failure("the cover has no size"));
    }
    if (longest <= longest_edge) {
        return original();
    }
    // Aspect kept; each side at least a pixel, and even, which 4:2:0 wants.
    const auto scaled = [&](int side) {
        const auto value = static_cast<int>(static_cast<std::int64_t>(side) * longest_edge / longest);
        return std::max(2, value & ~1);
    };
    const auto width = scaled(source.width);
    const auto height = scaled(source.height);

    Scaler scaler{sws_getContext(source.width, source.height,
                                 static_cast<AVPixelFormat>(source.format), width, height,
                                 AV_PIX_FMT_YUVJ420P, SWS_LANCZOS, nullptr, nullptr, nullptr)};
    Frame target{av_frame_alloc()};
    if (!scaler || !target) {
        return std::unexpected(failure("the cover's pixel format cannot be scaled"));
    }
    target->format = AV_PIX_FMT_YUVJ420P;
    target->width = width;
    target->height = height;
    if (av_frame_get_buffer(target.get(), 0) < 0 ||
        sws_scale(scaler.get(), source.data, source.linesize, 0, source.height, target->data,
                  target->linesize) != height) {
        return std::unexpected(failure("the cover could not be scaled"));
    }
    return encode_jpeg(*target);
}

} // namespace trackknife::engine
