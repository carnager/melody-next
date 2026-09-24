// SPDX-License-Identifier: GPL-3.0-only
//
// Covers scaled by the engine for a client that shows them small: a phone
// scrolling a grid pays for every byte of every original.

#include "trackknife/engine/cover_fitting.hpp"
#include "trackknife/protocol/message.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace engine = trackknife::engine;

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

// Whether an image's longest edge is at most `edge`: fitting it to that
// size hands it back untouched exactly when it already fits.
[[nodiscard]] bool fits(const std::vector<std::uint8_t>& image, const int edge) {
    const auto again = engine::fit_cover(image, edge);
    require(again.has_value(), "a fitted cover can be fitted again");
    return *again == image;
}

} // namespace

int main(int argc, char** argv) {
    require(argc == 2, "usage: cover_fitting_test <fixture-dir>");
    const std::filesystem::path fixtures{argv[1]};
    const auto png = fixture(fixtures / "cover-64-png.b64");
    const auto small_jpeg = fixture(fixtures / "external-blue-jpeg.b64");

    // Within the size: the original, byte for byte -- re-encoding would only
    // lose quality and gain nothing.
    auto unchanged = engine::fit_cover(png, 64);
    require(unchanged.has_value() && *unchanged == png, "a cover within the size is sent as it is");
    unchanged = engine::fit_cover(png, 0);
    require(unchanged.has_value() && *unchanged == png, "no size asked is no scaling");

    // Over it: a JPEG exactly that size on its longest edge.
    const auto fitted = engine::fit_cover(png, 16);
    require(fitted.has_value(), "a 64-pixel PNG fits into 16");
    require(jpeg(*fitted), "and comes back as JPEG");
    require(fitted->size() < 4'096U, "small");
    require(fits(*fitted, 16), "no longer than 16 pixels");
    require(!fits(*fitted, 15), "and not smaller than asked");

    // Aspect kept: an 8x6 JPEG into 4 is 4 wide, and still a JPEG.
    const auto narrow = engine::fit_cover(small_jpeg, 4);
    require(narrow.has_value() && jpeg(*narrow), "a JPEG is scaled too");
    require(fits(*narrow, 4) && !fits(*narrow, 3), "to the size asked on its longer side");

    // Not an image: said, not sent on as though it were one.
    const std::vector<std::uint8_t> nonsense{'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'a',
                                             'g', 'e'};
    require(!engine::fit_cover(nonsense, 16).has_value(), "bytes that are no image are refused");

    std::cout << "cover fitting: ok\n";
    return EXIT_SUCCESS;
}
