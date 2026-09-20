// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/artwork_write_plan.hpp"

#include <algorithm>
#include <climits>
#include <ranges>
#include <string>
#include <unordered_map>
#include <utility>

namespace trackknife::metadata {
namespace {

constexpr std::size_t maximum_logical_intents = 100'000U;
constexpr std::uint64_t maximum_replacement_bytes = 16U * 1024U * 1024U;
constexpr std::size_t maximum_added_description_bytes = 4'096U;

struct PhysicalKey {
    std::uint64_t device{0U};
    std::uint64_t inode{0U};

    friend bool operator==(const PhysicalKey&, const PhysicalKey&) = default;
};

struct PhysicalKeyHash {
    [[nodiscard]] std::size_t operator()(const PhysicalKey& key) const noexcept {
        const auto first = std::hash<std::uint64_t>{}(key.device);
        const auto second = std::hash<std::uint64_t>{}(key.inode);
        return first ^ (second + 0x9E3779B9U + (first << 6U) + (first >> 2U));
    }
};

[[nodiscard]] core::Error plan_error(const core::ErrorCode code, std::string message,
                                     const std::string& raw_path = {}) {
    core::Error result{.code = code, .message = std::move(message), .context = {}};
    if (!raw_path.empty()) {
        result.context.push_back({.key = "path", .value = core::escape_raw_path(raw_path)});
    }
    return result;
}

[[nodiscard]] core::Result<ArtworkWritePlan> cancelled() {
    return std::unexpected(core::Error{
        .code = core::ErrorCode::cancelled,
        .message = "artwork write-plan revalidation was cancelled",
        .context = {},
    });
}

void add_issue(ArtworkWritePlanSource& source, const ArtworkWritePlanIssueKind kind,
               core::Error error) {
    source.issues.push_back(ArtworkWritePlanIssue{
        .kind = kind,
        .error = std::move(error),
        .occurrence_indexes = source.occurrence_indexes,
        .blocking = true,
    });
}

[[nodiscard]] bool same_physical_intent(const ArtworkWritePlanSource& source,
                                        const ArtworkWritePlanIntent& intent) {
    const auto& change = source.change;
    const auto source_replacement = change.replacement
                                        ? std::optional<std::string>{change.replacement->raw_path}
                                        : std::nullopt;
    return change.kind == intent.kind && change.target_ordinal == intent.target_ordinal &&
           change.expected_target_fingerprint == intent.expected_target_fingerprint &&
           source_replacement == intent.replacement_raw_path &&
           (intent.kind != ArtworkWritePlanIntentKind::add ||
            (change.added_role == intent.added_role &&
             change.added_description == intent.added_description)) &&
           (static_cast<bool>(change.replacement && change.replacement->embedded_source_ordinal) ==
            intent.replacement_embedded_source.has_value()) &&
           (!intent.replacement_embedded_source ||
            (change.replacement &&
             change.replacement->raw_path == intent.replacement_embedded_source->raw_source_path &&
             change.replacement->source_revision ==
                 intent.replacement_embedded_source->source_revision &&
             *change.replacement->embedded_source_ordinal ==
                 intent.replacement_embedded_source->source_ordinal &&
             change.replacement->content_fingerprint ==
                 intent.replacement_embedded_source->content_fingerprint));
}

[[nodiscard]] bool valid_intent_shape(const ArtworkWritePlanIntent& intent) {
    if (intent.raw_media_path.empty() || intent.raw_media_path.find('\0') != std::string::npos) {
        return false;
    }
    if (intent.kind == ArtworkWritePlanIntentKind::replace ||
        intent.kind == ArtworkWritePlanIntentKind::add) {
        if (!intent.replacement_raw_path || intent.replacement_raw_path->empty() ||
            intent.replacement_raw_path->find('\0') != std::string::npos) {
            return false;
        }
        const auto embedded_shape =
            !intent.replacement_embedded_source ||
            (intent.replacement_embedded_source->provenance == ArtworkProvenance::embedded &&
             intent.replacement_embedded_source->raw_source_path == *intent.replacement_raw_path);
        return embedded_shape &&
               (intent.kind != ArtworkWritePlanIntentKind::add ||
                intent.added_description.size() <= maximum_added_description_bytes);
    }
    return intent.kind == ArtworkWritePlanIntentKind::remove && !intent.replacement_raw_path;
}

[[nodiscard]] ArtworkWritePlanSource make_source(const ArtworkWritePlanIntent& intent) {
    ArtworkWritePlanSource source{
        .raw_media_path = intent.raw_media_path,
        .occurrence_indexes = {intent.occurrence_index},
        .expected_media_revision = intent.expected_media_revision,
        .observed_media_revision = std::nullopt,
        .adapter_name = {},
        .change =
            ArtworkWritePlanChange{
                .kind = intent.kind,
                .target_ordinal = intent.target_ordinal,
                .expected_target_fingerprint = intent.expected_target_fingerprint,
                .original = std::nullopt,
                .replacement = std::nullopt,
                .added_role = intent.added_role,
                .added_description = intent.added_description,
            },
        .issues = {},
    };
    if ((intent.kind == ArtworkWritePlanIntentKind::replace ||
         intent.kind == ArtworkWritePlanIntentKind::add) &&
        intent.replacement_raw_path) {
        source.change.replacement = ArtworkImageFile{
            .raw_path = *intent.replacement_raw_path,
            .source_revision = {},
            .mime_type = {},
            .width = std::nullopt,
            .height = std::nullopt,
            .byte_size = 0U,
            .content_fingerprint = {},
            .embedded_source_ordinal = std::nullopt,
        };
        if (intent.replacement_embedded_source) {
            const auto& embedded = *intent.replacement_embedded_source;
            source.change.replacement = ArtworkImageFile{
                .raw_path = embedded.raw_source_path,
                .source_revision = embedded.source_revision,
                .mime_type = embedded.mime_type,
                .width = embedded.width,
                .height = embedded.height,
                .byte_size = embedded.byte_size,
                .content_fingerprint = embedded.content_fingerprint,
                .embedded_source_ordinal = embedded.source_ordinal,
            };
        }
    }
    return source;
}

} // namespace

std::string_view artwork_write_plan_intent_kind_name(const ArtworkWritePlanIntentKind kind) {
    switch (kind) {
    case ArtworkWritePlanIntentKind::replace:
        return "replace";
    case ArtworkWritePlanIntentKind::remove:
        return "remove";
    case ArtworkWritePlanIntentKind::add:
        return "add";
    case ArtworkWritePlanIntentKind::batch:
        return "batch";
    }
    return "replace";
}

std::string_view artwork_write_plan_issue_kind_name(const ArtworkWritePlanIssueKind kind) {
    switch (kind) {
    case ArtworkWritePlanIssueKind::missing_baseline_revision:
        return "missing baseline revision";
    case ArtworkWritePlanIssueKind::inconsistent_baseline_revision:
        return "inconsistent baseline revision";
    case ArtworkWritePlanIssueKind::conflicting_logical_intents:
        return "conflicting logical intents";
    case ArtworkWritePlanIssueKind::source_revalidation_failed:
        return "source revalidation failed";
    case ArtworkWritePlanIssueKind::source_changed:
        return "source changed";
    case ArtworkWritePlanIssueKind::physical_source_alias:
        return "physical source alias";
    case ArtworkWritePlanIssueKind::writer_unavailable:
        return "writer unavailable";
    case ArtworkWritePlanIssueKind::target_not_found:
        return "target not found";
    case ArtworkWritePlanIssueKind::target_changed:
        return "target changed";
    case ArtworkWritePlanIssueKind::replacement_unavailable:
        return "replacement unavailable";
    case ArtworkWritePlanIssueKind::replacement_unsupported:
        return "replacement unsupported";
    case ArtworkWritePlanIssueKind::replacement_unchanged:
        return "replacement unchanged";
    }
    return "source revalidation failed";
}

bool ArtworkWritePlanSource::ready() const noexcept {
    const auto change_ready = [](const ArtworkWritePlanChange& item) {
        const auto complete =
            (item.kind == ArtworkWritePlanIntentKind::remove && !item.replacement) ||
            ((item.kind == ArtworkWritePlanIntentKind::replace ||
              item.kind == ArtworkWritePlanIntentKind::add) &&
             item.replacement);
        return complete && (item.kind == ArtworkWritePlanIntentKind::add || item.original);
    };
    const auto complete =
        expected_media_revision && observed_media_revision &&
        *expected_media_revision == *observed_media_revision &&
        (embed ? is_qualified_artwork_adapter(adapter_name) : folder_image.has_value()) &&
        change_ready(change) && std::ranges::all_of(additional_changes, change_ready);
    return complete &&
           std::ranges::none_of(issues, [](const auto& issue) { return issue.blocking; });
}

std::size_t ArtworkWritePlanSource::blocking_issue_count() const noexcept {
    return static_cast<std::size_t>(
        std::ranges::count_if(issues, [](const auto& issue) { return issue.blocking; }));
}

bool ArtworkWritePlan::ready() const noexcept {
    return !sources.empty() && std::ranges::all_of(sources, &ArtworkWritePlanSource::ready);
}

std::size_t ArtworkWritePlan::ready_source_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(sources, &ArtworkWritePlanSource::ready));
}

