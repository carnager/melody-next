// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/library_views.hpp"

#include "track_format.hpp"
#include "trackknife/core/unicode.hpp"
#include "trackknife/persistence/tkq_row.hpp"
#include "trackknife/query/tkq.hpp"
#include "trackknife/titleformat/compiler.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace trackknife::engine {

using persistence::LibraryEntry;
using persistence::LibraryEntryKind;
using persistence::LibraryPage;
using persistence::LibraryQuery;
using persistence::LibraryViewLevel;

namespace {

[[nodiscard]] core::Error invalid(std::string message) {
    return core::Error{
        .code = core::ErrorCode::invalid_argument, .message = std::move(message), .context = {}};
}

[[nodiscard]] std::string folded(const std::string& text) {
    auto lowered = core::unicodeSimpleLower(text);
    return lowered ? std::move(*lowered) : text;
}

[[nodiscard]] bool digit(const char c) { return c >= '0' && c <= '9'; }

// Ordered as a reader expects: digit runs by their value, so "Disc 2" comes
// before "Disc 10" and "1990s" before "2000s"; the rest bytewise, already
// case-folded.
[[nodiscard]] int natural_compare(const std::string_view left, const std::string_view right) {
    std::size_t i = 0U;
    std::size_t j = 0U;
    while (i < left.size() && j < right.size()) {
        if (digit(left[i]) && digit(right[j])) {
            auto a = i;
            auto b = j;
            while (a < left.size() && left[a] == '0') {
                ++a;
            }
            while (b < right.size() && right[b] == '0') {
                ++b;
            }
            auto end_a = a;
            auto end_b = b;
            while (end_a < left.size() && digit(left[end_a])) {
                ++end_a;
            }
            while (end_b < right.size() && digit(right[end_b])) {
                ++end_b;
            }
            if (end_a - a != end_b - b) {
                return end_a - a < end_b - b ? -1 : 1;
            }
            if (const auto order = left.substr(a, end_a - a).compare(right.substr(b, end_b - b));
                order != 0) {
                return order < 0 ? -1 : 1;
            }
            i = end_a;
            j = end_b;
            continue;
        }
        if (left[i] != right[j]) {
            return static_cast<unsigned char>(left[i]) < static_cast<unsigned char>(right[j]) ? -1
                                                                                              : 1;
        }
        ++i;
        ++j;
    }
    if (i == left.size() && j == right.size()) {
        return 0;
    }
    return i == left.size() ? -1 : 1;
}

struct Node {
    std::string label;
    // Folded; the first of its tracks' in the level's order.
    std::optional<std::string> sort;
    std::vector<std::uint32_t> tracks;
    std::vector<Node> children;
    std::unordered_map<std::string, std::uint32_t> index;
};

struct Level {
    titleformat::Program format;
    std::optional<titleformat::Program> sort;
    bool descending{false};
};

[[nodiscard]] core::Result<titleformat::Program> compile_level(const std::string& source) {
    auto compiled =
        titleformat::compile(source, {.context = titleformat::FormatContextKind::tree_level,
                                      .dialect = {},
                                      .parse_options = {}});
    if (!compiled.isValid()) {
        return std::unexpected(invalid(!compiled.parse_diagnostics.empty()
                                           ? compiled.parse_diagnostics.front().message
                                           : compiled.diagnostics.front().message));
    }
    return std::move(*compiled.program);
}

[[nodiscard]] bool reads_history(const query::CompiledTkq& compiled) {
    return (compiled.sort && !compiled.sort->history.empty()) ||
           std::ranges::any_of(compiled.predicates, [](const auto& predicate) {
               return predicate.operand == query::TkqOperandKind::history;
           });
}

// Children in their level's order, at every depth; empty labels last.
void order(Node& node, const std::vector<Level>& levels, const std::size_t depth) {
    if (depth >= levels.size()) {
        return;
    }
    const auto descending = levels[depth].descending;
    std::ranges::stable_sort(node.children, [descending](const Node& left, const Node& right) {
        if (left.label.empty() != right.label.empty()) {
            return right.label.empty();
        }
        const auto& a = left.sort ? *left.sort : left.label;
        const auto& b = right.sort ? *right.sort : right.label;
        auto compared = natural_compare(a, b);
        if (compared == 0) {
            compared = natural_compare(folded(left.label), folded(right.label));
        }
        return descending ? compared > 0 : compared < 0;
    });
    node.index.clear();
    for (std::uint32_t position = 0U; position < node.children.size(); ++position) {
        node.index.emplace(node.children[position].label, position);
        order(node.children[position], levels, depth + 1U);
    }
}

} // namespace

