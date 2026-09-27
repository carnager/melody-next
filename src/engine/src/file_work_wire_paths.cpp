// SPDX-License-Identifier: GPL-3.0-only

// ADR-0237, stage 5: moves and renames as they cross the protocol -- the
// reviewed path plan, the engine's look at its filesystem for it, the whole
// preparation (tags, paths, or both) and what publishing it did -- as exactly
// as the rest of file_work_wire. Every path and path fragment travels as
// encoded bytes: a name the layout made is compared byte for byte later.

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

constexpr std::array<std::pair<operations::OutputPathPlanIssueKind, std::string_view>, 16>
    plan_issue_kinds{{
        {operations::OutputPathPlanIssueKind::invalid_source_path, "invalid_source_path"},
        {operations::OutputPathPlanIssueKind::expression_evaluation_failed,
         "expression_evaluation_failed"},
        {operations::OutputPathPlanIssueKind::invalid_expression_output,
         "invalid_expression_output"},
        {operations::OutputPathPlanIssueKind::absolute_relative_directory,
         "absolute_relative_directory"},
        {operations::OutputPathPlanIssueKind::component_too_long, "component_too_long"},
        {operations::OutputPathPlanIssueKind::path_too_long, "path_too_long"},
        {operations::OutputPathPlanIssueKind::containment_failure, "containment_failure"},
        {operations::OutputPathPlanIssueKind::shared_source_target_conflict,
         "shared_source_target_conflict"},
        {operations::OutputPathPlanIssueKind::shared_source_revision_conflict,
         "shared_source_revision_conflict"},
        {operations::OutputPathPlanIssueKind::physical_source_alias, "physical_source_alias"},
        {operations::OutputPathPlanIssueKind::duplicate_target, "duplicate_target"},
        {operations::OutputPathPlanIssueKind::existing_target, "existing_target"},
        {operations::OutputPathPlanIssueKind::target_parent_not_directory,
         "target_parent_not_directory"},
        {operations::OutputPathPlanIssueKind::source_target_dependency, "source_target_dependency"},
        {operations::OutputPathPlanIssueKind::case_only_change, "case_only_change"},
        {operations::OutputPathPlanIssueKind::mirror_source_outside_root,
         "mirror_source_outside_root"},
    }};

constexpr std::array<std::pair<operations::OutputPathPreflightIssueKind, std::string_view>, 16>
    preflight_issue_kinds{{
        {operations::OutputPathPreflightIssueKind::source_missing, "source_missing"},
        {operations::OutputPathPreflightIssueKind::source_symlink, "source_symlink"},
        {operations::OutputPathPreflightIssueKind::source_not_regular, "source_not_regular"},
        {operations::OutputPathPreflightIssueKind::source_changed, "source_changed"},
        {operations::OutputPathPreflightIssueKind::source_hard_linked, "source_hard_linked"},
        {operations::OutputPathPreflightIssueKind::source_parent_not_writable,
         "source_parent_not_writable"},
        {operations::OutputPathPreflightIssueKind::operation_root_missing,
         "operation_root_missing"},
        {operations::OutputPathPreflightIssueKind::operation_root_symlink,
         "operation_root_symlink"},
        {operations::OutputPathPreflightIssueKind::operation_root_not_directory,
         "operation_root_not_directory"},
        {operations::OutputPathPreflightIssueKind::target_parent_symlink, "target_parent_symlink"},
        {operations::OutputPathPreflightIssueKind::target_parent_not_directory,
         "target_parent_not_directory"},
        {operations::OutputPathPreflightIssueKind::target_parent_not_writable,
         "target_parent_not_writable"},
        {operations::OutputPathPreflightIssueKind::target_exists, "target_exists"},
        {operations::OutputPathPreflightIssueKind::component_too_long, "component_too_long"},
        {operations::OutputPathPreflightIssueKind::path_too_long, "path_too_long"},
        {operations::OutputPathPreflightIssueKind::filesystem_observation_failed,
         "filesystem_observation_failed"},
    }};

