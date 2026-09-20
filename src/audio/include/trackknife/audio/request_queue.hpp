// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace trackknife::audio {
// Occurrence-based FIFO shared by local progression and request presentation.
// Sources are deliberately opaque: local paths and server URIs never mix.
template <class Source> class RequestQueue {
  public:
    struct Entry {
        std::uint64_t id;
        Source source;
    };
    static constexpr std::size_t limit = 500;
    [[nodiscard]] std::uint64_t revision() const { return revision_; }
    [[nodiscard]] const std::vector<Entry>& pending() const { return pending_; }
    [[nodiscard]] const std::optional<Entry>& active() const { return active_; }
    bool insert(std::vector<Source> sources, std::size_t position) {
        if (sources.size() + pending_.size() > limit)
            return false;
        ++revision_;
        undo_ = pending_;
        position = std::min(position, pending_.size());
        std::vector<Entry> entries;
        for (auto& source : sources)
            entries.push_back({++serial_, std::move(source)});
        pending_.insert(pending_.begin() + static_cast<std::ptrdiff_t>(position),
                        std::make_move_iterator(entries.begin()),
                        std::make_move_iterator(entries.end()));
        return true;
    }
    bool remove(std::uint64_t id) {
        ++revision_;
        undo_ = pending_;
        return std::erase_if(pending_, [id](const Entry& item) { return item.id == id; }) != 0;
    }
    bool move(std::uint64_t id, std::size_t position) {
        const auto it = std::find_if(pending_.begin(), pending_.end(),
                                     [id](const Entry& item) { return item.id == id; });
        if (it == pending_.end() || position >= pending_.size())
            return false;
        ++revision_;
        undo_ = pending_;
        Entry item = std::move(*it);
        pending_.erase(it);
        pending_.insert(pending_.begin() + static_cast<std::ptrdiff_t>(position), std::move(item));
        return true;
    }
    // Retain/reorder exact occurrences as one revision and one undo step.
    bool retain(const std::vector<std::uint64_t>& ids) {
        std::vector<Entry> next;
        for (const auto id : ids) {
            const auto it = std::find_if(pending_.begin(), pending_.end(),
                                         [id](const Entry& entry) { return entry.id == id; });
            if (it == pending_.end() ||
                std::any_of(next.begin(), next.end(),
                            [id](const Entry& entry) { return entry.id == id; }))
                return false;
            next.push_back(*it);
        }
        ++revision_;
        undo_ = pending_;
        pending_ = std::move(next);
        return true;
    }
    // A committed preload may finish after its pending entry was removed. Retain
    // that exact occurrence as active rather than consuming a different request.
    void started(Entry entry) {
        remove(entry.id);
        ++revision_;
        active_ = std::move(entry);
        undo_.reset();
    }
    void finished() {
        if (active_)
            ++revision_;
        active_.reset();
        undo_.reset();
    }
    void clear() {
        ++revision_;
        undo_ = pending_;
        pending_.clear();
    }
    [[nodiscard]] bool canUndo() const { return undo_.has_value(); }
    void forgetUndo() { undo_.reset(); }
    bool undo() {
        if (!undo_)
            return false;
        ++revision_;
        pending_ = std::move(*undo_);
        undo_.reset();
        return true;
    }
    void abandon() {
        clear();
        finished();
    }
    template <class Function> void updateSources(Function function) {
        ++revision_;
        for (auto& entry : pending_)
            function(entry.source);
        if (active_)
            function(active_->source);
        if (undo_)
            for (auto& entry : *undo_)
                function(entry.source);
    }

  private:
    std::uint64_t serial_{0};
    std::uint64_t revision_{1};
    std::vector<Entry> pending_;
    std::optional<Entry> active_;
    std::optional<std::vector<Entry>> undo_;
};
} // namespace trackknife::audio