struct LibraryViews::Tree {
    std::vector<LibraryViewLevel> levels;
    std::string filter;
    std::string stamp;
    std::vector<LibraryEntry> tracks;
    std::vector<std::string> album_keys;
    Node root;
};

core::Result<std::shared_ptr<const LibraryViews::Tree>>
LibraryViews::tree(const persistence::LocalLibrary& library, const LibraryQuery& request,
                   const core::CancellationToken& cancellation) const {
    std::optional<query::CompiledTkq> filter;
    if (const auto source = request.view_filter; !source.empty()) {
        auto compiled = query::compile_tkq(source);
        if (!compiled) {
            return std::unexpected(std::move(compiled.error()));
        }
        if (!compiled->match_all) {
            filter = std::move(*compiled);
        }
    }
    const bool history = filter && reads_history(*filter);
    auto stamp = library.view_stamp(history);
    if (!stamp) {
        return std::unexpected(std::move(stamp.error()));
    }
    {
        const std::lock_guard guard{mutex_};
        const auto found = std::ranges::find_if(trees_, [&](const auto& tree) {
            return tree->levels == request.view && tree->filter == request.view_filter &&
                   tree->stamp == *stamp;
        });
        if (found != trees_.end()) {
            auto tree = *found;
            trees_.erase(found);
            trees_.push_front(tree);
            return std::shared_ptr<const Tree>{tree};
        }
    }

    std::vector<Level> levels;
    levels.reserve(request.view.size());
    for (const auto& level : request.view) {
        auto format = compile_level(level.format);
        if (!format) {
            return std::unexpected(std::move(format.error()));
        }
        Level compiled{
            .format = std::move(*format), .sort = std::nullopt, .descending = level.descending};
        if (!level.sort.empty()) {
            auto sort = compile_level(level.sort);
            if (!sort) {
                return std::unexpected(std::move(sort.error()));
            }
            compiled.sort = std::move(*sort);
        }
        levels.push_back(std::move(compiled));
    }

    auto built = std::make_shared<Tree>();
    built->levels = request.view;
    built->filter = request.view_filter;
    built->stamp = std::move(*stamp);
    std::optional<core::Error> failure;
    std::vector<std::vector<std::string>> labels(levels.size());
    std::vector<std::vector<std::string>> sorts(levels.size());

    // A track under every combination of its levels' labels; each node holds
    // it once however many of its branches pass through.
    const auto place = [&](const auto& self, Node& node, const std::size_t depth,
                           const std::uint32_t track) -> void {
        if (node.tracks.empty() || node.tracks.back() != track) {
            node.tracks.push_back(track);
        }
        if (depth == levels.size()) {
            return;
        }
        for (std::size_t branch = 0U; branch < labels[depth].size(); ++branch) {
            const auto& label = labels[depth][branch];
            auto found = node.index.find(label);
            if (found == node.index.end()) {
                found = node.index.emplace(label, static_cast<std::uint32_t>(node.children.size()))
                            .first;
                node.children.push_back(
                    Node{.label = label, .sort = {}, .tracks = {}, .children = {}, .index = {}});
            }
            auto& child = node.children[found->second];
            const auto& key = sorts[depth][std::min(branch, sorts[depth].size() - 1U)];
            if (!child.sort) {
                child.sort = key;
            } else if (const auto compared = natural_compare(key, *child.sort);
                       levels[depth].descending ? compared > 0 : compared < 0) {
                child.sort = key;
            }
            self(self, child, depth + 1U, track);
        }
    };

    auto walked = library.each_track(
        [&](persistence::LibraryViewTrack&& track) {
            if (failure) {
                return;
            }
            if (filter && !persistence::tkq_matches(*filter, track.facts, cancellation)) {
                return;
            }
            name_by_file(track.facts, track.entry.key);
            const auto host = track_fields(track.facts, track.entry.key);
            std::size_t branches = 1U;
            for (std::size_t depth = 0U; depth < levels.size(); ++depth) {
                auto values = persistence::tkq_format_each(levels[depth].format, track.facts, host,
                                                           cancellation);
                if (!values) {
                    failure = std::move(values.error());
                    return;
                }
                if (values->empty()) {
                    values->emplace_back();
                }
                branches *= values->size();
                if (branches > maximum_branches) {
                    failure = invalid("A track falls under more than " +
                                      std::to_string(maximum_branches) +
                                      " branches of this view; use $each on fewer levels");
                    return;
                }
                labels[depth] = std::move(*values);
                sorts[depth].clear();
                if (levels[depth].sort) {
                    auto keys = persistence::tkq_format_each(*levels[depth].sort, track.facts, host,
                                                             cancellation);
                    if (!keys) {
                        failure = std::move(keys.error());
                        return;
                    }
                    for (const auto& key : *keys) {
                        sorts[depth].push_back(folded(key));
                    }
                }
                if (sorts[depth].empty()) {
                    for (const auto& label : labels[depth]) {
                        sorts[depth].push_back(folded(label));
                    }
                }
            }
            const auto position = static_cast<std::uint32_t>(built->tracks.size());
            built->tracks.push_back(std::move(track.entry));
            built->album_keys.push_back(std::move(track.album_key));
            place(place, built->root, 0U, position);
        },
        history, cancellation);
    if (!walked) {
        return std::unexpected(std::move(walked.error()));
    }
    if (failure) {
        return std::unexpected(std::move(*failure));
    }
    order(built->root, levels, 0U);

    std::shared_ptr<const Tree> result = built;
    const std::lock_guard guard{mutex_};
    trees_.push_front(result);
    while (trees_.size() > kept) {
        trees_.pop_back();
    }
    return result;
}