constexpr std::array<std::pair<operations::OutputPathPublicationKind, std::string_view>, 3>
    publication_kinds{{
        {operations::OutputPathPublicationKind::no_change, "no_change"},
        {operations::OutputPathPublicationKind::same_filesystem_rename, "same_filesystem_rename"},
        {operations::OutputPathPublicationKind::cross_filesystem_copy, "cross_filesystem_copy"},
    }};

constexpr std::array<std::pair<operations::PreparationPlanIssueKind, std::string_view>, 6>
    preparation_issue_kinds{{
        {operations::PreparationPlanIssueKind::no_effect, "no_effect"},
        {operations::PreparationPlanIssueKind::metadata_plan_missing, "metadata_plan_missing"},
        {operations::PreparationPlanIssueKind::path_plan_missing, "path_plan_missing"},
        {operations::PreparationPlanIssueKind::path_preflight_missing, "path_preflight_missing"},
        {operations::PreparationPlanIssueKind::combined_source_mismatch,
         "combined_source_mismatch"},
        {operations::PreparationPlanIssueKind::replaygain_unavailable, "replaygain_unavailable"},
    }};

constexpr std::array<std::pair<operations::FilePublicationContentKind, std::string_view>, 2>
    content_kinds{{
        {operations::FilePublicationContentKind::preserve_source_bytes, "preserve_source_bytes"},
        {operations::FilePublicationContentKind::prepared_destination_artifact,
         "prepared_destination_artifact"},
    }};

constexpr std::array<std::pair<operations::FilePublicationApplySourceState, std::string_view>, 6>
    apply_states{{
        {operations::FilePublicationApplySourceState::pending, "pending"},
        {operations::FilePublicationApplySourceState::running, "running"},
        {operations::FilePublicationApplySourceState::unchanged, "unchanged"},
        {operations::FilePublicationApplySourceState::committed, "committed"},
        {operations::FilePublicationApplySourceState::failed, "failed"},
        {operations::FilePublicationApplySourceState::cancelled, "cancelled"},
    }};

[[nodiscard]] Json encode_bytes(const std::optional<std::string>& value) {
    return value ? Json(protocol::encode_raw_path(*value)) : Json();
}

[[nodiscard]] std::optional<std::string> optional_bytes(Reader& in, const std::string_view name) {
    if (in.find(name) == nullptr) {
        return std::nullopt;
    }
    return in.bytes(name);
}

[[nodiscard]] Json encode_byte_list(const std::vector<std::string>& values) {
    return encode_list(values,
                       [](const std::string& value) { return protocol::encode_raw_path(value); });
}

[[nodiscard]] std::vector<std::string> read_byte_list(Reader& in, const std::string_view name) {
    std::vector<std::string> result;
    const auto* value = in.need(name);
    if (value == nullptr) {
        return result;
    }
    if (!value->is_array()) {
        in.fail(std::string{name} + " must be a list");
        return result;
    }
    for (const auto& entry : *value) {
        auto decoded = entry.is_string()
                           ? protocol::decode_raw_path(entry.get<std::string>())
                           : core::Result<std::string>{std::unexpected(core::Error{})};
        if (!decoded) {
            in.fail(std::string{name} + " must hold encoded bytes");
            return result;
        }
        result.push_back(std::move(*decoded));
    }
    return result;
}

