// SPDX-License-Identifier: GPL-3.0-only
//
// Covers scaled by the engine for a client that shows them small: a phone
// scrolling a grid pays for every byte of every original.

#include "trackknife/formats/cover_fitting.hpp"
#include "trackknife/protocol/message.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace formats = trackknife::formats;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

[[nodiscard]] std::vector<std::uint8_t> fixture(const std::filesystem::path& encoded) {
    std::ifstream input{encoded};
    require(input.good(), "the fixture opens");
    std::string base64((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::erase(base64, '\n');
    const auto decoded = trackknife::protocol::decode_raw_path(base64);
    require(decoded.has_value(), "the fixture decodes");
    return {decoded->begin(), decoded->end()};
}

[[nodiscard]] bool jpeg(const std::vector<std::uint8_t>& image) {
    return image.size() > 3U && image[0] == 0xFF && image[1] == 0xD8 && image[2] == 0xFF;
}

// A JPEG's longest edge, as its frame header says.
[[nodiscard]] int longest_edge(const std::vector<std::uint8_t>& image) {
    std::size_t at = 2U;
    while (at + 9U < image.size() && image[at] == 0xFF) {
        const auto marker = image[at + 1U];
        const auto length = static_cast<std::size_t>((image[at + 2U] << 8) | image[at + 3U]);
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 &&
            marker != 0xCC) {
            const auto height = (image[at + 5U] << 8) | image[at + 6U];
            const auto width = (image[at + 7U] << 8) | image[at + 8U];
            return std::max(width, height);
        }
        at += 2U + length;
    }
    require(false, "the JPEG has a frame header");
    return 0;
}

[[nodiscard]] bool fits(const std::vector<std::uint8_t>& image, const int edge) {
    return longest_edge(image) <= edge;
}

// The JPEG with comments after its start: as heavy as an embedded cover
// carrying a megabyte of what nobody sees, and no larger to look at.
[[nodiscard]] std::vector<std::uint8_t> padded(const std::vector<std::uint8_t>& jpeg_image,
                                               const std::size_t segments) {
    std::vector<std::uint8_t> heavy{jpeg_image.begin(), jpeg_image.begin() + 2};
    for (std::size_t index = 0; index < segments; ++index) {
        constexpr std::size_t length = 65'000U;
        heavy.insert(heavy.end(), {0xFF, 0xFE, static_cast<std::uint8_t>(length >> 8),
                                   static_cast<std::uint8_t>(length & 0xFFU)});
        heavy.insert(heavy.end(), length - 2U, static_cast<std::uint8_t>('x'));
    }
    heavy.insert(heavy.end(), jpeg_image.begin() + 2, jpeg_image.end());
    return heavy;
}

// A cover all grain -- the worst a film still or a scanned sleeve can be --
// `side` pixels square, as JPEG at the finest quantiser: well over a
// megabyte at 1000.
[[nodiscard]] std::vector<std::uint8_t> grainy(const int side) {
    const auto* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    require(codec != nullptr, "a JPEG encoder");
    auto* context = avcodec_alloc_context3(codec);
    context->width = side;
    context->height = side;
    context->pix_fmt = AV_PIX_FMT_YUVJ420P;
    context->time_base = AVRational{1, 1};
    context->flags |= AV_CODEC_FLAG_QSCALE;
    context->global_quality = FF_QP2LAMBDA * 2;
    require(avcodec_open2(context, codec, nullptr) >= 0, "the encoder opens");
    auto* frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_YUVJ420P;
    frame->width = side;
    frame->height = side;
    require(av_frame_get_buffer(frame, 0) >= 0, "a frame");
    std::mt19937 random{7U};
    for (int plane = 0; plane < 3; ++plane) {
        const auto rows = plane == 0 ? side : side / 2;
        for (int row = 0; row < rows; ++row) {
            for (int column = 0; column < frame->linesize[plane]; ++column) {
                frame->data[plane][row * frame->linesize[plane] + column] =
                    static_cast<std::uint8_t>(random() & 0xFFU);
            }
        }
    }
    frame->quality = context->global_quality;
    auto* packet = av_packet_alloc();
    require(avcodec_send_frame(context, frame) >= 0 && avcodec_send_frame(context, nullptr) >= 0 &&
                avcodec_receive_packet(context, packet) >= 0,
            "the grain is encoded");
    std::vector<std::uint8_t> image(packet->data, packet->data + packet->size);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&context);
    return image;
}

} // namespace