namespace {

[[nodiscard]] const Node* find_node(const Node& root, const std::vector<std::string>& path) {
    const auto* node = &root;
    for (const auto& label : path) {
        const auto found = node->index.find(label);
        if (found == node->index.end()) {
            return nullptr;
        }
        node = &node->children[found->second];
    }
    return node;
}

} // namespace

namespace {

// A node as its parent lists it: a group, or at the last level an album
// when it holds one.
[[nodiscard]] LibraryEntry describe_node(const LibraryViews::Tree& grouped, const Node& child,
                                         const bool last_level) {
    LibraryEntry entry;
    entry.kind = LibraryEntryKind::group;
    entry.key = child.label;
    entry.label = child.label;
    entry.view_value = child.label;
    entry.tracks = child.tracks.size();
    entry.duration_ms = 0;
    std::set<std::string_view> albums;
    bool one_artist = true;
    for (const auto index : child.tracks) {
        const auto& track = grouped.tracks[index];
        entry.available += track.available;
        albums.insert(grouped.album_keys[index]);
        if (entry.duration_ms >= 0) {
            entry.duration_ms = track.duration_ms >= 0 ? entry.duration_ms + track.duration_ms : -1;
        }
        if (!track.date.empty() && (entry.date.empty() || track.date < entry.date)) {
            entry.date = track.date;
        }
        entry.added = std::max(entry.added, track.added);
        if (index == child.tracks.front()) {
            entry.artist = track.artist;
            entry.album = track.album;
        } else if (one_artist && track.artist != entry.artist) {
            one_artist = false;
        }
    }
    if (!one_artist) {
        entry.artist.clear();
    }
    entry.albums = albums.size();
    // One album at the last level is that album, as the artist tree has
    // it: its cover, its rating, Go to album.
    if (last_level && albums.size() == 1U && !child.tracks.empty()) {
        const auto& first = grouped.tracks[child.tracks.front()];
        entry.kind = LibraryEntryKind::album;
        entry.key = grouped.album_keys[child.tracks.front()];
        entry.rating_hash = first.album_rating_hash;
        entry.rating = first.album_rating;
        entry.album_rating_hash = first.album_rating_hash;
        entry.album_rating = first.album_rating;
    } else {
        entry.album.clear();
    }
    return entry;
}

} // namespace

