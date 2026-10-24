// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/operations/file_publication_journal.hpp"
#include "trackknife/operations/output_path_preflight.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace {

int failures = 0;

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

class TemporaryDirectory final {
  public:
    explicit TemporaryDirectory(
        const std::filesystem::path& base = std::filesystem::temp_directory_path())
        : path_{base / ("trackknife-output-preflight-" +
                        trackknife::core::StableId::random().to_string())} {
        std::filesystem::create_directories(path_);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, const std::string_view bytes = "audio") {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
}

trackknife::operations::OutputPathPlan
ready_plan(const std::filesystem::path& source, const std::filesystem::path& target,
           const trackknife::core::LocalSourceRevision& revision, const bool move = false,
           const std::filesystem::path& destination = {}) {
    using namespace trackknife::operations;
    return OutputPathPlan{
        .layout = OutputLayoutProfile{.schema_version = 1U,
                                      .name = "Test",
                                      .dialect = {},
                                      .relative_directory_expression = {},
                                      .basename_expression = "%title%",
                                      .sanitization_policy = {"linux", 1U}},
        .destination = move ? std::optional{DestinationProfile{
                                  .schema_version = 1U,
                                  .name = "Destination",
                                  .root_raw_path = destination.native(),
                                  .containment_policy = {"lexical-beneath-root", 1U}}}
                            : std::nullopt,
        .operations = {.rename_files = true, .move_files = move},
        .sources = {PlannedOutputPathSource{
            .source_raw_path = source.native(),
            .source_revision = revision,
            .target_raw_path = target.native(),
            .raw_relative_directory = {},
            .sanitized_relative_directory = {},
            .raw_basename = target.stem().native(),
            .sanitized_basename = target.stem().native(),
            .item_indexes = {0U},
            .sanitized = false,
            .no_change = source == target,
        }},
        .issues = {},
    };
}

bool has_issue(const trackknife::operations::OutputPathPreflight& preflight,
               const trackknife::operations::OutputPathPreflightIssueKind kind) {
    return std::ranges::any_of(preflight.issues,
                               [kind](const auto& issue) { return issue.kind == kind; });
}

void classifiesSameFilesystemAndMissingDirectoriesWithoutMutation() {
    using namespace trackknife;
    TemporaryDirectory temporary;
    const auto source = temporary.path() / "source.flac";
    const auto target = temporary.path() / "Artist" / "Album" / "renamed.flac";
    write_file(source);
    const auto revision = core::observe_local_source_revision(source.native());
    CHECK(revision.has_value());
    if (!revision) {
        return;
    }
    const auto destination_with_trailing_separator =
        std::filesystem::path{temporary.path().native() + "/"};
    const auto checked = operations::preflight_output_paths(
        ready_plan(source, target, *revision, true, destination_with_trailing_separator));
    CHECK(checked.has_value());
    CHECK(checked && checked->ready());
    CHECK(checked && checked->sources.size() == 1U);
    CHECK(checked && checked->sources[0].publication ==
                         operations::OutputPathPublicationKind::same_filesystem_rename);
    CHECK(checked &&
          checked->sources[0].missing_directory_raw_paths ==
              (std::vector<std::string>{(temporary.path() / "Artist").native(),
                                        (temporary.path() / "Artist" / "Album").native()}));
    CHECK(!std::filesystem::exists(target.parent_path()));
    CHECK(std::filesystem::exists(source));
    const auto journal_id = core::StableId::random();
    const auto record = operations::make_file_publication_journal_record(*checked, 0U, journal_id);
    CHECK(record.has_value());
    CHECK(record && record->id == journal_id && record->source_raw_path == source.native() &&
          record->target_raw_path == target.native() && record->prepared_raw_path.empty() &&
          record->expected_source_revision == *revision &&
          record->planned_missing_directory_raw_paths ==
              checked->sources[0].missing_directory_raw_paths);
}

void changedHardLinkedOccupiedAndSymlinkedPathsFailClosed() {
    using namespace trackknife;
    TemporaryDirectory temporary;
    const auto source = temporary.path() / "source.flac";
    const auto target = temporary.path() / "target.flac";
    write_file(source);
    auto revision = core::observe_local_source_revision(source.native());
    CHECK(revision.has_value());
    if (!revision) {
        return;
    }
    auto changed_plan = ready_plan(source, target, *revision);
    write_file(source, "changed bytes");
    auto changed = operations::preflight_output_paths(changed_plan);
    CHECK(changed.has_value());
    CHECK(changed && !changed->ready());
    CHECK(changed && has_issue(*changed, operations::OutputPathPreflightIssueKind::source_changed));

    revision = core::observe_local_source_revision(source.native());
    const auto alias = temporary.path() / "alias.flac";
    CHECK(::link(source.c_str(), alias.c_str()) == 0);
    auto linked = operations::preflight_output_paths(ready_plan(source, target, *revision));
    CHECK(linked.has_value());
    CHECK(linked && !linked->ready());
    CHECK(linked &&
          has_issue(*linked, operations::OutputPathPreflightIssueKind::source_hard_linked));
    std::filesystem::remove(alias);

    write_file(target, "occupied");
    revision = core::observe_local_source_revision(source.native());
    auto occupied = operations::preflight_output_paths(ready_plan(source, target, *revision));
    CHECK(occupied.has_value());
    CHECK(occupied && !occupied->ready());
    CHECK(occupied &&
          has_issue(*occupied, operations::OutputPathPreflightIssueKind::target_exists));
    std::filesystem::remove(target);

    const auto real_parent = temporary.path() / "real";
    const auto linked_parent = temporary.path() / "linked";
    std::filesystem::create_directory(real_parent);
    std::filesystem::create_directory_symlink(real_parent, linked_parent);
    auto symlinked = operations::preflight_output_paths(
        ready_plan(source, linked_parent / "target.flac", *revision, true, temporary.path()));
    CHECK(symlinked.has_value());
    CHECK(symlinked && !symlinked->ready());
    CHECK(symlinked &&
          has_issue(*symlinked, operations::OutputPathPreflightIssueKind::target_parent_symlink));
}

void noChangeIsReadyAndCancellationIsTyped() {
    using namespace trackknife;
    TemporaryDirectory temporary;
    const auto source = temporary.path() / "same.flac";
    write_file(source);
    const auto revision = core::observe_local_source_revision(source.native());
    CHECK(revision.has_value());
    if (!revision) {
        return;
    }
    const auto unchanged =
        operations::preflight_output_paths(ready_plan(source, source, *revision));
    CHECK(unchanged.has_value());
    CHECK(unchanged && unchanged->ready());
    CHECK(unchanged &&
          unchanged->sources[0].publication == operations::OutputPathPublicationKind::no_change);
    const auto no_journal =
        operations::make_file_publication_journal_record(*unchanged, 0U, core::StableId::random());
    CHECK(!no_journal);
    CHECK(no_journal.error().code == core::ErrorCode::invalid_argument);

    core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto cancelled = operations::preflight_output_paths(ready_plan(source, source, *revision),
                                                              cancellation.token());
    CHECK(!cancelled);
    CHECK(cancelled.error().code == core::ErrorCode::cancelled);
}

void classifiesARealCrossFilesystemTargetWhenAvailable() {
    using namespace trackknife;
    const std::filesystem::path shared_memory{"/dev/shm"};
    std::error_code error;
    if (!std::filesystem::is_directory(shared_memory, error) || error) {
        return;
    }
    TemporaryDirectory source_directory;
    TemporaryDirectory destination_directory{shared_memory};
    const auto source = source_directory.path() / "source.flac";
    const auto target = destination_directory.path() / "Album" / "moved.flac";
    write_file(source);
    const auto revision = core::observe_local_source_revision(source.native());
    CHECK(revision.has_value());
    if (!revision) {
        return;
    }
    struct stat destination_status{};
    CHECK(::stat(destination_directory.path().c_str(), &destination_status) == 0);
    if (static_cast<std::uint64_t>(destination_status.st_dev) == revision->device) {
        return;
    }
    const auto checked = operations::preflight_output_paths(
        ready_plan(source, target, *revision, true, destination_directory.path()));
    CHECK(checked.has_value());
    CHECK(checked && checked->ready());
    CHECK(checked && checked->sources[0].publication ==
                         operations::OutputPathPublicationKind::cross_filesystem_copy);
    CHECK(checked && checked->sources[0].target_filesystem_device ==
                         static_cast<std::uint64_t>(destination_status.st_dev));
    CHECK(!std::filesystem::exists(target.parent_path()));
}

} // namespace

