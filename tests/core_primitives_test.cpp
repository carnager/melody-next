// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/atomic_rename.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/error.hpp"
#include "trackknife/core/listen_accounting.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/progress.hpp"
#include "trackknife/core/revision.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/core/unicode.hpp"

#include <fcntl.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <string_view>

namespace {

int failures = 0;

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

void stableIdsRoundTrip() {
    const auto id = trackknife::core::StableId::random();
    CHECK(!id.is_nil());
    const auto text = id.to_string();
    CHECK(text.size() == 36U);
    const auto parsed = trackknife::core::StableId::parse(text);
    CHECK(parsed.has_value());
    CHECK(parsed && *parsed == id);

    const auto known = trackknife::core::StableId::parse("00112233-4455-4677-8899-aabbccddeeff");
    CHECK(known.has_value());
    CHECK(known && known->to_string() == "00112233-4455-4677-8899-aabbccddeeff");
    CHECK(!trackknife::core::StableId::parse("not-an-id").has_value());
    CHECK(!trackknife::core::StableId::parse("00112233-4455-4677-8899-aabbccddeefg").has_value());
}

void cancellationIsSharedAndMonotonic() {
    trackknife::core::CancellationSource source;
    const auto first = source.token();
    const auto second = source.token();
    CHECK(!first.is_cancellation_requested());
    source.request_cancellation();
    CHECK(first.is_cancellation_requested());
    CHECK(second.is_cancellation_requested());
    CHECK(source.is_cancellation_requested());
}

void progressClampsItsFraction() {
    trackknife::core::Progress unknown{5, std::nullopt, "scan", "discovering"};
    CHECK(!unknown.fraction().has_value());
    trackknife::core::Progress halfway{5, 10, "scan", "decoding"};
    CHECK(halfway.fraction().has_value());
    CHECK(std::abs(*halfway.fraction() - 0.5) < 0.000001);
    trackknife::core::Progress overrun{11, 10, "scan", "finalizing"};
    CHECK(overrun.fraction() == 1.0);
}

void errorsRetainStructuredContext() {
    auto error = trackknife::core::Error{trackknife::core::ErrorCode::conflict,
                                         "source revision changed",
                                         {}}
                     .with_context("source_id", "abc")
                     .with_context("operation", "rename");
    CHECK(error.context.size() == 2U);
    CHECK(error.context.front().key == "source_id");
}

void unicodeSimpleCaseComparisonIsDeterministic() {
    const auto accented = trackknife::core::unicodeSimpleCaseEqual("Ärger", "äRGER");
    CHECK(accented && *accented);

    const auto no_full_fold = trackknife::core::unicodeSimpleCaseEqual("Straße", "STRASSE");
    CHECK(no_full_fold && !*no_full_fold);

    const auto no_normalization = trackknife::core::unicodeSimpleCaseEqual("é", "e\u0301");
    CHECK(no_normalization && !*no_normalization);

    const std::string_view invalid{"\xC3", 1};
    const auto rejected = trackknife::core::unicodeSimpleCaseEqual(invalid, invalid);
    CHECK(!rejected.has_value());
    CHECK(rejected.error().code == trackknife::core::ErrorCode::invalid_argument);

    const std::string invalid_after_mismatch{"x\xC3", 2};
    const auto trailing_invalid =
        trackknife::core::unicodeSimpleCaseEqual(invalid_after_mismatch, "y");
    CHECK(!trailing_invalid.has_value());
}

void unicodeCodePointIndexingUsesDecodedCharacters() {
    const std::string text = "AÄ🙂e\u0301";
    const auto count = trackknife::core::unicodeCodePointCount(text);
    CHECK(count && *count == 5U);

    const auto start = trackknife::core::unicodeByteOffset(text, 0U);
    const auto after_ascii = trackknife::core::unicodeByteOffset(text, 1U);
    const auto after_bmp = trackknife::core::unicodeByteOffset(text, 2U);
    const auto after_non_bmp = trackknife::core::unicodeByteOffset(text, 3U);
    const auto past_end = trackknife::core::unicodeByteOffset(text, 99U);
    CHECK(start && *start == 0U);
    CHECK(after_ascii && *after_ascii == 1U);
    CHECK(after_bmp && *after_bmp == 3U);
    CHECK(after_non_bmp && *after_non_bmp == 7U);
    CHECK(past_end && *past_end == text.size());

    const std::string invalid_after_boundary{"A\xC3", 2};
    CHECK(!trackknife::core::unicodeByteOffset(invalid_after_boundary, 1U).has_value());
    CHECK(!trackknife::core::unicodeCodePointCount(invalid_after_boundary).has_value());
}

void unicodeCaseTransformsAndEncodingAreValidated() {
    const auto lowered = trackknife::core::unicodeSimpleLower("ÄBC🙂");
    CHECK(lowered && *lowered == "äbc🙂");
    const auto uppered = trackknife::core::unicodeSimpleUpper("äbc🙂");
    CHECK(uppered && *uppered == "ÄBC🙂");

    const auto lozenge = trackknife::core::unicodeEncodeCodePoint(9674U);
    CHECK(lozenge && *lozenge == "◊");
    const auto non_bmp = trackknife::core::unicodeEncodeCodePoint(0x1F642U);
    CHECK(non_bmp && *non_bmp == "🙂");
    CHECK(!trackknife::core::unicodeEncodeCodePoint(0xD800U).has_value());
    CHECK(!trackknife::core::unicodeSimpleLower(std::string_view{"\xC3", 1}).has_value());
}

void localSourceDiscoveryPreservesRawPathsAndOrder() {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("trackknife-local-sources-" + trackknife::core::StableId::random().to_string());
    std::error_code error;
    CHECK(std::filesystem::create_directories(root / "nested", error));
    const auto first = root / "b.flac";
    const auto second = root / "nested" / "a.opus";
    const std::string invalid_name{"invalid-\xFF.wav", 13U};
    const auto invalid = root / std::filesystem::path{invalid_name};
    std::ofstream{first}.put('\0');
    std::ofstream{second}.put('\0');
    std::ofstream{invalid}.put('\0');

    const std::array inputs{root.native(), first.native()};
    const auto discovered = trackknife::core::discover_local_sources(inputs, {});
    CHECK(!discovered.cancelled);
    CHECK(!discovered.truncated);
    CHECK(discovered.issues.empty());
    CHECK(discovered.raw_files.size() == 4U);
    CHECK(discovered.raw_files.at(0) == first.native());
    CHECK(discovered.raw_files.at(1) == invalid.native());
    CHECK(discovered.raw_files.at(2) == second.native());
    CHECK(discovered.raw_files.back() == first.native());
    CHECK(std::ranges::count(discovered.raw_files, first.native()) == 2);
    CHECK(std::ranges::find(discovered.raw_files, invalid.native()) != discovered.raw_files.end());
    CHECK(trackknife::core::escape_raw_path(invalid_name) == "invalid-\\xFF.wav");
    // For people: valid UTF-8 as written, only what is not text escaped.
    CHECK(trackknife::core::display_raw_path("/music/Kopfh\xC3\xB6rer.mp3") ==
          "/music/Kopfh\xC3\xB6rer.mp3");
    CHECK(trackknife::core::escape_raw_path("/music/Kopfh\xC3\xB6rer.mp3") ==
          "/music/Kopfh\\xC3\\xB6rer.mp3");
    CHECK(trackknife::core::display_raw_path(invalid_name) == "invalid-\\xFF.wav");
    CHECK(trackknife::core::display_raw_path("a\nb") == "a\\x0Ab");
    // A direction override would disguise the rest of the name.
    CHECK(trackknife::core::display_raw_path("x\xE2\x80\xAEy") == "x\\xE2\\x80\\xAEy");
    // Overlong and truncated sequences are not UTF-8.
    CHECK(trackknife::core::display_raw_path("\xC0\xAF") == "\\xC0\\xAF");
    CHECK(trackknife::core::display_raw_path("\xE2\x82") == "\\xE2\\x82");

    trackknife::core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto cancelled = trackknife::core::discover_local_sources(inputs, cancellation.token());
    CHECK(cancelled.cancelled);
    CHECK(cancelled.raw_files.empty());
    const auto limited = trackknife::core::discover_local_sources(inputs, {}, 2U);
    CHECK(limited.truncated);
    CHECK(limited.raw_files.size() == 2U);

    // Extension filtering applies to directory expansion only; explicitly
    // listed files always pass, and matching is ASCII case-insensitive.
    const auto cover = root / "cover.png";
    const auto upper = root / "LOUD.FLAC";
    std::ofstream{cover}.put('\0');
    std::ofstream{upper}.put('\0');
    constexpr std::array<std::string_view, 2> audio_only{"flac", "opus"};
    const std::array filtered_inputs{root.native(), cover.native()};
    const auto filtered =
        trackknife::core::discover_local_sources(filtered_inputs, {}, 100U, std::span{audio_only});
    CHECK(filtered.issues.empty());
    CHECK(std::ranges::count(filtered.raw_files, cover.native()) == 1);
    CHECK(std::ranges::count(filtered.raw_files, upper.native()) == 1);
    CHECK(std::ranges::count(filtered.raw_files, second.native()) == 1);
    CHECK(std::ranges::count(filtered.raw_files, invalid.native()) == 0);
    CHECK(std::ranges::count(filtered.raw_files, first.native()) == 1);

    std::filesystem::remove_all(root, error);
    CHECK(!error);
}

void containedSourceRevalidationFollowsSymlinksAndRejectsEscapes() {
    const auto base =
        std::filesystem::temp_directory_path() /
        ("trackknife-containment-" + trackknife::core::StableId::random().to_string());
    const auto root = base / "root";
    const auto outside = base / "outside";
    std::error_code error;
    CHECK(std::filesystem::create_directories(root / "sub", error));
    CHECK(std::filesystem::create_directories(outside, error));
    const auto inner = root / "sub" / "inner.flac";
    const auto secret = outside / "secret.flac";
    std::ofstream{inner}.put('\0');
    std::ofstream{secret}.put('\0');
    std::filesystem::create_symlink(inner, root / "link-in.flac", error);
    CHECK(!error);
    std::filesystem::create_symlink(secret, root / "escape.flac", error);
    CHECK(!error);

    const auto direct =
        trackknife::core::revalidate_contained_source(root.native(), inner.native());
    CHECK(direct.has_value());
    CHECK(direct && direct->resolved_path == std::filesystem::canonical(inner).native());

    // A symlink whose final target stays inside the root is acceptable and
    // reports the resolved target, never the link.
    const auto linked = trackknife::core::revalidate_contained_source(
        root.native(), (root / "link-in.flac").native());
    CHECK(linked.has_value());
    CHECK(linked && linked->resolved_path == std::filesystem::canonical(inner).native());

    // A symlink escaping the root is rejected even though the referenced path
    // is lexically inside it.
    const auto escaped = trackknife::core::revalidate_contained_source(
        root.native(), (root / "escape.flac").native());
    CHECK(!escaped.has_value());
    CHECK(!escaped && escaped.error().code == trackknife::core::ErrorCode::invalid_argument);

    // Dot-dot traversal resolves outside and is rejected.
    const auto traversal = trackknife::core::revalidate_contained_source(
        root.native(), (root / ".." / "outside" / "secret.flac").native());
    CHECK(!traversal.has_value());

    // A sibling directory sharing the root's byte prefix is not contained.
    const auto sibling = base / "root-evil";
    CHECK(std::filesystem::create_directories(sibling, error));
    const auto sibling_file = sibling / "song.flac";
    std::ofstream{sibling_file}.put('\0');
    const auto prefixed =
        trackknife::core::revalidate_contained_source(root.native(), sibling_file.native());
    CHECK(!prefixed.has_value());

    const auto missing = trackknife::core::revalidate_contained_source(
        root.native(), (root / "missing.flac").native());
    CHECK(!missing.has_value());
    CHECK(!missing && missing.error().code == trackknife::core::ErrorCode::not_found);

    const auto directory =
        trackknife::core::revalidate_contained_source(root.native(), (root / "sub").native());
    CHECK(!directory.has_value());
    CHECK(!directory && directory.error().code == trackknife::core::ErrorCode::unsupported);

    CHECK(!trackknife::core::revalidate_contained_source({}, inner.native()).has_value());
    CHECK(!trackknife::core::revalidate_contained_source(root.native(), {}).has_value());
    const auto missing_root =
        trackknife::core::revalidate_contained_source((base / "no-root").native(), inner.native());
    CHECK(!missing_root.has_value());

    std::filesystem::remove_all(base, error);
    CHECK(!error);
}

} // namespace

