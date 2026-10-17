// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::core {

// ADR-0286: the format of melodyd.conf. Lines of `key = value`, comments as
// lines of their own starting with `#`, blank lines, and sections headed
// `[kind name]`. A value is the rest of its line, trimmed, as raw bytes --
// a path need not be UTF-8 -- and is never quoted. What the keys mean is the
// reader's business; this only knows the shape, and refuses what is not it:
// a line that is none of the above, a key given twice in one section, a
// section given twice, a key without a value.
//
// Edits change only the line they are about. Every other line -- comments,
// blank lines, order -- is kept as it was.
class ConfigFile {
  public:
    struct Entry {
        std::string key;
        std::string value;
        std::size_t line{};
    };

    struct Section {
        std::string kind;
        // The rest of the header, trimmed; may hold spaces ("Pixel 10 Pro")
        // and may be empty.
        std::string name;
        std::size_t line{};
        std::vector<Entry> entries;
    };

    // `origin` names the file in errors, which read "<origin>:<line>: ...".
    [[nodiscard]] static Result<ConfigFile> parse(std::string_view text, std::string origin);
    // not_found when there is no file.
    [[nodiscard]] static Result<ConfigFile> read(const std::filesystem::path& path);

    [[nodiscard]] const std::string& origin() const noexcept { return origin_; }
    // The entries before the first section.
    [[nodiscard]] const std::vector<Entry>& top() const noexcept { return top_; }
    [[nodiscard]] const std::vector<Section>& sections() const noexcept { return sections_; }
    // A top-level entry, or one of a section (kind and name; an empty kind is
    // the top level). Null when it is not there.
    [[nodiscard]] const Entry* find(std::string_view kind, std::string_view name,
                                    std::string_view key) const;
    [[nodiscard]] const Section* section(std::string_view kind, std::string_view name) const;

    // Sets a key: its line is rewritten where it is; else a line is added
    // below the key's commented example ("#key = ...") in its section, as a
    // skeleton has them; else after the section's last entry -- for the top
    // level, before the comments that head the first section, or at the end
    // when there is none; else the section is added at the end. Refuses a
    // key or value this format cannot hold.
    [[nodiscard]] Result<void> set(std::string_view kind, std::string_view name,
                                   std::string_view key, std::string_view value);
    // Removes a key's line; false when it was not there.
    bool unset(std::string_view kind, std::string_view name, std::string_view key);

    // The file as text, every line ending in a newline.
    [[nodiscard]] std::string text() const;

    // Whether `key` may name a key or a section's kind: lowercase ASCII
    // letters, digits and inner dashes.
    [[nodiscard]] static bool valid_key(std::string_view key) noexcept;

  private:
    [[nodiscard]] Result<void> reparse();

    std::string origin_;
    std::vector<std::string> lines_;
    std::vector<Entry> top_;
    std::vector<Section> sections_;
};

// Writes a config file in one step: a prepared copy beside it, renamed into
// place. Through a symlink, its target is written and the link kept (as a
// dotfile manager makes them). An existing file keeps its permissions; a new
// one, and the folder made for it, are its owner's only, for it may hold a
// password.
[[nodiscard]] Result<void> write_config_file(const std::filesystem::path& path,
                                             std::string_view text);

} // namespace trackknife::core