core::Result<LibraryPage> LibraryViews::query(const persistence::LocalLibrary& library,
                                              const LibraryQuery& request,
                                              const core::CancellationToken& cancellation) const {
    if (request.view.empty()) {
        return std::unexpected(invalid("A view has at least one level"));
    }
    if (request.view_path.size() > request.view.size()) {
        return std::unexpected(invalid("The path is deeper than the view"));
    }
    auto tree = this->tree(library, request, cancellation);
    if (!tree) {
        return std::unexpected(std::move(tree.error()));
    }
    const auto& grouped = **tree;
    LibraryPage page;
    // A node that went with a change to the library is empty, not an error:
    // the client reloads and finds what is there now.
    const auto* node = find_node(grouped.root, request.view_path);
    if (node == nullptr) {
        return page;
    }
    // ADR-0273: where a file is -- its node at each level below this one,
    // the first it is under, as the levels order them -- for a client to
    // open the way to it. Not there (filtered out, or not in the library):
    // no way, and still an answer.
    if (request.raw_path) {
        page.located = true;
        const auto found = std::ranges::find(grouped.tracks, *request.raw_path, &LibraryEntry::key);
        if (found == grouped.tracks.end()) {
            return page;
        }
        const auto wanted = static_cast<std::uint32_t>(found - grouped.tracks.begin());
        for (auto depth = request.view_path.size(); depth < request.view.size(); ++depth) {
            const auto under = std::ranges::find_if(node->children, [wanted](const Node& child) {
                return std::ranges::contains(child.tracks, wanted);
            });
            if (under == node->children.end()) {
                break;
            }
            page.entries.push_back(
                describe_node(grouped, *under, depth + 1U == request.view.size()));
            node = &*under;
        }
        return page;
    }
    const auto limit = std::max<std::size_t>(request.limit, 1U);
    if (request.view_path.size() == request.view.size()) {
        for (auto position = request.offset; position < node->tracks.size(); ++position) {
            if (page.entries.size() == limit) {
                page.more = true;
                break;
            }
            page.entries.push_back(grouped.tracks[node->tracks[position]]);
        }
        return page;
    }
    const bool last_level = request.view_path.size() + 1U == request.view.size();
    for (auto position = request.offset; position < node->children.size(); ++position) {
        if (page.entries.size() == limit) {
            page.more = true;
            break;
        }
        page.entries.push_back(describe_node(grouped, node->children[position], last_level));
    }
    return page;
}

core::Result<std::vector<std::string>>
LibraryViews::paths(const persistence::LocalLibrary& library, const LibraryQuery& request,
                    const core::CancellationToken& cancellation) const {
    if (request.view.empty() || request.view_path.size() > request.view.size()) {
        return std::unexpected(invalid("The path does not fit the view"));
    }
    auto tree = this->tree(library, request, cancellation);
    if (!tree) {
        return std::unexpected(std::move(tree.error()));
    }
    std::vector<std::string> paths;
    const auto* node = find_node((*tree)->root, request.view_path);
    if (node == nullptr) {
        return paths;
    }
    paths.reserve(node->tracks.size());
    for (const auto index : node->tracks) {
        const auto& track = (*tree)->tracks[index];
        if (track.available > 0U) {
            paths.push_back(track.key);
        }
    }
    return paths;
}

} // namespace trackknife::engine
