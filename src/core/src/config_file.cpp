// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/config_file.hpp"

#include "trackknife/core/posix.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>
#include <utility>

namespace trackknife::core {
namespace {

[[nodiscard]] bool blank(const char c) noexcept { return c == ' ' || c == '\t'; }

[[nodiscard]] std::string_view trimmed(std::string_view text) noexcept {
    while (!text.empty() && blank(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && blank(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] Error refused(const std::string& origin, const std::size_t line,
                            const std::string& what) {
    return Error{.code = ErrorCode::invalid_argument,
                 .message = origin + ":" + std::to_string(line) + ": " + what,
                 .context = {{"file", origin}, {"line", std::to_string(line)}}};
}

[[nodiscard]] Error io_error(const std::filesystem::path& path, const std::string& what,
                             const int error) {
    return Error{.code = ErrorCode::io,
                 .message = what + " " + path.string() + ": " + std::strerror(error),
                 .context = {{"path", path.string()}}};
}

[[nodiscard]] std::string section_title(const std::string_view kind,
                                        const std::string_view name) {
    std::string title{kind};
    if (!name.empty()) {
        title += ' ';
        title += name;
    }
    return title;
}

} // namespace

bool ConfigFile::valid_key(const std::string_view key) noexcept {
    if (key.empty() || key.front() == '-' || key.back() == '-') {
        return false;
    }
    for (const char c : key) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

Result<ConfigFile> ConfigFile::parse(const std::string_view text, std::string origin) {
    ConfigFile file;
    file.origin_ = std::move(origin);
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        auto line = text.substr(start, end == std::string_view::npos ? text.size() - start
                                                                     : end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        file.lines_.emplace_back(line);
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    if (auto parsed = file.reparse(); !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    return file;
}

Result<ConfigFile> ConfigFile::read(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        const int error = errno;
        if (error == ENOENT) {
            return std::unexpected(Error{.code = ErrorCode::not_found,
                                         .message = "no " + path.string(),
                                         .context = {{"path", path.string()}}});
        }
        return std::unexpected(io_error(path, "cannot read", error));
    }
    const std::string text{std::istreambuf_iterator<char>{stream},
                           std::istreambuf_iterator<char>{}};
    if (stream.bad()) {
        return std::unexpected(io_error(path, "cannot read", errno));
    }
    return parse(text, path.string());
}

Result<void> ConfigFile::reparse() {
    top_.clear();
    sections_.clear();
    // Which section the lines belong to: none yet, the top level. An index,
    // not a pointer, for sections_ grows as headers are read.
    std::optional<std::size_t> current;
    const auto entries = [&]() -> std::vector<Entry>& {
        return current ? sections_[*current].entries : top_;
    };
    for (std::size_t index = 0; index < lines_.size(); ++index) {
        const auto number = index + 1;
        const auto line = trimmed(lines_[index]);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (line.front() == '[') {
            if (line.back() != ']') {
                return std::unexpected(refused(origin_, number, "a section header ends with ]"));
            }
            const auto inside = trimmed(line.substr(1, line.size() - 2));
            const auto split = inside.find_first_of(" \t");
            const auto kind = inside.substr(0, split);
            const auto name =
                split == std::string_view::npos ? std::string_view{} : trimmed(inside.substr(split));
            if (!valid_key(kind)) {
                return std::unexpected(refused(origin_, number,
                                               "[" + std::string{inside} +
                                                   "] is not a section: it starts with a kind "
                                                   "such as [engine NAME]"));
            }
            for (const auto& earlier : sections_) {
                if (earlier.kind == kind && earlier.name == name) {
                    return std::unexpected(refused(
                        origin_, number,
                        "[" + section_title(kind, name) + "] is given twice (first on line " +
                            std::to_string(earlier.line) + ")"));
                }
            }
            sections_.push_back(Section{.kind = std::string{kind},
                                        .name = std::string{name},
                                        .line = number,
                                        .entries = {}});
            current = sections_.size() - 1;
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) {
            return std::unexpected(refused(origin_, number,
                                           "not a setting: settings read key = value, "
                                           "comments start with #"));
        }
        const auto key = trimmed(line.substr(0, equals));
        const auto value = trimmed(line.substr(equals + 1));
        if (!valid_key(key)) {
            return std::unexpected(refused(origin_, number,
                                           "\"" + std::string{key} +
                                               "\" is not a key: keys are lowercase letters, "
                                               "digits and dashes"));
        }
        if (value.empty()) {
            return std::unexpected(refused(origin_, number,
                                           std::string{key} +
                                               " has no value; leave it out, or comment it out "
                                               "with #"));
        }
        for (const auto& earlier : entries()) {
            if (earlier.key == key) {
                return std::unexpected(refused(origin_, number,
                                               std::string{key} +
                                                   " is given twice (first on line " +
                                                   std::to_string(earlier.line) + ")"));
            }
        }
        entries().push_back(
            Entry{.key = std::string{key}, .value = std::string{value}, .line = number});
    }
    return {};
}

const ConfigFile::Section* ConfigFile::section(const std::string_view kind,
                                               const std::string_view name) const {
    for (const auto& candidate : sections_) {
        if (candidate.kind == kind && candidate.name == name) {
            return &candidate;
        }
    }
    return nullptr;
}

const ConfigFile::Entry* ConfigFile::find(const std::string_view kind, const std::string_view name,
                                          const std::string_view key) const {
    const std::vector<Entry>* entries = &top_;
    if (!kind.empty()) {
        const auto* found = section(kind, name);
        if (found == nullptr) {
            return nullptr;
        }
        entries = &found->entries;
    }
    for (const auto& entry : *entries) {
        if (entry.key == key) {
            return &entry;
        }
    }
    return nullptr;
}

Result<void> ConfigFile::set(const std::string_view kind, const std::string_view name,
                             const std::string_view key, const std::string_view value) {
    if (!valid_key(key) || (!kind.empty() && !valid_key(kind))) {
        return std::unexpected(Error{.code = ErrorCode::invalid_argument,
                                     .message = "\"" + std::string{key} +
                                                "\" is not a key: keys are lowercase letters, "
                                                "digits and dashes",
                                     .context = {}});
    }
    if (value.empty() || trimmed(value) != value || value.find_first_of("\r\n") != std::string_view::npos) {
        return std::unexpected(Error{
            .code = ErrorCode::invalid_argument,
            .message = std::string{key} +
                       ": a value is one line, not empty, without spaces at either end",
            .context = {}});
    }
    if (name.find_first_of("]\r\n") != std::string_view::npos || trimmed(name) != name) {
        return std::unexpected(Error{.code = ErrorCode::invalid_argument,
                                     .message = "\"" + std::string{name} +
                                                "\" cannot name a section",
                                     .context = {}});
    }
    const auto line = std::string{key} + " = " + std::string{value};
    if (const auto* existing = find(kind, name, key)) {
        lines_[existing->line - 1] = line;
        return reparse();
    }
    if (kind.empty()) {
        std::size_t at = 0;
        if (!top_.empty()) {
            at = top_.back().line;
        } else {
            // Before the first section and the comments that head it.
            at = sections_.empty() ? lines_.size() : sections_.front().line - 1;
            while (at > 0 && trimmed(lines_[at - 1]).starts_with('#')) {
                --at;
            }
            while (at > 0 && trimmed(lines_[at - 1]).empty()) {
                --at;
            }
        }
        lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(at), line);
        if (top_.empty() && at + 1 < lines_.size() && !trimmed(lines_[at + 1]).empty()) {
            lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(at + 1), std::string{});
        }
        return reparse();
    }
    if (const auto* existing = section(kind, name)) {
        const auto at = existing->entries.empty() ? existing->line : existing->entries.back().line;
        lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(at), line);
        return reparse();
    }
    if (!lines_.empty() && !trimmed(lines_.back()).empty()) {
        lines_.emplace_back();
    }
    lines_.push_back("[" + section_title(kind, name) + "]");
    lines_.push_back(line);
    return reparse();
}

bool ConfigFile::unset(const std::string_view kind, const std::string_view name,
                       const std::string_view key) {
    const auto* existing = find(kind, name, key);
    if (existing == nullptr) {
        return false;
    }
    lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(existing->line - 1));
    // Only a line went; what is left parsed before and still does.
    static_cast<void>(reparse());
    return true;
}

std::string ConfigFile::text() const {
    std::string text;
    for (const auto& line : lines_) {
        text += line;
        text += '\n';
    }
    return text;
}

Result<void> write_config_file(const std::filesystem::path& path, const std::string_view text) {
    std::error_code error;
    auto target = path;
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(path, error))) {
        target = std::filesystem::weakly_canonical(path, error);
        if (error) {
            return std::unexpected(io_error(path, "cannot follow", error.value()));
        }
    }
    const auto folder = target.parent_path().empty() ? std::filesystem::path{"."}
                                                     : target.parent_path();
    if (!std::filesystem::exists(folder, error)) {
        std::filesystem::create_directories(folder, error);
        if (error) {
            return std::unexpected(io_error(folder, "cannot make", error.value()));
        }
        std::filesystem::permissions(folder, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, error);
    }
    mode_t mode = S_IRUSR | S_IWUSR;
    if (struct stat existing{}; ::stat(target.c_str(), &existing) == 0) {
        mode = existing.st_mode & 07777;
    }
    auto prepared = (folder / ("." + target.filename().string() + ".XXXXXX")).string();
    const int fd = ::mkstemp(prepared.data());
    if (fd < 0) {
        return std::unexpected(io_error(folder, "cannot write in", errno));
    }
    const auto fail = [&](const std::string& what) {
        const int failure = errno;
        ::close(fd);
        ::unlink(prepared.c_str());
        return std::unexpected(io_error(target, what, failure));
    };
    std::string_view rest = text;
    while (!rest.empty()) {
        const auto written = ::write(fd, rest.data(), rest.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail("cannot write");
        }
        rest.remove_prefix(static_cast<std::size_t>(written));
    }
    if (::fchmod(fd, mode) != 0) {
        return fail("cannot set the permissions of");
    }
    if (::fsync(fd) != 0) {
        return fail("cannot write");
    }
    if (::close(fd) != 0) {
        const int failure = errno;
        ::unlink(prepared.c_str());
        return std::unexpected(io_error(target, "cannot write", failure));
    }
    if (::rename(prepared.c_str(), target.c_str()) != 0) {
        const int failure = errno;
        ::unlink(prepared.c_str());
        return std::unexpected(io_error(target, "cannot replace", failure));
    }
    if (const int directory = ::open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        directory >= 0) {
        static_cast<void>(::fsync(directory));
        ::close(directory);
    }
    return {};
}

} // namespace trackknife::core
