// SPDX-License-Identifier: GPL-3.0-only
#include "trackknife/formats/cover_fitting.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/display.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace trackknife::formats {
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
    if (starts({'I', 'I', 0x2A, 0x00}) || starts({'M', 'M', 0x00, 0x2A})) {
        return AV_CODEC_ID_TIFF;
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

[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_jpeg(const AVFrame& picture,
                                                                  const int quantiser) {
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
    // A quantiser rather than a bitrate: a still has no rate.
    context->flags |= AV_CODEC_FLAG_QSCALE;
    context->global_quality = FF_QP2LAMBDA * quantiser;
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

// As JPEG within its budget: 3 looks like the original, and is kept where
// the cover allows; a grainy one -- film grain, a scanned sleeve -- is coarser
// until it fits, the last step taken whatever it weighs.
[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_within(const AVFrame& picture,
                                                                    const std::size_t budget) {
    core::Result<std::vector<std::uint8_t>> encoded;
    for (const auto quantiser : {3, 5, 7, 10, 14, 20}) {
        encoded = encode_jpeg(picture, quantiser);
        if (!encoded || encoded->size() <= budget) {
            break;
        }
    }
    return encoded;
}

// How a decoded picture is turned to be seen upright -- a photo's EXIF
// orientation, which FFmpeg hands on as a display matrix: where a pixel at
// (x, y) goes, as x·(a, b) + y·(c, d). Only quarter turns and mirrors.
struct Orientation {
    int a{1};
    int b{0};
    int c{0};
    int d{1};
    [[nodiscard]] bool upright() const { return a == 1 && b == 0 && c == 0 && d == 1; }
    [[nodiscard]] bool turned() const { return a == 0; }
};

[[nodiscard]] Orientation orientation_of(const AVFrame& frame) {
    const auto* side = av_frame_get_side_data(&frame, AV_FRAME_DATA_DISPLAYMATRIX);
    if (side == nullptr || side->size < 9U * sizeof(std::int32_t)) {
        return {};
    }
    std::array<std::int32_t, 9> matrix{};
    std::memcpy(matrix.data(), side->data, sizeof(matrix));
    const auto unit = [](const std::int32_t value) {
        return value > (1 << 15) ? 1 : value < -(1 << 15) ? -1 : 0;
    };
    const Orientation found{
        .a = unit(matrix[0]), .b = unit(matrix[1]), .c = unit(matrix[3]), .d = unit(matrix[4])};
    const bool quarter = std::abs(found.a) + std::abs(found.b) == 1 &&
                         std::abs(found.c) + std::abs(found.d) == 1 &&
                         (found.a != 0) != (found.c != 0);
    return quarter ? found : Orientation{};
}

// The picture at `width` × `height` as decoded, ready for the JPEG encoder:
// scaled smoothly, any transparency over white -- a cover is a JPEG -- and
// turned upright.
[[nodiscard]] core::Result<Frame> render(const AVFrame& source, const int width, const int height) {
    Scaler to_rgba{sws_getContext(source.width, source.height,
                                  static_cast<AVPixelFormat>(source.format), width, height,
                                  AV_PIX_FMT_RGBA, SWS_LANCZOS, nullptr, nullptr, nullptr)};
    Scaler to_yuv{sws_getContext(width, height, AV_PIX_FMT_RGBA, width, height, AV_PIX_FMT_YUVJ420P,
                                 SWS_POINT, nullptr, nullptr, nullptr)};
    Frame rgba{av_frame_alloc()};
    Frame target{av_frame_alloc()};
    if (!to_rgba || !to_yuv || !rgba || !target) {
        return std::unexpected(failure("the cover's pixel format cannot be scaled"));
    }
    rgba->format = AV_PIX_FMT_RGBA;
    rgba->width = width;
    rgba->height = height;
    target->format = AV_PIX_FMT_YUVJ420P;
    target->width = width;
    target->height = height;
    if (av_frame_get_buffer(rgba.get(), 0) < 0 || av_frame_get_buffer(target.get(), 0) < 0 ||
        sws_scale(to_rgba.get(), source.data, source.linesize, 0, source.height, rgba->data,
                  rgba->linesize) != height) {
        return std::unexpected(failure("the cover could not be scaled"));
    }
    for (int y = 0; y < height; ++y) {
        auto* pixel = rgba->data[0] + static_cast<std::ptrdiff_t>(y) * rgba->linesize[0];
        for (int x = 0; x < width; ++x, pixel += 4) {
            const unsigned alpha = pixel[3];
            for (int channel = 0; channel < 3; ++channel) {
                pixel[channel] = static_cast<std::uint8_t>(
                    (pixel[channel] * alpha + 255U * (255U - alpha) + 127U) / 255U);
            }
            pixel[3] = 255U;
        }
    }
    const auto orientation = orientation_of(source);
    if (!orientation.upright()) {
        const auto shown_width = orientation.turned() ? height : width;
        const auto shown_height = orientation.turned() ? width : height;
        Frame shown{av_frame_alloc()};
        if (!shown) {
            return std::unexpected(failure("the cover could not be turned"));
        }
        shown->format = AV_PIX_FMT_RGBA;
        shown->width = shown_width;
        shown->height = shown_height;
        if (av_frame_get_buffer(shown.get(), 0) < 0) {
            return std::unexpected(failure("the cover could not be turned"));
        }
        const auto offset_x =
            -(std::min(0, orientation.a) * (width - 1) + std::min(0, orientation.c) * (height - 1));
        const auto offset_y =
            -(std::min(0, orientation.b) * (width - 1) + std::min(0, orientation.d) * (height - 1));
        for (int y = 0; y < height; ++y) {
            const auto* from = rgba->data[0] + static_cast<std::ptrdiff_t>(y) * rgba->linesize[0];
            for (int x = 0; x < width; ++x, from += 4) {
                const auto to_x = orientation.a * x + orientation.c * y + offset_x;
                const auto to_y = orientation.b * x + orientation.d * y + offset_y;
                std::memcpy(shown->data[0] +
                                static_cast<std::ptrdiff_t>(to_y) * shown->linesize[0] +
                                static_cast<std::ptrdiff_t>(to_x) * 4,
                            from, 4U);
            }
        }
        rgba = std::move(shown);
        to_yuv.reset(sws_getContext(shown_width, shown_height, AV_PIX_FMT_RGBA, shown_width,
                                    shown_height, AV_PIX_FMT_YUVJ420P, SWS_POINT, nullptr, nullptr,
                                    nullptr));
        target.reset(av_frame_alloc());
        if (!to_yuv || !target) {
            return std::unexpected(failure("the cover could not be turned"));
        }
        target->format = AV_PIX_FMT_YUVJ420P;
        target->width = shown_width;
        target->height = shown_height;
        if (av_frame_get_buffer(target.get(), 0) < 0) {
            return std::unexpected(failure("the cover could not be turned"));
        }
    }
    if (sws_scale(to_yuv.get(), rgba->data, rgba->linesize, 0, rgba->height, target->data,
                  target->linesize) != rgba->height) {
        return std::unexpected(failure("the cover could not be converted"));
    }
    return target;
}

// The cover decoded, refused where it is no picture or more pixels than any
// cover needs -- a decoder is not handed a bomb.
[[nodiscard]] core::Result<Frame> decode_cover(std::span<const std::uint8_t> image) {
    const auto id = sniff(image);
    if (!id) {
        return std::unexpected(failure("the cover is in a format that cannot be scaled"));
    }
    auto decoded = decode(image, *id);
    if (!decoded) {
        return std::unexpected(std::move(decoded.error()));
    }
    const auto& source = **decoded;
    if (source.width <= 0 || source.height <= 0) {
        return std::unexpected(failure("the cover has no size"));
    }
    if (static_cast<std::int64_t>(source.width) * source.height > maximum_cover_pixels) {
        return std::unexpected(failure("the cover is too large to read"));
    }
    return decoded;
}

} // namespace

core::Result<std::optional<FittedCover>> fit_cover_for_writing(std::span<const std::uint8_t> image,
                                                               const std::uint32_t longest_edge) {
    auto decoded = decode_cover(image);
    if (!decoded) {
        return std::unexpected(std::move(decoded.error()));
    }
    const auto& source = **decoded;
    const auto longest = std::max(source.width, source.height);
    const auto edge = longest_edge == 0U ? longest
                                         : static_cast<int>(std::min<std::uint32_t>(
                                               longest_edge, static_cast<std::uint32_t>(longest)));
    // A JPEG that fits is written as it is.
    if (sniff(image) == AV_CODEC_ID_MJPEG && edge == longest) {
        return std::optional<FittedCover>{};
    }
    const auto scaled = [&](const int side) {
        return std::max(1, static_cast<int>(static_cast<std::int64_t>(side) * edge / longest));
    };
    const auto width = scaled(source.width);
    const auto height = scaled(source.height);
    auto target = render(source, width, height);
    if (!target) {
        return std::unexpected(std::move(target.error()));
    }
    // Quantiser 2: what a JPEG at quality 90 is.
    auto encoded = encode_jpeg(**target, 2);
    if (!encoded) {
        return std::unexpected(std::move(encoded.error()));
    }
    return std::optional{FittedCover{
        .bytes = std::move(*encoded), .width = (*target)->width, .height = (*target)->height}};
}

core::Result<std::vector<std::uint8_t>> fit_cover(std::span<const std::uint8_t> image,
                                                  const int longest_edge) {
    const auto original = [&image] {
        return std::vector<std::uint8_t>(image.begin(), image.end());
    };
    if (longest_edge <= 0 || image.empty()) {
        return original();
    }
    auto decoded = decode_cover(image);
    if (!decoded) {
        return std::unexpected(std::move(decoded.error()));
    }
    const auto& source = **decoded;
    const auto longest = std::max(source.width, source.height);
    // Within the size, it is not made larger -- but it is made lighter: a
    // 1000-pixel cover embedded as a megabyte is no cover for a phone.
    const bool within = longest <= longest_edge;
    const auto edge = within ? longest : longest_edge;
    // Aspect kept; each side at least a pixel, and even, which 4:2:0 wants.
    const auto scaled = [&](int side) {
        const auto value = static_cast<int>(static_cast<std::int64_t>(side) * edge / longest);
        return std::max(2, value & ~1);
    };
    const auto width = scaled(source.width);
    const auto height = scaled(source.height);
    // A budget of 0.3 bytes a pixel -- 300 KB for a cover a phone's width
    // across, 5 KB for a thumbnail -- and never less than a tiny one needs.
    const auto budget = std::max<std::size_t>(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U / 10U, 4'096U);
    // One already within the size and the budget is sent as it was, losing
    // nothing; a heavier one is re-encoded.
    if (within && image.size() <= budget) {
        return original();
    }

    auto target = render(source, width, height);
    if (!target) {
        return std::unexpected(std::move(target.error()));
    }
    auto encoded = encode_within(**target, budget);
    if (!encoded) {
        return std::unexpected(std::move(encoded.error()));
    }
    if (within && encoded->size() >= image.size()) {
        return original();
    }
    return std::move(*encoded);
}

} // namespace trackknife::formats
