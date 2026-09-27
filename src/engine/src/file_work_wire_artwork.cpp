// SPDX-License-Identifier: GPL-3.0-only

// ADR-0237, stage 4: artwork as it crosses the protocol -- what a file holds,
// the images a plan names, the plan and what applying it did -- as exactly as
// the rest of file_work_wire. Encoded image bytes never travel in these:
// images are named by path, revision and content fingerprint, as the plans
// themselves hold them.

#include "trackknife/engine/file_work_wire.hpp"

#include "file_work_wire_internal.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace trackknife::engine::wire {
namespace {

using namespace detail;

constexpr std::array<std::pair<metadata::ArtworkRole, std::string_view>, 6> roles{{
    {metadata::ArtworkRole::front, "front"},
    {metadata::ArtworkRole::back, "back"},
    {metadata::ArtworkRole::artist, "artist"},
    {metadata::ArtworkRole::disc, "disc"},
    {metadata::ArtworkRole::icon, "icon"},
    {metadata::ArtworkRole::other, "other"},
}};

constexpr std::array<std::pair<metadata::ArtworkProvenance, std::string_view>, 2> provenances{{
    {metadata::ArtworkProvenance::embedded, "embedded"},
    {metadata::ArtworkProvenance::external, "external"},
}};

constexpr std::array<std::pair<metadata::ArtworkWritePlanIntentKind, std::string_view>, 4>
    change_kinds{{
        {metadata::ArtworkWritePlanIntentKind::replace, "replace"},
        {metadata::ArtworkWritePlanIntentKind::remove, "remove"},
        {metadata::ArtworkWritePlanIntentKind::add, "add"},
        {metadata::ArtworkWritePlanIntentKind::batch, "batch"},
    }};

constexpr std::array<std::pair<metadata::ArtworkWritePlanIssueKind, std::string_view>, 12>
    issue_kinds{{
        {metadata::ArtworkWritePlanIssueKind::missing_baseline_revision,
         "missing_baseline_revision"},
        {metadata::ArtworkWritePlanIssueKind::inconsistent_baseline_revision,
         "inconsistent_baseline_revision"},
        {metadata::ArtworkWritePlanIssueKind::conflicting_logical_intents,
         "conflicting_logical_intents"},
        {metadata::ArtworkWritePlanIssueKind::source_revalidation_failed,
         "source_revalidation_failed"},
        {metadata::ArtworkWritePlanIssueKind::source_changed, "source_changed"},
        {metadata::ArtworkWritePlanIssueKind::physical_source_alias, "physical_source_alias"},
        {metadata::ArtworkWritePlanIssueKind::writer_unavailable, "writer_unavailable"},
        {metadata::ArtworkWritePlanIssueKind::target_not_found, "target_not_found"},
        {metadata::ArtworkWritePlanIssueKind::target_changed, "target_changed"},
        {metadata::ArtworkWritePlanIssueKind::replacement_unavailable, "replacement_unavailable"},
        {metadata::ArtworkWritePlanIssueKind::replacement_unsupported, "replacement_unsupported"},
        {metadata::ArtworkWritePlanIssueKind::replacement_unchanged, "replacement_unchanged"},
    }};

constexpr std::array<std::pair<operations::ArtworkApplySourceState, std::string_view>, 5>
    apply_states{{
        {operations::ArtworkApplySourceState::pending, "pending"},
        {operations::ArtworkApplySourceState::running, "running"},
        {operations::ArtworkApplySourceState::committed, "committed"},
        {operations::ArtworkApplySourceState::failed, "failed"},
        {operations::ArtworkApplySourceState::cancelled, "cancelled"},
    }};

[[nodiscard]] std::string fingerprint_text(const core::ContentFingerprint& fingerprint) {
    constexpr std::string_view hex = "0123456789abcdef";
    std::string text;
    text.reserve(64);
    for (const auto byte : fingerprint.sha256) {
        text.push_back(hex[byte >> 4U]);
        text.push_back(hex[byte & 0x0FU]);
    }
    return text;
}

[[nodiscard]] core::ContentFingerprint read_fingerprint(Reader& in, const std::string_view name) {
    core::ContentFingerprint fingerprint;
    const auto* value = in.need(name);
    if (value == nullptr) {
        return fingerprint;
    }
    const auto nibble = [](const char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        return -1;
    };
    if (!value->is_string() || value->get_ref<const std::string&>().size() != 64U) {
        in.fail(std::string{name} + " must be a SHA-256 in hex");
        return fingerprint;
    }
    const auto& text = value->get_ref<const std::string&>();
    for (std::size_t index = 0; index < 32U; ++index) {
        const auto high = nibble(text[index * 2]);
        const auto low = nibble(text[(index * 2) + 1]);
        if (high < 0 || low < 0) {
            in.fail(std::string{name} + " must be a SHA-256 in hex");
            return fingerprint;
        }
        fingerprint.sha256[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return fingerprint;
}

template <typename T> [[nodiscard]] Json optional_number(const std::optional<T>& value) {
    return value ? Json(*value) : Json();
}

[[nodiscard]] std::optional<std::uint32_t> read_optional_edge(Reader& in,
                                                              const std::string_view name) {
    const auto index = in.optional_index(name);
    return index ? std::optional{static_cast<std::uint32_t>(*index)} : std::nullopt;
}

[[nodiscard]] metadata::ArtworkImageFile read_image(Reader& in) {
    metadata::ArtworkImageFile image;
    image.raw_path = in.bytes("path");
    image.source_revision = in.revision("revision");
    image.mime_type = in.text("mime_type");
    image.width = read_optional_edge(in, "width");
    image.height = read_optional_edge(in, "height");
    image.byte_size = in.index("byte_size");
    image.content_fingerprint = read_fingerprint(in, "fingerprint");
    image.embedded_source_ordinal = in.optional_index("embedded_source_ordinal");
    return image;
}

[[nodiscard]] metadata::ArtworkInventoryItem read_item(Reader& in) {
    metadata::ArtworkInventoryItem item;
    item.role = in.named("role", roles);
    item.native_type = in.text("native_type");
    item.mime_type = in.text("mime_type");
    item.description = in.text("description");
    item.width = read_optional_edge(in, "width");
    item.height = read_optional_edge(in, "height");
    item.byte_size = in.index("byte_size");
    item.content_fingerprint = read_fingerprint(in, "fingerprint");
    item.provenance = in.named("provenance", provenances);
    item.raw_source_path = in.bytes("source_path");
    item.source_revision = in.revision("source_revision");
    item.source_ordinal = in.index("source_ordinal");
    item.duplicate_of = in.optional_index("duplicate_of");
    return item;
}

[[nodiscard]] Json encode_item(const metadata::ArtworkInventoryItem& item) {
    return Json{{"role", name_of(item.role, roles)},
                {"native_type", encode_text(item.native_type)},
                {"mime_type", encode_text(item.mime_type)},
                {"description", encode_text(item.description)},
                {"width", optional_number(item.width)},
                {"height", optional_number(item.height)},
                {"byte_size", item.byte_size},
                {"fingerprint", fingerprint_text(item.content_fingerprint)},
                {"provenance", name_of(item.provenance, provenances)},
                {"source_path", protocol::encode_raw_path(item.raw_source_path)},
                {"source_revision", encode(item.source_revision)},
                {"source_ordinal", item.source_ordinal},
                {"duplicate_of", optional_number(item.duplicate_of)}};
}

[[nodiscard]] Json encode_change(const metadata::ArtworkWritePlanChange& change) {
    return Json{
        {"kind", name_of(change.kind, change_kinds)},
        {"target_ordinal", change.target_ordinal},
        {"expected_target_fingerprint", fingerprint_text(change.expected_target_fingerprint)},
        {"original", change.original ? encode_item(*change.original) : Json()},
        {"replacement", change.replacement ? encode(*change.replacement) : Json()},
        {"added_role", name_of(change.added_role, roles)},
        {"added_description", encode_text(change.added_description)}};
}

[[nodiscard]] metadata::ArtworkWritePlanChange read_change(Reader& in) {
    metadata::ArtworkWritePlanChange change;
    change.kind = in.named("kind", change_kinds);
    change.target_ordinal = in.index("target_ordinal");
    change.expected_target_fingerprint = read_fingerprint(in, "expected_target_fingerprint");
    change.original = in.optional_object<metadata::ArtworkInventoryItem>("original", read_item);
    change.replacement = in.optional_object<metadata::ArtworkImageFile>("replacement", read_image);
    change.added_role = in.named("added_role", roles);
    change.added_description = in.text("added_description");
    return change;
}

[[nodiscard]] Json encode_issue(const metadata::ArtworkWritePlanIssue& issue) {
    return Json{{"kind", name_of(issue.kind, issue_kinds)},
                {"error", encode(issue.error)},
                {"occurrence_indexes", issue.occurrence_indexes},
                {"blocking", issue.blocking}};
}

[[nodiscard]] metadata::ArtworkWritePlanIssue read_issue(Reader& in) {
    metadata::ArtworkWritePlanIssue issue;
    issue.kind = in.named("kind", issue_kinds);
    if (const auto* error = in.need("error")) {
        if (auto decoded = decode_error(*error)) {
            issue.error = std::move(*decoded);
        } else {
            in.adopt(decoded.error());
        }
    }
    issue.occurrence_indexes = in.indexes("occurrence_indexes");
    issue.blocking = in.flag("blocking");
    return issue;
}

[[nodiscard]] metadata::ArtworkWritePlanSource read_source(Reader& in) {
    metadata::ArtworkWritePlanSource source;
    source.raw_media_path = in.bytes("media_path");
    source.occurrence_indexes = in.indexes("occurrence_indexes");
    source.expected_media_revision = in.optional_revision("expected_media_revision");
    source.observed_media_revision = in.optional_revision("observed_media_revision");
    source.adapter_name = in.text("adapter_name");
    if (const auto* change = in.need("change")) {
        Reader change_in{*change};
        source.change = read_change(change_in);
        if (!change_in.ok()) {
            in.adopt(change_in.error());
        }
    }
    source.issues = in.list<metadata::ArtworkWritePlanIssue>("issues", read_issue);
    source.additional_changes =
        in.list<metadata::ArtworkWritePlanChange>("additional_changes", read_change);
    source.embed = in.flag("embed");
    source.folder_image =
        in.optional_object<metadata::FolderImageWritePlan>("folder_image", [](Reader& folder_in) {
            metadata::FolderImageWritePlan folder;
            folder.raw_path = folder_in.bytes("path");
            if (const auto* image = folder_in.need("image")) {
                Reader image_in{*image};
                folder.image = read_image(image_in);
                if (!image_in.ok()) {
                    folder_in.adopt(image_in.error());
                }
            }
            folder.original =
                folder_in.optional_object<metadata::ArtworkImageFile>("original", read_image);
            return folder;
        });
    return source;
}

} // namespace

Json encode(const metadata::ArtworkImageFile& image) {
    return Json{{"path", protocol::encode_raw_path(image.raw_path)},
                {"revision", encode(image.source_revision)},
                {"mime_type", encode_text(image.mime_type)},
                {"width", optional_number(image.width)},
                {"height", optional_number(image.height)},
                {"byte_size", image.byte_size},
                {"fingerprint", fingerprint_text(image.content_fingerprint)},
                {"embedded_source_ordinal", optional_number(image.embedded_source_ordinal)}};
}

core::Result<metadata::ArtworkImageFile> decode_image_file(const Json& value) {
    Reader in{value};
    auto image = read_image(in);
    if (!in.ok()) {
        return std::unexpected(in.error());
    }
    return image;
}

Json encode(const metadata::ArtworkInventoryPolicy& policy) {
    auto patterns = Json::array();
    for (const auto& pattern : policy.external_patterns) {
        patterns.push_back(Json{{"basename", protocol::encode_raw_path(pattern.raw_basename)},
                                {"role", name_of(pattern.role, roles)}});
    }
    return Json{{"external_patterns", std::move(patterns)},
                {"maximum_items", policy.maximum_items},
                {"maximum_item_bytes", policy.maximum_item_bytes},
                {"maximum_total_bytes", policy.maximum_total_bytes}};
}

core::Result<metadata::ArtworkInventoryPolicy> decode_inventory_policy(const Json& value) {
    Reader in{value};
    metadata::ArtworkInventoryPolicy policy;
    policy.external_patterns =
        in.list<metadata::ExternalArtworkPattern>("external_patterns", [](Reader& pattern_in) {
            return metadata::ExternalArtworkPattern{.raw_basename = pattern_in.bytes("basename"),
                                                    .role = pattern_in.named("role", roles)};
        });
    policy.maximum_items = in.index("maximum_items");
    policy.maximum_item_bytes = in.index("maximum_item_bytes");
    policy.maximum_total_bytes = in.index("maximum_total_bytes");
    if (!in.ok()) {
        return std::unexpected(in.error());
    }
    return policy;
}

Json encode(const metadata::LocalArtworkInventory& inventory) {
    auto issues = Json::array();
    for (const auto& issue : inventory.issues) {
        issues.push_back(Json{{"source_path", protocol::encode_raw_path(issue.raw_source_path)},
                              {"error", encode(issue.error)}});
    }
    return Json{{"media_path", protocol::encode_raw_path(inventory.raw_media_path)},
                {"media_revision", encode(inventory.media_revision)},
                {"embedded_adapter_name", encode_text(inventory.embedded_adapter_name)},
                {"embedded_readable", inventory.capabilities.embedded_readable},
                {"external_readable", inventory.capabilities.external_readable},
                {"items", encode_list(inventory.items, encode_item)},
                {"issues", std::move(issues)}};
}

core::Result<metadata::LocalArtworkInventory> decode_inventory(const Json& value) {
    Reader in{value};
    metadata::LocalArtworkInventory inventory;
    inventory.raw_media_path = in.bytes("media_path");
    inventory.media_revision = in.revision("media_revision");
    inventory.embedded_adapter_name = in.text("embedded_adapter_name");
    inventory.capabilities.embedded_readable = in.flag("embedded_readable");
    inventory.capabilities.external_readable = in.flag("external_readable");
    inventory.items = in.list<metadata::ArtworkInventoryItem>("items", read_item);
    inventory.issues = in.list<metadata::ArtworkInventoryIssue>("issues", [](Reader& issue_in) {
        metadata::ArtworkInventoryIssue issue;
        issue.raw_source_path = issue_in.bytes("source_path");
        if (const auto* error = issue_in.need("error")) {
            if (auto decoded = decode_error(*error)) {
                issue.error = std::move(*decoded);
            } else {
                issue_in.adopt(decoded.error());
            }
        }
        return issue;
    });
    if (!in.ok()) {
        return std::unexpected(in.error());
    }
    return inventory;
}

Json encode(const metadata::ArtworkWritePlanSource& source) {
    Json folder = nullptr;
    if (source.folder_image) {
        folder =
            Json{{"path", protocol::encode_raw_path(source.folder_image->raw_path)},
                 {"image", encode(source.folder_image->image)},
                 {"original",
                  source.folder_image->original ? encode(*source.folder_image->original) : Json()}};
    }
    return Json{
        {"media_path", protocol::encode_raw_path(source.raw_media_path)},
        {"occurrence_indexes", source.occurrence_indexes},
        {"expected_media_revision", encode_optional_revision(source.expected_media_revision)},
        {"observed_media_revision", encode_optional_revision(source.observed_media_revision)},
        {"adapter_name", encode_text(source.adapter_name)},
        {"change", encode_change(source.change)},
        {"issues", encode_list(source.issues, encode_issue)},
        {"additional_changes", encode_list(source.additional_changes, encode_change)},
        {"embed", source.embed},
        {"folder_image", std::move(folder)}};
}

core::Result<metadata::ArtworkWritePlanSource> decode_artwork_source(const Json& value) {
    Reader in{value};
    auto source = read_source(in);
    if (!in.ok()) {
        return std::unexpected(in.error());
    }
    return source;
}

Json encode(const metadata::ArtworkWritePlan& plan) {
    return Json{{"sources", encode_list(plan.sources,
                                        [](const metadata::ArtworkWritePlanSource& source) {
                                            return encode(source);
                                        })},
                {"logical_intent_count", plan.logical_intent_count}};
}

core::Result<metadata::ArtworkWritePlan> decode_artwork_plan(const Json& value) {
    Reader in{value};
    metadata::ArtworkWritePlan plan;
    plan.sources = in.list<metadata::ArtworkWritePlanSource>("sources", read_source);
    plan.logical_intent_count = in.index("logical_intent_count");
    if (!in.ok()) {
        return std::unexpected(in.error());
    }
    return plan;
}

Json encode(const operations::ArtworkApplyResult& result) {
    auto sources = Json::array();
    for (const auto& source : result.sources) {
        sources.push_back(Json{{"source_index", source.source_index},
                               {"path", protocol::encode_raw_path(source.raw_path)},
                               {"state", name_of(source.state, apply_states)},
                               {"commit", source.commit ? encode_commit(*source.commit) : Json()},
                               {"issue", encode_optional_error(source.issue)}});
    }
    return Json{{"sources", std::move(sources)},
                {"cancellation_requested", result.cancellation_requested}};
}

core::Result<operations::ArtworkApplyResult> decode_artwork_apply_result(const Json& value) {
    Reader in{value};
    operations::ArtworkApplyResult result;
    result.sources =
        in.list<operations::ArtworkApplySourceResult>("sources", [](Reader& source_in) {
            operations::ArtworkApplySourceResult source;
            source.source_index = source_in.index("source_index");
            source.raw_path = source_in.bytes("path");
            source.state = source_in.named("state", apply_states);
            source.commit =
                source_in.optional_object<operations::MetadataCommitResult>("commit", read_commit);
            source.issue = source_in.optional_error("issue");
            return source;
        });
    result.cancellation_requested = in.flag("cancellation_requested");
    if (!in.ok()) {
        return std::unexpected(in.error());
    }
    return result;
}

Json encode(const operations::ArtworkApplyProgress& progress) {
    return Json{{"source_index", progress.source_index},
                {"path", protocol::encode_raw_path(progress.raw_path)},
                {"state", name_of(progress.state, apply_states)},
                {"completed_sources", progress.completed_sources},
                {"total_sources", progress.total_sources},
                {"issue", encode_optional_error(progress.issue)}};
}

core::Result<operations::ArtworkApplyProgress> decode_artwork_apply_progress(const Json& value) {
    Reader in{value};
    operations::ArtworkApplyProgress progress;
    progress.source_index = in.index("source_index");
    progress.raw_path = in.bytes("path");
    progress.state = in.named("state", apply_states);
    progress.completed_sources = in.index("completed_sources");
    progress.total_sources = in.index("total_sources");
    progress.issue = in.optional_error("issue");
    if (!in.ok()) {
        return std::unexpected(in.error());
    }
    return progress;
}

} // namespace trackknife::engine::wire
