// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/stable_id.hpp"
#include "trackknife/persistence/musicbrainz_cache.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

struct TemporaryDirectory {
    TemporaryDirectory() {
        path = std::filesystem::temp_directory_path() /
               ("trackknife-musicbrainz-cache-" + trackknife::core::StableId::random().to_string());
        std::error_code error;
        if (!std::filesystem::create_directory(path, error) || error) {
            path.clear();
        }
    }
    ~TemporaryDirectory() {
        if (!path.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    }
    std::filesystem::path path;
};

[[nodiscard]] std::string body_text(const std::vector<std::byte>& body) {
    std::string text;
    text.reserve(body.size());
    for (const auto value : body) {
        text.push_back(static_cast<char>(value));
    }
    return text;
}

void storesLoadsExpiresAndBounds() {
    const TemporaryDirectory directory;
    CHECK(!directory.path.empty());
    auto cache = trackknife::persistence::SqliteMusicBrainzResponseCache::open(directory.path /
                                                                               "lists.sqlite");
    CHECK(cache.has_value());
    if (!cache) {
        return;
    }
    constexpr std::int64_t ttl = 100;

    CHECK(cache->store("https://musicbrainz.org/a", "alpha", 1'000, ttl, 8U).has_value());
    const auto hit = cache->load("https://musicbrainz.org/a", 1'050, ttl);
    CHECK(hit.has_value() && hit->has_value() && body_text(**hit) == "alpha");

    // Replacement updates the body in place.
    CHECK(cache->store("https://musicbrainz.org/a", "beta", 1'060, ttl, 8U).has_value());
    const auto replaced = cache->load("https://musicbrainz.org/a", 1'070, ttl);
    CHECK(replaced.has_value() && replaced->has_value() && body_text(**replaced) == "beta");

    // Expired rows do not load and are pruned by the next store.
    const auto expired = cache->load("https://musicbrainz.org/a", 1'060 + ttl + 1, ttl);
    CHECK(expired.has_value() && !expired->has_value());

    // The entry bound evicts oldest-first.
    for (int index = 0; index < 5; ++index) {
        const auto url = "https://musicbrainz.org/bulk-" + std::to_string(index);
        CHECK(cache->store(url, "body", 2'000 + index, ttl, 3U).has_value());
    }
    const auto oldest = cache->load("https://musicbrainz.org/bulk-0", 2'010, ttl);
    CHECK(oldest.has_value() && !oldest->has_value());
    const auto newest = cache->load("https://musicbrainz.org/bulk-4", 2'010, ttl);
    CHECK(newest.has_value() && newest->has_value());

    // Out-of-bounds stores fail typed.
    CHECK(!cache->store("", "body", 1'000, ttl, 8U).has_value());
    CHECK(!cache->store("https://musicbrainz.org/x", "body", 1'000, ttl, 0U).has_value());
}

} // namespace

int main() {
    storesLoadsExpiresAndBounds();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