int main(int argc, char** argv) {
    require(argc == 2, "usage: cover_fitting_test <fixture-dir>");
    const std::filesystem::path fixtures{argv[1]};
    const auto png = fixture(fixtures / "cover-64-png.b64");
    const auto small_jpeg = fixture(fixtures / "external-blue-jpeg.b64");

    // Within the size and lean: the original, byte for byte -- re-encoding
    // would only lose quality and gain nothing.
    auto unchanged = formats::fit_cover(png, 64);
    require(unchanged.has_value() && *unchanged == png, "a cover within the size is sent as it is");
    unchanged = formats::fit_cover(small_jpeg, 1024);
    require(unchanged.has_value() && *unchanged == small_jpeg, "a lean JPEG is sent as it is");
    // Within the size and heavy -- a cover embedded at half a megabyte --
    // re-encoded: as large to look at, and light.
    const auto heavy = padded(small_jpeg, 8U);
    require(heavy.size() > 500'000U && longest_edge(heavy) == longest_edge(small_jpeg),
            "the padded fixture is heavy and the same size");
    const auto lightened = formats::fit_cover(heavy, 1024);
    require(lightened.has_value() && jpeg(*lightened), "a heavy cover comes back as JPEG");
    require(lightened->size() < 4'096U, "and light");
    require(longest_edge(*lightened) == longest_edge(small_jpeg), "and never made larger");
    // All grain, within the size: coarser until it is within 0.3 bytes a
    // pixel -- where the last step allows -- and a phone is never sent the
    // megabyte.
    const auto grain = grainy(1000);
    require(grain.size() > 1'000'000U, "the grain is heavy");
    const auto fitted_grain = formats::fit_cover(grain, 1024);
    require(fitted_grain.has_value() && jpeg(*fitted_grain), "grain comes back as JPEG");
    require(longest_edge(*fitted_grain) == 1000, "as large as it was");
    require(fitted_grain->size() < grain.size() / 2U, "and less than half the weight");
    // Scaled down, the same budget: a 1000-pixel cover asked for at 512.
    const auto smaller_grain = formats::fit_cover(grain, 512);
    require(smaller_grain.has_value() && longest_edge(*smaller_grain) == 512, "scaled to 512");
    require(smaller_grain->size() < fitted_grain->size(), "and lighter still");
    unchanged = formats::fit_cover(png, 0);
    require(unchanged.has_value() && *unchanged == png, "no size asked is no scaling");

    // Over it: a JPEG exactly that size on its longest edge.
    const auto fitted = formats::fit_cover(png, 16);
    require(fitted.has_value(), "a 64-pixel PNG fits into 16");
    require(jpeg(*fitted), "and comes back as JPEG");
    require(fitted->size() < 4'096U, "small");
    require(fits(*fitted, 16), "no longer than 16 pixels");
    require(!fits(*fitted, 15), "and not smaller than asked");

    // Aspect kept: an 8x6 JPEG into 4 is 4 wide, and still a JPEG.
    const auto narrow = formats::fit_cover(small_jpeg, 4);
    require(narrow.has_value() && jpeg(*narrow), "a JPEG is scaled too");
    require(fits(*narrow, 4) && !fits(*narrow, 3), "to the size asked on its longer side");

    // Not an image: said, not sent on as though it were one.
    const std::vector<std::uint8_t> nonsense{'n', 'o', 't', ' ', 'a', 'n',
                                             ' ', 'i', 'm', 'a', 'g', 'e'};
    require(!formats::fit_cover(nonsense, 16).has_value(), "bytes that are no image are refused");

    std::cout << "cover fitting: ok\n";
    return EXIT_SUCCESS;
}