std::size_t ArtworkWritePlan::blocking_issue_count() const noexcept {
    std::size_t result = 0U;
    for (const auto& source : sources) {
        result += source.blocking_issue_count();
    }
    return result;
}

core::Result<ArtworkWritePlan>
build_artwork_write_plan(const std::vector<ArtworkWritePlanIntent>& intents,
                         const ArtworkWritePlanInventoryReader& inventory_reader,
                         const ArtworkWritePlanImageReader& image_reader,
                         const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested()) {
        return cancelled();
    }
    if (!inventory_reader || !image_reader) {
        return std::unexpected(plan_error(core::ErrorCode::invalid_argument,
                                          "artwork write-plan readers are not configured"));
    }
    if (intents.empty() || intents.size() > maximum_logical_intents) {
        return std::unexpected(plan_error(
            intents.empty() ? core::ErrorCode::invalid_argument : core::ErrorCode::limit_exceeded,
            intents.empty() ? "artwork write plan requires at least one intent"
                            : "artwork write-plan intent count exceeds the limit"));
    }
    if (std::ranges::any_of(intents,
                            [](const auto& intent) { return !valid_intent_shape(intent); })) {
        return std::unexpected(plan_error(core::ErrorCode::invalid_argument,
                                          "artwork write plan contains an invalid intent"));
    }

    ArtworkWritePlan plan{.sources = {}, .logical_intent_count = intents.size()};
    std::unordered_map<std::string, std::size_t> source_positions;
    source_positions.reserve(intents.size());
    for (const auto& intent : intents) {
        if (cancellation.is_cancellation_requested()) {
            return cancelled();
        }
        const auto [position, inserted] = source_positions.emplace(
            intent.raw_media_path + std::string(1, '\0') +
                (intent.kind == ArtworkWritePlanIntentKind::add
                     ? "add" + std::string(1, '\0') + *intent.replacement_raw_path +
                           std::string(1, '\0') +
                           std::to_string(static_cast<int>(intent.added_role)) +
                           std::string(1, '\0') + intent.added_description
                     : std::to_string(intent.target_ordinal)),
            plan.sources.size());
        if (inserted) {
            plan.sources.push_back(make_source(intent));
            continue;
        }
        auto& source = plan.sources[position->second];
        source.occurrence_indexes.push_back(intent.occurrence_index);
        for (auto& issue : source.issues) {
            issue.occurrence_indexes = source.occurrence_indexes;
        }
        if (source.expected_media_revision != intent.expected_media_revision) {
            add_issue(source, ArtworkWritePlanIssueKind::inconsistent_baseline_revision,
                      plan_error(core::ErrorCode::conflict,
                                 "logical occurrences disagree on the captured media revision",
                                 source.raw_media_path));
        }
        if (!same_physical_intent(source, intent)) {
            add_issue(source, ArtworkWritePlanIssueKind::conflicting_logical_intents,
                      plan_error(core::ErrorCode::conflict,
                                 "logical occurrences request different artwork changes",
                                 source.raw_media_path));
        }
    }

    std::unordered_map<std::string, core::Result<LocalArtworkInventory>> inventory_cache;
    const auto cached_inventory = [&](const std::string& path,
                                      const core::CancellationToken& token) {
        const auto found = inventory_cache.find(path);
        if (found != inventory_cache.end()) {
            return found->second;
        }
        auto read = inventory_reader(path, token);
        inventory_cache.emplace(path, read);
        return read;
    };
    std::unordered_map<std::string, ArtworkImageFile> replacement_cache;
    replacement_cache.reserve(plan.sources.size());
    for (auto& source : plan.sources) {
        if (cancellation.is_cancellation_requested()) {
            return cancelled();
        }
        if (!source.expected_media_revision) {
            add_issue(source, ArtworkWritePlanIssueKind::missing_baseline_revision,
                      plan_error(core::ErrorCode::conflict,
                                 "artwork change has no captured media revision",
                                 source.raw_media_path));
            continue;
        }
        auto inventory = cached_inventory(source.raw_media_path, cancellation);
        if (!inventory) {
            if (inventory.error().code == core::ErrorCode::cancelled) {
                return std::unexpected(std::move(inventory.error()));
            }
            add_issue(source, ArtworkWritePlanIssueKind::source_revalidation_failed,
                      std::move(inventory.error()));
            continue;
        }
        source.observed_media_revision = inventory->media_revision;
        source.adapter_name = inventory->embedded_adapter_name;
        if (inventory->media_revision != *source.expected_media_revision) {
            add_issue(source, ArtworkWritePlanIssueKind::source_changed,
                      plan_error(core::ErrorCode::conflict,
                                 "media source changed after artwork was inspected",
                                 source.raw_media_path));
        }
        if (!inventory->capabilities.embedded_readable ||
            !is_qualified_artwork_adapter(inventory->embedded_adapter_name)) {
            add_issue(source, ArtworkWritePlanIssueKind::writer_unavailable,
                      plan_error(core::ErrorCode::unsupported,
                                 "artwork changes require a qualified FLAC, MP3, or MP4 file",
                                 source.raw_media_path));
            continue;
        }
        if (source.change.kind == ArtworkWritePlanIntentKind::add) {
            source.change.target_ordinal = static_cast<std::size_t>(
                std::ranges::count_if(inventory->items, [](const auto& item) {
                    return item.provenance == ArtworkProvenance::embedded;
                }));
        }
        if (source.change.kind != ArtworkWritePlanIntentKind::add) {
            const auto target = std::ranges::find_if(inventory->items, [&](const auto& item) {
                return item.provenance == ArtworkProvenance::embedded &&
                       item.source_ordinal == source.change.target_ordinal;
            });
            if (target == inventory->items.end()) {
                add_issue(source, ArtworkWritePlanIssueKind::target_not_found,
                          plan_error(core::ErrorCode::conflict,
                                     "the reviewed embedded artwork ordinal no longer exists",
                                     source.raw_media_path));
                continue;
            }
            source.change.original = *target;
            if (target->content_fingerprint != source.change.expected_target_fingerprint) {
                add_issue(source, ArtworkWritePlanIssueKind::target_changed,
                          plan_error(core::ErrorCode::conflict,
                                     "the reviewed embedded artwork bytes changed",
                                     source.raw_media_path));
            }
        }
        if ((source.change.kind != ArtworkWritePlanIntentKind::replace &&
             source.change.kind != ArtworkWritePlanIntentKind::add) ||
            !source.change.replacement) {
            continue;
        }
        const auto& replacement_path = source.change.replacement->raw_path;
        auto replacement_key = replacement_path;
        if (source.change.replacement->embedded_source_ordinal) {
            replacement_key.push_back('\0');
            replacement_key += std::to_string(*source.change.replacement->embedded_source_ordinal);
        }
        const auto cached = replacement_cache.find(replacement_key);
        if (cached != replacement_cache.end()) {
            source.change.replacement = cached->second;
        } else if (source.change.replacement->embedded_source_ordinal) {
            const auto expected = *source.change.replacement;
            if (std::ranges::any_of(plan.sources, [&](const auto& other) {
                    return other.raw_media_path == expected.raw_path;
                })) {
                add_issue(source, ArtworkWritePlanIssueKind::replacement_unavailable,
                          plan_error(core::ErrorCode::conflict,
                                     "the copied image's donor also has pending changes; export it "
                                     "first and add the exported image",
                                     expected.raw_path));
                continue;
            }
            auto donor_inventory = cached_inventory(expected.raw_path, cancellation);
            if (!donor_inventory) {
                if (donor_inventory.error().code == core::ErrorCode::cancelled) {
                    return std::unexpected(std::move(donor_inventory.error()));
                }
                add_issue(source, ArtworkWritePlanIssueKind::replacement_unavailable,
                          std::move(donor_inventory.error()));
                source.change.replacement.reset();
                continue;
            }
            const auto donor = std::ranges::find_if(donor_inventory->items, [&](const auto& item) {
                return item.provenance == ArtworkProvenance::embedded &&
                       item.source_ordinal == *expected.embedded_source_ordinal;
            });
            if (donor_inventory->media_revision != expected.source_revision ||
                donor == donor_inventory->items.end() ||
                donor->raw_source_path != expected.raw_path ||
                donor->content_fingerprint != expected.content_fingerprint ||
                donor->mime_type != expected.mime_type || donor->width != expected.width ||
                donor->height != expected.height || donor->byte_size != expected.byte_size) {
                add_issue(source, ArtworkWritePlanIssueKind::replacement_unavailable,
                          plan_error(core::ErrorCode::conflict,
                                     "embedded artwork donor changed after inspection",
                                     expected.raw_path));
                source.change.replacement.reset();
                continue;
            }
            replacement_cache.emplace(replacement_key, expected);
        } else {
            auto replacement = image_reader(replacement_path, cancellation);
            if (!replacement) {
                if (replacement.error().code == core::ErrorCode::cancelled) {
                    return std::unexpected(std::move(replacement.error()));
                }
                const auto issue_kind =
                    replacement.error().code == core::ErrorCode::unsupported ||
                            replacement.error().code == core::ErrorCode::limit_exceeded
                        ? ArtworkWritePlanIssueKind::replacement_unsupported
                        : ArtworkWritePlanIssueKind::replacement_unavailable;
                add_issue(source, issue_kind, std::move(replacement.error()));
                source.change.replacement.reset();
                continue;
            }
            replacement_cache.emplace(replacement_key, *replacement);
            source.change.replacement = std::move(*replacement);
        }
        const auto& replacement = *source.change.replacement;
        if (!replacement.width || !replacement.height ||
            *replacement.width > static_cast<std::uint32_t>(INT_MAX) ||
            *replacement.height > static_cast<std::uint32_t>(INT_MAX)) {
            add_issue(source, ArtworkWritePlanIssueKind::replacement_unsupported,
                      plan_error(core::ErrorCode::unsupported,
                                 "replacement dimensions cannot be represented in the container",
                                 replacement.raw_path));
        }
        const auto duplicate = std::ranges::find_if(inventory->items, [&](const auto& item) {
            return item.provenance == ArtworkProvenance::embedded &&
                   item.content_fingerprint == replacement.content_fingerprint &&
                   !std::ranges::any_of(plan.sources, [&](const auto& removal) {
                       return removal.raw_media_path == source.raw_media_path &&
                              removal.change.kind == ArtworkWritePlanIntentKind::remove &&
                              removal.change.target_ordinal == item.source_ordinal;
                   });
        });
        if ((source.change.kind == ArtworkWritePlanIntentKind::replace && source.change.original &&
             replacement.content_fingerprint == source.change.original->content_fingerprint) ||
            (source.change.kind == ArtworkWritePlanIntentKind::add &&
             duplicate != inventory->items.end())) {
            add_issue(source, ArtworkWritePlanIssueKind::replacement_unchanged,
                      plan_error(core::ErrorCode::conflict,
                                 source.change.kind == ArtworkWritePlanIntentKind::add
                                     ? "added artwork bytes already exist in the target"
                                     : "replacement artwork bytes are unchanged",
                                 replacement.raw_path));
        }
    }

    std::unordered_map<PhysicalKey, std::size_t, PhysicalKeyHash> physical_sources;
    physical_sources.reserve(plan.sources.size());
    for (std::size_t index = 0U; index < plan.sources.size(); ++index) {
        auto& source = plan.sources[index];
        if (!source.observed_media_revision) {
            continue;
        }
        const PhysicalKey key{.device = source.observed_media_revision->device,
                              .inode = source.observed_media_revision->inode};
        const auto [existing, inserted] = physical_sources.emplace(key, index);
        if (inserted) {
            continue;
        }
        auto& first = plan.sources[existing->second];
        if (first.raw_media_path == source.raw_media_path) {
            if (first.expected_media_revision != source.expected_media_revision) {
                add_issue(source, ArtworkWritePlanIssueKind::inconsistent_baseline_revision,
                          plan_error(core::ErrorCode::conflict,
                                     "artwork changes disagree on the captured revision",
                                     source.raw_media_path));
            }
            continue;
        }
        add_issue(first, ArtworkWritePlanIssueKind::physical_source_alias,
                  plan_error(core::ErrorCode::conflict,
                             "another artwork plan path addresses the same physical source",
                             first.raw_media_path));
        add_issue(source, ArtworkWritePlanIssueKind::physical_source_alias,
                  plan_error(core::ErrorCode::conflict,
                             "another artwork plan path addresses the same physical source",
                             source.raw_media_path));
    }
    // Original ordinals remain stable when edits run from the end backwards.
    // Insertions run last, after every reviewed removal.
    std::ranges::stable_sort(plan.sources, [](const auto& left, const auto& right) {
        if (left.raw_media_path != right.raw_media_path) {
            return left.raw_media_path < right.raw_media_path;
        }
        const auto left_add = left.change.kind == ArtworkWritePlanIntentKind::add;
        const auto right_add = right.change.kind == ArtworkWritePlanIntentKind::add;
        if (left_add != right_add) {
            return !left_add;
        }
        return !left_add && left.change.target_ordinal > right.change.target_ordinal;
    });
    std::unordered_map<std::string, std::size_t> added_counts;
    std::unordered_map<std::string, std::vector<core::ContentFingerprint>> added_images;
    for (auto& source : plan.sources) {
        if (source.change.kind != ArtworkWritePlanIntentKind::add) {
            continue;
        }
        source.change.target_ordinal += added_counts[source.raw_media_path]++;
        if (!source.change.replacement) {
            continue;
        }
        auto& images = added_images[source.raw_media_path];
        const auto fingerprint = source.change.replacement->content_fingerprint;
        if (std::ranges::find(images, fingerprint) != images.end()) {
            add_issue(source, ArtworkWritePlanIssueKind::replacement_unchanged,
                      plan_error(core::ErrorCode::conflict,
                                 "the same image is being added more than once",
                                 source.raw_media_path));
        }
        images.push_back(fingerprint);
    }
    std::vector<ArtworkWritePlanSource> grouped;
    std::size_t removed = 0;
    for (auto& source : plan.sources) {
        if (grouped.empty() || grouped.back().raw_media_path != source.raw_media_path) {
            removed = source.change.kind == ArtworkWritePlanIntentKind::remove ? 1U : 0U;
            grouped.push_back(std::move(source));
            continue;
        }
        if (source.change.kind == ArtworkWritePlanIntentKind::add) {
            source.change.target_ordinal -= removed;
        }
        if (source.change.kind == ArtworkWritePlanIntentKind::remove) {
            ++removed;
        }
        auto& destination = grouped.back();
        destination.additional_changes.push_back(std::move(source.change));
        destination.issues.insert(destination.issues.end(), source.issues.begin(),
                                  source.issues.end());
        for (const auto occurrence : source.occurrence_indexes) {
            if (!std::ranges::contains(destination.occurrence_indexes, occurrence)) {
                destination.occurrence_indexes.push_back(occurrence);
            }
        }
    }
    plan.sources = std::move(grouped);
    return plan;
}

core::Result<ArtworkWritePlan>
revalidate_artwork_write_plan(const std::vector<ArtworkWritePlanIntent>& intents,
                              const core::CancellationToken& cancellation) {
    return build_artwork_write_plan(
        intents,
        [](const std::string& raw_path, const core::CancellationToken& token) {
            auto policy = default_artwork_inventory_policy();
            policy.external_patterns.clear();
            return read_local_artwork_inventory(raw_path, policy, token);
        },
        [](const std::string& raw_path, const core::CancellationToken& token) {
            return read_artwork_image_file(raw_path, maximum_replacement_bytes, token);
        },
        cancellation);
}

bool is_qualified_artwork_adapter(const std::string_view adapter_name) {
    return adapter_name == "taglib-flac-picture-v1" || adapter_name == "taglib-id3v2-apic-v1" ||
           adapter_name == "taglib-mp4-covr-v1";
}

} // namespace trackknife::metadata