void noReplacePublicationLaddersAndRefusesOccupiedTargets() {
    namespace fs = std::filesystem;
    const auto directory =
        fs::temp_directory_path() /
        ("trackknife-no-replace-" + trackknife::core::StableId::random().to_string());
    fs::create_directory(directory);
    const auto write = [](const fs::path& path, const std::string_view content) {
        std::ofstream output{path, std::ios::binary};
        output << content;
    };

    // Publish into an empty slot: the preferred rung succeeds and the
    // temporary is gone.
    write(directory / "temp-a", "alpha");
    auto published = trackknife::core::publish_no_replace_at(
        AT_FDCWD, (directory / "temp-a").native(), AT_FDCWD, (directory / "final-a").native());
    check(published.has_value(), "no-replace publication succeeds", __LINE__);
    // Which rung succeeded is the filesystem's business (tmpfs renames
    // atomically, NFS falls back to the hard link); the outcome is what
    // this pins.
    check(fs::exists(directory / "final-a") && !fs::exists(directory / "temp-a"),
          "publication moved the temporary", __LINE__);

    // An occupied target is a typed conflict and both files stay put.
    write(directory / "temp-b", "bravo");
    auto conflicted = trackknife::core::publish_no_replace_at(
        AT_FDCWD, (directory / "temp-b").native(), AT_FDCWD, (directory / "final-a").native());
    check(!conflicted.has_value(), "occupied targets are refused", __LINE__);
    check(!conflicted && conflicted.error().code == trackknife::core::ErrorCode::conflict,
          "occupied targets report conflict", __LINE__);
    check(fs::exists(directory / "temp-b"), "the temporary survives a conflict", __LINE__);
    {
        std::ifstream input{directory / "final-a"};
        std::string content;
        std::getline(input, content);
        check(content == "alpha", "the occupied target was not replaced", __LINE__);
    }

    // A missing temporary is an error, not a silent success.
    auto missing = trackknife::core::publish_no_replace_at(
        AT_FDCWD, (directory / "gone").native(), AT_FDCWD, (directory / "final-c").native());
    check(!missing.has_value(), "publishing a missing temporary fails", __LINE__);
    fs::remove_all(directory);
}