[[nodiscard]] std::uint32_t read_u32(Reader& in, const std::string_view name) {
    const auto value = in.index(name);
    if (value > UINT32_MAX) {
        in.fail(std::string{name} + " is out of range");
        return 0U;
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] Json encode_policy(const operations::PolicyVersion& policy) {
    return Json{{"name", encode_text(policy.name)}, {"version", policy.version}};
}

[[nodiscard]] operations::PolicyVersion read_policy(Reader& in) {
    return operations::PolicyVersion{.name = in.text("name"), .version = read_u32(in, "version")};
}

[[nodiscard]] Json encode_layout(const operations::OutputLayoutProfile& layout) {
    return Json{
        {"schema_version", layout.schema_version},
        {"name", encode_text(layout.name)},
        {"dialect", Json{{"dialect", encode_text(layout.dialect.dialect)},
                         {"dialect_version", layout.dialect.dialect_version},
                         {"compiler_schema", layout.dialect.compiler_schema}}},
        {"relative_directory_expression", encode_text(layout.relative_directory_expression)},
        {"basename_expression", encode_text(layout.basename_expression)},
        {"sanitization_policy", encode_policy(layout.sanitization_policy)}};
}

[[nodiscard]] operations::OutputLayoutProfile read_layout(Reader& in) {
    operations::OutputLayoutProfile layout;
    layout.schema_version = read_u32(in, "schema_version");
    layout.name = in.text("name");
    if (auto dialect =
            in.optional_object<titleformat::DialectVersion>("dialect", [](Reader& dialect_in) {
                return titleformat::DialectVersion{
                    .dialect = dialect_in.text("dialect"),
                    .dialect_version = read_u32(dialect_in, "dialect_version"),
                    .compiler_schema = read_u32(dialect_in, "compiler_schema")};
            })) {
        layout.dialect = std::move(*dialect);
    } else {
        in.fail("dialect is missing");
    }
    layout.relative_directory_expression = in.text("relative_directory_expression");
    layout.basename_expression = in.text("basename_expression");
    if (auto policy =
            in.optional_object<operations::PolicyVersion>("sanitization_policy", read_policy)) {
        layout.sanitization_policy = std::move(*policy);
    } else {
        in.fail("sanitization_policy is missing");
    }
    return layout;
}

[[nodiscard]] Json encode_destination(const operations::DestinationProfile& destination) {
    return Json{{"schema_version", destination.schema_version},
                {"name", encode_text(destination.name)},
                {"root_path", protocol::encode_raw_path(destination.root_raw_path)},
                {"containment_policy", encode_policy(destination.containment_policy)}};
}

[[nodiscard]] operations::DestinationProfile read_destination(Reader& in) {
    operations::DestinationProfile destination;
    destination.schema_version = read_u32(in, "schema_version");
    destination.name = in.text("name");
    destination.root_raw_path = in.bytes("root_path");
    if (auto policy =
            in.optional_object<operations::PolicyVersion>("containment_policy", read_policy)) {
        destination.containment_policy = std::move(*policy);
    } else {
        in.fail("containment_policy is missing");
    }
    return destination;
}

[[nodiscard]] Json encode_planned(const operations::PlannedOutputPathSource& source) {
    return Json{
        {"source_path", protocol::encode_raw_path(source.source_raw_path)},
        {"source_revision", encode(source.source_revision)},
        {"target_path", protocol::encode_raw_path(source.target_raw_path)},
        {"raw_relative_directory", protocol::encode_raw_path(source.raw_relative_directory)},
        {"sanitized_relative_directory",
         protocol::encode_raw_path(source.sanitized_relative_directory)},
        {"raw_basename", protocol::encode_raw_path(source.raw_basename)},
        {"sanitized_basename", protocol::encode_raw_path(source.sanitized_basename)},
        {"item_indexes", source.item_indexes},
        {"sanitized", source.sanitized},
        {"no_change", source.no_change}};
}

[[nodiscard]] operations::PlannedOutputPathSource read_planned(Reader& in) {
    return operations::PlannedOutputPathSource{
        .source_raw_path = in.bytes("source_path"),
        .source_revision = in.revision("source_revision"),
        .target_raw_path = in.bytes("target_path"),
        .raw_relative_directory = in.bytes("raw_relative_directory"),
        .sanitized_relative_directory = in.bytes("sanitized_relative_directory"),
        .raw_basename = in.bytes("raw_basename"),
        .sanitized_basename = in.bytes("sanitized_basename"),
        .item_indexes = in.indexes("item_indexes"),
        .sanitized = in.flag("sanitized"),
        .no_change = in.flag("no_change"),
    };
}

[[nodiscard]] Json encode_path_plan(const operations::OutputPathPlan& plan) {
    return Json{
        {"layout", encode_layout(plan.layout)},
        {"destination", plan.destination ? encode_destination(*plan.destination) : Json()},
        {"operations", Json{{"rename_files", plan.operations.rename_files},
                            {"move_files", plan.operations.move_files}}},
        {"sources", encode_list(plan.sources, encode_planned)},
        {"issues", encode_list(plan.issues, [](const operations::OutputPathPlanIssue& issue) {
             return Json{{"kind", name_of(issue.kind, plan_issue_kinds)},
                         {"blocking", issue.blocking},
                         {"message", encode_text(issue.message)},
                         {"item_indexes", issue.item_indexes},
                         {"source_path", encode_bytes(issue.source_raw_path)},
                         {"target_path", encode_bytes(issue.target_raw_path)}};
         })}};
}

[[nodiscard]] operations::OutputPathPlan read_path_plan(Reader& in) {
    operations::OutputPathPlan plan;
    if (auto layout = in.optional_object<operations::OutputLayoutProfile>("layout", read_layout)) {
        plan.layout = std::move(*layout);
    } else {
        in.fail("layout is missing");
    }
    plan.destination =
        in.optional_object<operations::DestinationProfile>("destination", read_destination);
    if (auto selection = in.optional_object<operations::OutputPathOperationSelection>(
            "operations", [](Reader& selection_in) {
                return operations::OutputPathOperationSelection{
                    .rename_files = selection_in.flag("rename_files"),
                    .move_files = selection_in.flag("move_files")};
            })) {
        plan.operations = *selection;
    } else {
        in.fail("operations is missing");
    }
    plan.sources = in.list<operations::PlannedOutputPathSource>("sources", read_planned);
    plan.issues = in.list<operations::OutputPathPlanIssue>("issues", [](Reader& issue_in) {
        operations::OutputPathPlanIssue issue;
        issue.kind = issue_in.named("kind", plan_issue_kinds);
        issue.blocking = issue_in.flag("blocking");
        issue.message = issue_in.text("message");
        issue.item_indexes = issue_in.indexes("item_indexes");
        issue.source_raw_path = optional_bytes(issue_in, "source_path");
        issue.target_raw_path = optional_bytes(issue_in, "target_path");
        return issue;
    });
    return plan;
}

[[nodiscard]] Json encode_path_preflight(const operations::OutputPathPreflight& preflight) {
    return Json{
        {"plan", encode_path_plan(preflight.plan)},
        {"sources",
         encode_list(preflight.sources,
                     [](const operations::OutputPathPreflightSource& source) {
                         return Json{
                             {"planned", encode_planned(source.planned)},
                             {"observed_revision", encode(source.observed_revision)},
                             {"publication", name_of(source.publication, publication_kinds)},
                             {"target_filesystem_device", source.target_filesystem_device},
                             {"missing_directories",
                              encode_byte_list(source.missing_directory_raw_paths)}};
                     })},
        {"issues",
         encode_list(preflight.issues, [](const operations::OutputPathPreflightIssue& issue) {
             return Json{{"kind", name_of(issue.kind, preflight_issue_kinds)},
                         {"blocking", issue.blocking},
                         {"message", encode_text(issue.message)},
                         {"item_indexes", issue.item_indexes},
                         {"source_path", protocol::encode_raw_path(issue.source_raw_path)},
                         {"target_path", protocol::encode_raw_path(issue.target_raw_path)}};
         })}};
}

[[nodiscard]] operations::OutputPathPreflight read_path_preflight(Reader& in) {
    operations::OutputPathPreflight preflight;
    if (auto plan = in.optional_object<operations::OutputPathPlan>("plan", read_path_plan)) {
        preflight.plan = std::move(*plan);
    } else {
        in.fail("plan is missing");
    }
    preflight.sources =
        in.list<operations::OutputPathPreflightSource>("sources", [](Reader& source_in) {
            operations::OutputPathPreflightSource source;
            if (auto planned = source_in.optional_object<operations::PlannedOutputPathSource>(
                    "planned", read_planned)) {
                source.planned = std::move(*planned);
            } else {
                source_in.fail("planned is missing");
            }
            source.observed_revision = source_in.revision("observed_revision");
            source.publication = source_in.named("publication", publication_kinds);
            source.target_filesystem_device = source_in.index("target_filesystem_device");
            source.missing_directory_raw_paths = read_byte_list(source_in, "missing_directories");
            return source;
        });
    preflight.issues =
        in.list<operations::OutputPathPreflightIssue>("issues", [](Reader& issue_in) {
            operations::OutputPathPreflightIssue issue;
            issue.kind = issue_in.named("kind", preflight_issue_kinds);
            issue.blocking = issue_in.flag("blocking");
            issue.message = issue_in.text("message");
            issue.item_indexes = issue_in.indexes("item_indexes");
            issue.source_raw_path = issue_in.bytes("source_path");
            issue.target_raw_path = issue_in.bytes("target_path");
            return issue;
        });
    return preflight;
}

[[nodiscard]] Json encode_publication_commit(const operations::FilePublicationCommitResult& done) {
    return Json{{"journal_id", done.journal_id.to_string()},
                {"content", name_of(done.content, content_kinds)},
                {"source_path", protocol::encode_raw_path(done.source_raw_path)},
                {"target_path", protocol::encode_raw_path(done.target_raw_path)},
                {"source_revision", encode(done.source_revision)},
                {"target_revision", encode(done.target_revision)},
                {"occurrence_indexes", done.occurrence_indexes},
                {"notes", encode_texts(done.notes)}};
}

[[nodiscard]] operations::FilePublicationCommitResult read_publication_commit(Reader& in) {
    operations::FilePublicationCommitResult done;
    if (auto id = core::StableId::parse(in.text("journal_id"))) {
        done.journal_id = *id;
    } else {
        in.fail("journal_id is not an identity");
    }
    done.content = in.named("content", content_kinds);
    done.source_raw_path = in.bytes("source_path");
    done.target_raw_path = in.bytes("target_path");
    done.source_revision = in.revision("source_revision");
    done.target_revision = in.revision("target_revision");
    done.occurrence_indexes = in.indexes("occurrence_indexes");
    done.notes = in.texts("notes");
    return done;
}

template <typename T>
[[nodiscard]] core::Result<T> read_whole(const Json& value, T (*read)(Reader&)) {
    Reader in{value};
    auto decoded = read(in);
    if (!in.ok()) {
        return std::unexpected(in.error());
    }
    return decoded;
}

} // namespace