// ADR-0318: an album leaving its folder whole takes its images, CUE sheets
// and playlists along; one leaving part of its audio behind takes nothing;
// a CUE sheet naming a renamed file blocks; two covers for one target block.
void companionsTravelWithTheirAlbum() {
    using namespace trackknife;
    using namespace trackknife::operations;
    TemporaryDirectory temporary;
    const auto album = temporary.path() / "In" / "Album";
    const auto destination = temporary.path() / "Out";
    std::filesystem::create_directories(album);
    std::filesystem::create_directories(destination);
    for (const auto* name : {"01.flac", "02.flac"}) {
        write_file(album / name);
    }
    write_file(album / "cover.jpg", "jpeg");
    write_file(album / "album.cue", "FILE \"01.flac\" WAVE\n  TRACK 01 AUDIO\n");
    write_file(album / "album.m3u", "01.flac\n02.flac\n");
    write_file(album / "notes.txt", "text");
    write_file(album / ".hidden.jpg", "jpeg");
    const auto target = destination / "Artist" / "Album";
    const auto plan_of = [&](const std::vector<std::pair<std::string, std::string>>& moves) {
        auto plan = ready_plan(album / moves.front().first, target / moves.front().second,
                               *core::observe_local_source_revision(
                                   (album / moves.front().first).native()),
                               true, destination);
        for (std::size_t index = 1U; index < moves.size(); ++index) {
            auto next = plan.sources.front();
            next.source_raw_path = (album / moves[index].first).native();
            next.target_raw_path = (target / moves[index].second).native();
            next.source_revision =
                *core::observe_local_source_revision(next.source_raw_path);
            next.item_indexes = {index};
            plan.sources.push_back(next);
        }
        return plan;
    };
    const auto companions_of = [](const OutputPathPreflight& preflight) {
        std::vector<std::string> names;
        for (const auto& source : preflight.sources) {
            if (source.planned.companion) {
                names.push_back(std::filesystem::path{source.planned.target_raw_path}
                                    .filename()
                                    .string());
            }
        }
        std::ranges::sort(names);
        return names;
    };

    // Whole, names kept: cover, sheet and playlist along, into the target.
    auto whole = preflight_output_paths_with_companions(
        plan_of({{"01.flac", "01.flac"}, {"02.flac", "02.flac"}}));
    CHECK(whole.has_value());
    CHECK(whole->ready());
    CHECK((companions_of(*whole) ==
           std::vector<std::string>{"album.cue", "album.m3u", "cover.jpg"}));
    CHECK(std::ranges::all_of(whole->sources, [&](const auto& source) {
        return std::filesystem::path{source.planned.target_raw_path}.parent_path() == target;
    }));
    CHECK(std::ranges::all_of(whole->sources, [](const auto& source) {
        return !source.planned.companion || source.planned.item_indexes.empty();
    }));

    // Part of it: nothing along.
    auto part = preflight_output_paths_with_companions(plan_of({{"01.flac", "01.flac"}}));
    CHECK(part.has_value() && part->ready());
    CHECK(companions_of(*part).empty());

    // The sheet's file renamed: blocked, saying why.
    auto renamed = preflight_output_paths_with_companions(
        plan_of({{"01.flac", "01 - One.flac"}, {"02.flac", "02.flac"}}));
    CHECK(renamed.has_value());
    CHECK(!renamed->ready());
    CHECK(has_issue(*renamed, OutputPathPreflightIssueKind::cue_reference_broken));

    // A cover already at the target: the preflight's own check.
    std::filesystem::create_directories(target);
    write_file(target / "cover.jpg", "other");
    auto occupied = preflight_output_paths_with_companions(
        plan_of({{"01.flac", "01.flac"}, {"02.flac", "02.flac"}}));
    CHECK(occupied.has_value());
    CHECK(!occupied->ready());
    CHECK(has_issue(*occupied, OutputPathPreflightIssueKind::target_exists));
    std::filesystem::remove(target / "cover.jpg");

    // Nothing of a plan that moves nothing out of its folder.
    auto staying = preflight_output_paths_with_companions(
        ready_plan(album / "01.flac", album / "01 - One.flac",
                   *core::observe_local_source_revision((album / "01.flac").native())));
    CHECK(staying.has_value());
    CHECK(companions_of(*staying).empty());
}

int main() {
    companionsTravelWithTheirAlbum();
    classifiesSameFilesystemAndMissingDirectoriesWithoutMutation();
    changedHardLinkedOccupiedAndSymlinkedPathsFailClosed();
    noChangeIsReadyAndCancellationIsTyped();
    classifiesARealCrossFilesystemTargetWhenAvailable();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