// The listening rules the engine credits plays with. They were only exercised
// through the window's own player; the rules are core's, so they are tested
// here, where no player is needed.
void listenAccountingCreditsRealListeningOnly() {
    trackknife::core::ListenAccounting accounting;
    std::int64_t now = 0;
    const auto observe = [&](const std::string& identity, const double duration,
                             const double position, const bool playing) {
        const auto credited = accounting.observe(identity, duration, position, playing, now);
        now += 1000;
        return credited;
    };
    int credits = 0;
    for (int second = 0; second <= 10; ++second)
        credits += observe("one", 40, second, true) ? 1 : 0;
    CHECK(credits == 0); // Ten seconds of a forty-second track is not half.
    for (int n = 0; n < 10; ++n)
        credits += observe("one", 40, 10, false) ? 1 : 0;
    credits += observe("one", 40, 10, true) ? 1 : 0;
    credits += observe("one", 40, 35, true) ? 1 : 0; // A seek forward earns nothing,
    credits += observe("one", 40, 0, true) ? 1 : 0;  // nor does one back.
    for (int second = 1; second <= 9; ++second)
        credits += observe("one", 40, second, true) ? 1 : 0;
    CHECK(credits == 0);
    credits += observe("one", 40, 10, true) ? 1 : 0; // Twenty seconds heard: half.
    CHECK(credits == 1);
    for (int second = 11; second <= 40; ++second)
        credits += observe("one", 40, second, true) ? 1 : 0;
    CHECK(credits == 1); // Once per playback, however long it goes on.
    for (int second = 0; second <= 20; ++second)
        credits += observe("two", 40, second, true) ? 1 : 0;
    CHECK(credits == 2); // Another playback of the same file counts again.
    for (int second = 0; second <= 30; ++second)
        credits += observe("three", 30, second, true) ? 1 : 0;
    CHECK(credits == 2); // Clips of thirty seconds or less never count.
}

int main() {
    stableIdsRoundTrip();
    cancellationIsSharedAndMonotonic();
    progressClampsItsFraction();
    errorsRetainStructuredContext();
    unicodeSimpleCaseComparisonIsDeterministic();
    unicodeCodePointIndexingUsesDecodedCharacters();
    unicodeCaseTransformsAndEncodingAreValidated();
    localSourceDiscoveryPreservesRawPathsAndOrder();
    containedSourceRevalidationFollowsSymlinksAndRejectsEscapes();
    noReplacePublicationLaddersAndRefusesOccupiedTargets();
    listenAccountingCreditsRealListeningOnly();
    return failures == 0 ? 0 : 1;
}