Json encode(const operations::OutputPathPlan& plan) { return encode_path_plan(plan); }

core::Result<operations::OutputPathPlan> decode_path_plan(const Json& value) {
    return read_whole(value, read_path_plan);
}

Json encode(const operations::OutputPathPreflight& preflight) {
    return encode_path_preflight(preflight);
}

core::Result<operations::OutputPathPreflight> decode_path_preflight(const Json& value) {
    return read_whole(value, read_path_preflight);
}

Json encode(const operations::PreparationPlan& plan) {
    return Json{
        {"operations", Json{{"save_tags", plan.operations.save_tags},
                            {"rename_files", plan.operations.rename_files},
                            {"move_files", plan.operations.move_files},
                            {"replaygain", plan.operations.replaygain}}},
        {"metadata_context_change_count", plan.metadata_context_change_count},
        {"metadata", plan.metadata ? encode(*plan.metadata) : Json()},
        {"output_paths", plan.output_paths ? encode_path_plan(*plan.output_paths) : Json()},
        {"path_preflight",
         plan.path_preflight ? encode_path_preflight(*plan.path_preflight) : Json()},
        {"issues", encode_list(plan.issues, [](const operations::PreparationPlanIssue& issue) {
             return Json{{"kind", name_of(issue.kind, preparation_issue_kinds)},
                         {"blocking", issue.blocking},
                         {"message", encode_text(issue.message)}};
         })}};
}

