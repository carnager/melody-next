// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/token.hpp"

#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

namespace trackknife::engine {
namespace {

[[nodiscard]] core::Error token_error(std::string message, const std::filesystem::path& path) {
    return core::Error{.code = core::ErrorCode::io,
                       .message = std::move(message),
                       .context = {{.key = "path", .value = path.string()},
                                   {.key = "errno", .value = std::strerror(errno)}}};
}

[[nodiscard]] core::Result<std::string> generate() {
    std::array<unsigned char, 32> bytes{};
    std::size_t filled = 0;
    while (filled < bytes.size()) {
        const auto got = ::getrandom(bytes.data() + filled, bytes.size() - filled, 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(
                core::Error{.code = core::ErrorCode::io,
                            .message = "could not gather randomness",
                            .context = {{.key = "errno", .value = std::strerror(errno)}}});
        }
        filled += static_cast<std::size_t>(got);
    }
    constexpr std::string_view digits{"0123456789abcdef"};
    std::string text;
    text.reserve(bytes.size() * 2U);
    for (const auto byte : bytes) {
        text.push_back(digits[byte >> 4U]);
        text.push_back(digits[byte & 0x0FU]);
    }
    return text;
}

} // namespace

core::Result<std::string> load_or_create_token(const std::filesystem::path& path) {
    // Exclusive creation: if another engine got there first, read its token
    // rather than overwriting it.
    const auto created = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (created >= 0) {
        auto token = generate();
        if (!token) {
            ::close(created);
            ::unlink(path.c_str());
            return token;
        }
        const auto line = *token + "\n";
        const auto written = ::write(created, line.data(), line.size());
        ::close(created);
        if (written != static_cast<ssize_t>(line.size())) {
            ::unlink(path.c_str());
            return std::unexpected(token_error("could not write the token file", path));
        }
        return token;
    }
    if (errno != EEXIST) {
        return std::unexpected(token_error("could not create the token file", path));
    }

    struct stat status{};
    if (::stat(path.c_str(), &status) != 0) {
        return std::unexpected(token_error("could not inspect the token file", path));
    }
    if ((status.st_mode & (S_IRWXG | S_IRWXO)) != 0U) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "the token file is readable by others; chmod 600 it or delete it to "
                       "have a new one made",
            .context = {{.key = "path", .value = path.string()}}});
    }
    std::ifstream file{path};
    std::string token;
    std::getline(file, token);
    while (!token.empty() && (token.back() == '\r' || token.back() == ' ')) {
        token.pop_back();
    }
    if (token.empty()) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::conflict,
                        .message = "the token file is empty; delete it to have a new one made",
                        .context = {{.key = "path", .value = path.string()}}});
    }
    return token;
}

} // namespace trackknife::engine