core::Result<operations::PreparationPlan> decode_preparation_plan(const Json& value) {
    Reader in{value};
    operations::PreparationPlan plan;
    if (auto selection = in.optional_object<operations::PreparationOperationSelection>(
            "operations", [](Reader& selection_in) {
                return operations::PreparationOperationSelection{
                    .save_tags = selection_in.flag("save_tags"),
                    .rename_files = selection_in.flag("rename_files"),
                    .move_files = selection_in.flag("move_files"),
                    .replaygain = selection_in.flag("replaygain")};
            })) {
        plan.operations = *selection;
    } else {
        in.fail("operations is missing");
    }
    plan.metadata_context_change_count = in.index("metadata_context_change_count");
    if (const auto* metadata = in.find("metadata")) {
        auto decoded = decode_write_plan(*metadata);
        if (decoded) {
            plan.metadata = std::move(*decoded);
        } else {
            in.adopt(decoded.error());
        }
    }
    plan.output_paths =
        in.optional_object<operations::OutputPathPlan>("output_paths", read_path_plan);
    plan.path_preflight =
        in.optional_object<operations::OutputPathPreflight>("path_preflight", read_path_preflight);
    plan.issues = in.list<operations::PreparationPlanIssue>("issues", [](Reader& issue_in) {
        operations::PreparationPlanIssue issue;
        issue.kind = issue_in.named("kind", preparation_issue_kinds);
        issue.blocking = issue_in.flag("blocking");
        issue.message = issue_in.text("message");
        return issue;
    });
    if (!in.ok()) {
        return std::unexpected(in.error());
    }
    return plan;
}

Json encode(const operations::FilePublicationCommitResult& commit) {
    return encode_publication_commit(commit);
}

core::Result<operations::FilePublicationCommitResult> decode_publication_commit(const Json& value) {
    return read_whole(value, read_publication_commit);
}

Json encode(const operations::FilePublicationApplyResult& result) {
    auto sources =
        encode_list(result.sources, [](const operations::FilePublicationApplySourceResult& source) {
            return Json{
                {"source_index", source.source_index},
                {"source_path", protocol::encode_raw_path(source.source_raw_path)},
                {"target_path", protocol::encode_raw_path(source.target_raw_path)},
                {"publication", name_of(source.publication, publication_kinds)},
                {"state", name_of(source.state, apply_states)},
                {"commit", source.commit ? encode_publication_commit(*source.commit) : Json()},
                {"metadata_commit",
                 source.metadata_commit ? encode_commit(*source.metadata_commit) : Json()},
                {"published_metadata",
                 source.published_metadata ? encode(*source.published_metadata) : Json()},
                {"issue", encode_optional_error(source.issue)}};
        });
    return Json{{"sources", std::move(sources)},
                {"cancellation_requested", result.cancellation_requested}};
}

core::Result<operations::FilePublicationApplyResult>
decode_publication_apply_result(const Json& value) {
    Reader in{value};
    operations::FilePublicationApplyResult result;
    result.sources =
        in.list<operations::FilePublicationApplySourceResult>("sources", [](Reader& source_in) {
            operations::FilePublicationApplySourceResult source;
            source.source_index = source_in.index("source_index");
            source.source_raw_path = source_in.bytes("source_path");
            source.target_raw_path = source_in.bytes("target_path");
            source.publication = source_in.named("publication", publication_kinds);
            source.state = source_in.named("state", apply_states);
            source.commit = source_in.optional_object<operations::FilePublicationCommitResult>(
                "commit", read_publication_commit);
            source.metadata_commit = source_in.optional_object<operations::MetadataCommitResult>(
                "metadata_commit", read_commit);
            if (const auto* published = source_in.find("published_metadata")) {
                auto decoded = decode_document(*published);
                if (decoded) {
                    source.published_metadata = std::move(*decoded);
                } else {
                    source_in.adopt(decoded.error());
                }
            }
            source.issue = source_in.optional_error("issue");
            return source;
        });
    result.cancellation_requested = in.flag("cancellation_requested");
    if (!in.ok()) {
        return std::unexpected(in.error());
    }
    return result;
}

Json encode(const operations::FilePublicationApplyProgress& progress) {
    return Json{{"source_index", progress.source_index},
                {"source_path", protocol::encode_raw_path(progress.source_raw_path)},
                {"target_path", protocol::encode_raw_path(progress.target_raw_path)},
                {"publication", name_of(progress.publication, publication_kinds)},
                {"state", name_of(progress.state, apply_states)},
                {"completed_sources", progress.completed_sources},
                {"total_sources", progress.total_sources},
                {"issue", encode_optional_error(progress.issue)}};
}

core::Result<operations::FilePublicationApplyProgress>
decode_publication_apply_progress(const Json& value) {
    Reader in{value};
    operations::FilePublicationApplyProgress progress;
    progress.source_index = in.index("source_index");
    progress.source_raw_path = in.bytes("source_path");
    progress.target_raw_path = in.bytes("target_path");
    progress.publication = in.named("publication", publication_kinds);
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
