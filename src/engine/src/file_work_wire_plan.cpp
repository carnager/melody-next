// SPDX-License-Identifier: GPL-3.0-only

// ADR-0237: the metadata write plan and what applying it did, encoded as
// exactly as the rest of file_work_wire -- the plan a client previewed is the
// plan the engine writes, field for field.

#include "trackknife/engine/file_work_wire.hpp"

#include "trackknife/protocol/dispatch.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace trackknife::engine::wire {
namespace {

// Reads one object, keeping the first thing found wrong; every accessor
// answers a default once something is, and the caller checks ok() at the end.
class Reader final {
  public:
    explicit Reader(const Json& object) : object_(object) {
        if (!object.is_object()) {
            fail("expected an object");
        }
    }

    [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
    [[nodiscard]] core::Error error() const { return *error_; }
    void fail(const std::string& what) {
        if (!error_) {
            error_ = core::Error{.code = core::ErrorCode::invalid_argument,
                                 .message = "malformed file-work document: " + what,
                                 .context = {}};
        }
    }
    void adopt(const core::Error& error) {
        if (!error_) {
            error_ = error;
        }
    }

    [[nodiscard]] const Json* find(const std::string_view name) {
        if (!ok()) {
            return nullptr;
        }
        const auto found = object_.find(name);
        return found == object_.end() || found->is_null() ? nullptr : &*found;
    }
    [[nodiscard]] const Json* need(const std::string_view name) {
        const auto* found = find(name);
        if (found == nullptr) {
            fail(std::string{name} + " is missing");
        }
        return found;
    }

    [[nodiscard]] std::size_t index(const std::string_view name) {
        const auto* value = need(name);
        if (value == nullptr) {
            return 0U;
        }
        if (value->is_number_unsigned()) {
            return value->get<std::size_t>();
        }
        if (value->is_number_integer() && value->get<std::int64_t>() >= 0) {
            return static_cast<std::size_t>(value->get<std::int64_t>());
        }
        fail(std::string{name} + " must be a non-negative integer");
        return 0U;
    }
    [[nodiscard]] std::optional<std::size_t> optional_index(const std::string_view name) {
        if (find(name) == nullptr) {
            return std::nullopt;
        }
        return index(name);
    }
    [[nodiscard]] std::vector<std::size_t> indexes(const std::string_view name) {
        std::vector<std::size_t> result;
        const auto* value = need(name);
        if (value == nullptr) {
            return result;
        }
        if (!value->is_array()) {
            fail(std::string{name} + " must be a list");
            return result;
        }
        for (const auto& entry : *value) {
            if (entry.is_number_unsigned()) {
                result.push_back(entry.get<std::size_t>());
            } else {
                fail(std::string{name} + " must hold non-negative integers");
            }
        }
        return result;
    }
    [[nodiscard]] bool flag(const std::string_view name) {
        const auto* value = need(name);
        if (value != nullptr && !value->is_boolean()) {
            fail(std::string{name} + " must be true or false");
            return false;
        }
        return value != nullptr && value->get<bool>();
    }
    [[nodiscard]] std::string text(const std::string_view name) {
        const auto* value = need(name);
        return value == nullptr ? std::string{} : take(decode_text(*value));
    }
    [[nodiscard]] std::optional<std::string> optional_text(const std::string_view name) {
        const auto* value = find(name);
        return value == nullptr ? std::nullopt : std::optional{take(decode_text(*value))};
    }
    [[nodiscard]] std::vector<std::string> texts(const std::string_view name) {
        std::vector<std::string> result;
        const auto* value = need(name);
        if (value == nullptr) {
            return result;
        }
        if (!value->is_array()) {
            fail(std::string{name} + " must be a list");
            return result;
        }
        for (const auto& entry : *value) {
            result.push_back(take(decode_text(entry)));
        }
        return result;
    }
    [[nodiscard]] std::string bytes(const std::string_view name) {
        const auto* value = need(name);
        if (value == nullptr) {
            return {};
        }
        if (value->is_string()) {
            if (auto decoded = protocol::decode_raw_path(value->get<std::string>())) {
                return std::move(*decoded);
            }
        }
        fail(std::string{name} + " must be encoded bytes");
        return {};
    }
    [[nodiscard]] core::LocalSourceRevision revision(const std::string_view name) {
        const auto* value = need(name);
        return value == nullptr ? core::LocalSourceRevision{} : take(decode_revision(*value));
    }
    [[nodiscard]] std::optional<core::LocalSourceRevision>
    optional_revision(const std::string_view name) {
        const auto* value = find(name);
        return value == nullptr ? std::nullopt : std::optional{take(decode_revision(*value))};
    }
    [[nodiscard]] std::optional<core::Error> optional_error(const std::string_view name) {
        const auto* value = find(name);
        return value == nullptr ? std::nullopt : std::optional{take(decode_error(*value))};
    }
    template <typename Enum, std::size_t Count>
    [[nodiscard]] Enum named(const std::string_view name,
                             const std::array<std::pair<Enum, std::string_view>, Count>& names) {
        const auto* value = need(name);
        if (value != nullptr && value->is_string()) {
            for (const auto& [kind, spelled] : names) {
                if (spelled == value->get_ref<const std::string&>()) {
                    return kind;
                }
            }
        }
        fail(std::string{name} + " is not one of its known values");
        return names.front().first;
    }
    template <typename T>
    [[nodiscard]] std::vector<T> list(const std::string_view name,
                                      const std::function<T(Reader&)>& read) {
        std::vector<T> result;
        const auto* value = need(name);
        if (value == nullptr) {
            return result;
        }
        if (!value->is_array()) {
            fail(std::string{name} + " must be a list");
            return result;
        }
        for (const auto& entry : *value) {
            Reader nested{entry};
            auto item = read(nested);
            if (!nested.ok()) {
                adopt(nested.error());
                return result;
            }
            result.push_back(std::move(item));
        }
        return result;
    }
    template <typename T>
    [[nodiscard]] std::optional<T> optional_object(const std::string_view name,
                                                   const std::function<T(Reader&)>& read) {
        const auto* value = find(name);
        if (value == nullptr) {
            return std::nullopt;
        }
        Reader nested{*value};
        auto item = read(nested);
        if (!nested.ok()) {
            adopt(nested.error());
            return std::nullopt;
        }
        return item;
    }

  private:
    template <typename T> [[nodiscard]] T take(core::Result<T> result) {
        if (!result) {
            adopt(result.error());
            return T{};
        }
        return std::move(*result);
    }

    const Json& object_;
    std::optional<core::Error> error_;
};

template <typename Enum, std::size_t Count>
[[nodiscard]] std::string
name_of(const Enum value, const std::array<std::pair<Enum, std::string_view>, Count>& names) {
    for (const auto& [kind, spelled] : names) {
        if (kind == value) {
            return std::string{spelled};
        }
    }
    return std::string{names.front().second};
}

[[nodiscard]] Json encode_texts(const std::vector<std::string>& texts) {
    auto list = Json::array();
    for (const auto& text : texts) {
        list.push_back(encode_text(text));
    }
    return list;
}

[[nodiscard]] Json encode_optional_revision(const std::optional<core::LocalSourceRevision>& value) {
    return value ? encode(*value) : Json();
}

[[nodiscard]] Json encode_optional_text(const std::optional<std::string>& value) {
    return value ? encode_text(*value) : Json();
}

[[nodiscard]] Json encode_optional_error(const std::optional<core::Error>& value) {
    return value ? encode(*value) : Json();
}

template <typename T, typename Encode>
[[nodiscard]] Json encode_list(const std::vector<T>& values, Encode encode_one) {
    auto list = Json::array();
    for (const auto& value : values) {
        list.push_back(encode_one(value));
    }
    return list;
}

constexpr std::array<std::pair<metadata::MetadataWritePlanIssueKind, std::string_view>, 10>
    issue_kinds{{
        {metadata::MetadataWritePlanIssueKind::missing_baseline_revision,
         "missing_baseline_revision"},
        {metadata::MetadataWritePlanIssueKind::inconsistent_baseline_revision,
         "inconsistent_baseline_revision"},
        {metadata::MetadataWritePlanIssueKind::source_revalidation_failed,
         "source_revalidation_failed"},
        {metadata::MetadataWritePlanIssueKind::source_changed, "source_changed"},
        {metadata::MetadataWritePlanIssueKind::physical_source_alias, "physical_source_alias"},
        {metadata::MetadataWritePlanIssueKind::conflicting_logical_edits,
         "conflicting_logical_edits"},
        {metadata::MetadataWritePlanIssueKind::unresolved_non_embedded_target,
         "unresolved_non_embedded_target"},
        {metadata::MetadataWritePlanIssueKind::writer_unavailable, "writer_unavailable"},
        {metadata::MetadataWritePlanIssueKind::preservation_unproven, "preservation_unproven"},
        {metadata::MetadataWritePlanIssueKind::unsupported_field_mapping,
         "unsupported_field_mapping"},
    }};

constexpr std::array<std::pair<metadata::StagedMetadataPatchKind, std::string_view>, 2> patch_kinds{
    {
        {metadata::StagedMetadataPatchKind::replace_values, "replace_values"},
        {metadata::StagedMetadataPatchKind::remove_field, "remove_field"},
    }};

constexpr std::array<std::pair<operations::MetadataApplySourceState, std::string_view>, 5>
    apply_states{{
        {operations::MetadataApplySourceState::pending, "pending"},
        {operations::MetadataApplySourceState::running, "running"},
        {operations::MetadataApplySourceState::committed, "committed"},
        {operations::MetadataApplySourceState::failed, "failed"},
        {operations::MetadataApplySourceState::cancelled, "cancelled"},
    }};

constexpr std::array<std::pair<operations::MetadataOperationContentKind, std::string_view>, 5>
    content_kinds{{
        {operations::MetadataOperationContentKind::text_fields, "text_fields"},
        {operations::MetadataOperationContentKind::embedded_artwork, "embedded_artwork"},
        {operations::MetadataOperationContentKind::cue_replay_gain, "cue_replay_gain"},
        {operations::MetadataOperationContentKind::loudness_sidecar, "loudness_sidecar"},
        {operations::MetadataOperationContentKind::folder_image, "folder_image"},
    }};

[[nodiscard]] Json encode_issue(const metadata::MetadataWritePlanIssue& issue) {
    return Json{{"kind", name_of(issue.kind, issue_kinds)},
                {"error", encode(issue.error)},
                {"field_index", issue.field_index ? Json(*issue.field_index) : Json()},
                {"item_indexes", issue.item_indexes},
                {"blocking", issue.blocking}};
}

[[nodiscard]] metadata::MetadataWritePlanIssue read_issue(Reader& in) {
    metadata::MetadataWritePlanIssue issue;
    issue.kind = in.named("kind", issue_kinds);
    if (const auto* error = in.need("error")) {
        if (auto decoded = decode_error(*error)) {
            issue.error = std::move(*decoded);
        } else {
            in.adopt(decoded.error());
        }
    }
    issue.field_index = in.optional_index("field_index");
    issue.item_indexes = in.indexes("item_indexes");
    issue.blocking = in.flag("blocking");
    return issue;
}

[[nodiscard]] Json encode_identity(const metadata::StagedLogicalIdentity& identity) {
    const auto optional = [](const auto& value) { return value ? Json(*value) : Json(); };
    return Json{{"stream_index", optional(identity.stream_index)},
                {"subsong_index", optional(identity.subsong_index)},
                {"start_sample", optional(identity.start_sample)},
                {"end_sample", optional(identity.end_sample)}};
}

[[nodiscard]] metadata::StagedLogicalIdentity read_identity(Reader& in) {
    metadata::StagedLogicalIdentity identity;
    const auto integer = [&in](const std::string_view name) -> std::optional<std::int64_t> {
        const auto* value = in.find(name);
        if (value == nullptr) {
            return std::nullopt;
        }
        if (!value->is_number_integer()) {
            in.fail(std::string{name} + " must be an integer");
            return std::nullopt;
        }
        return value->get<std::int64_t>();
    };
    if (const auto stream = integer("stream_index")) {
        identity.stream_index = static_cast<int>(*stream);
    }
    if (const auto subsong = integer("subsong_index")) {
        identity.subsong_index = static_cast<int>(*subsong);
    }
    identity.start_sample = integer("start_sample");
    identity.end_sample = integer("end_sample");
    return identity;
}

[[nodiscard]] Json encode_loudness_field(const metadata::MetadataWritePlanLoudnessField& field) {
    return Json{{"field_index", field.field_index},
                {"canonical_name", encode_text(field.canonical_name)},
                {"kind", name_of(field.kind, patch_kinds)},
                {"values", encode_texts(field.values)},
                {"item_indexes", field.item_indexes}};
}

[[nodiscard]] metadata::MetadataWritePlanLoudnessField read_loudness_field(Reader& in) {
    metadata::MetadataWritePlanLoudnessField field;
    field.field_index = in.index("field_index");
    field.canonical_name = in.text("canonical_name");
    field.kind = in.named("kind", patch_kinds);
    field.values = in.texts("values");
    field.item_indexes = in.indexes("item_indexes");
    return field;
}

[[nodiscard]] Json encode_applied_field(const operations::CueReplayGainAppliedField& field) {
    return Json{{"canonical_name", encode_text(field.canonical_name)},
                {"display_name", encode_text(field.display_name)},
                {"value", encode_optional_text(field.value)}};
}

[[nodiscard]] operations::CueReplayGainAppliedField read_applied_field(Reader& in) {
    return operations::CueReplayGainAppliedField{.canonical_name = in.text("canonical_name"),
                                                 .display_name = in.text("display_name"),
                                                 .value = in.optional_text("value")};
}

} // namespace

Json encode(const metadata::MetadataWritePlan& plan) {
    auto sources = encode_list(plan.sources, [](const metadata::MetadataWritePlanSource& source) {
        auto changes =
            encode_list(source.changes, [](const metadata::MetadataWritePlanChange& change) {
                auto intents = encode_list(
                    change.intents, [](const metadata::MetadataWritePlanIntent& intent) {
                        return Json{{"item_index", intent.item_index},
                                    {"kind", name_of(intent.kind, patch_kinds)},
                                    {"values", encode_texts(intent.values)}};
                    });
                return Json{
                    {"field_index", change.field_index},
                    {"canonical_name", encode_text(change.canonical_name)},
                    {"display_name", encode_text(change.display_name)},
                    {"native_name", encode_text(change.native_name)},
                    {"original_present", change.original_present},
                    {"original_values", encode_texts(change.original_values)},
                    {"intents", std::move(intents)},
                    {"conflicting_intents", change.conflicting_intents},
                    {"unresolved_non_embedded_target", change.unresolved_non_embedded_target},
                    {"exact_native_name", encode_optional_text(change.exact_native_name)}};
            });
        return Json{{"path", protocol::encode_raw_path(source.raw_path)},
                    {"occurrence_indexes", source.occurrence_indexes},
                    {"expected_revision", encode_optional_revision(source.expected_revision)},
                    {"observed_revision", encode_optional_revision(source.observed_revision)},
                    {"adapter_name", encode_text(source.adapter_name)},
                    {"changes", std::move(changes)},
                    {"issues", encode_list(source.issues, encode_issue)},
                    // Artwork is stage 4: marked, so a decoder refuses rather than
                    // silently writing a plan without it.
                    {"artwork", source.artwork != nullptr}};
    });
    auto sheets =
        encode_list(plan.cue_sheets, [](const metadata::MetadataWritePlanCueSheet& sheet) {
            auto tracks =
                encode_list(sheet.tracks, [](const metadata::MetadataWritePlanCueTrack& track) {
                    return Json{{"file_index", track.file_index},
                                {"track_index", track.track_index},
                                {"occurrence_indexes", track.occurrence_indexes},
                                {"fields", encode_list(track.fields, encode_loudness_field)}};
                });
            return Json{{"path", protocol::encode_raw_path(sheet.raw_cue_path)},
                        {"expected_revision", encode_optional_revision(sheet.expected_revision)},
                        {"observed_revision", encode_optional_revision(sheet.observed_revision)},
                        {"tracks", std::move(tracks)},
                        {"album_fields", encode_list(sheet.album_fields, encode_loudness_field)},
                        {"issues", encode_list(sheet.issues, encode_issue)}};
        });
    auto sidecars =
        encode_list(plan.sidecars, [](const metadata::MetadataWritePlanSidecar& sidecar) {
            auto entries = encode_list(
                sidecar.entries, [](const metadata::MetadataWritePlanSidecarEntry& entry) {
                    return Json{{"identity", encode_identity(entry.identity)},
                                {"occurrence_indexes", entry.occurrence_indexes},
                                {"fields", encode_list(entry.fields, encode_loudness_field)},
                                {"true_peak", entry.true_peak}};
                });
            return Json{{"path", protocol::encode_raw_path(sidecar.raw_audio_path)},
                        {"expected_revision", encode_optional_revision(sidecar.expected_revision)},
                        {"observed_revision", encode_optional_revision(sidecar.observed_revision)},
                        {"entries", std::move(entries)},
                        {"issues", encode_list(sidecar.issues, encode_issue)}};
        });
    return Json{{"sources", std::move(sources)},
                {"patch_count", plan.patch_count},
                {"cue_sheets", std::move(sheets)},
                {"sidecars", std::move(sidecars)}};
}

core::Result<metadata::MetadataWritePlan> decode_write_plan(const Json& value) {
    Reader in{value};
    metadata::MetadataWritePlan plan;
    plan.sources = in.list<metadata::MetadataWritePlanSource>("sources", [](Reader& source_in) {
        metadata::MetadataWritePlanSource source;
        source.raw_path = source_in.bytes("path");
        source.occurrence_indexes = source_in.indexes("occurrence_indexes");
        source.expected_revision = source_in.optional_revision("expected_revision");
        source.observed_revision = source_in.optional_revision("observed_revision");
        source.adapter_name = source_in.text("adapter_name");
        source.changes =
            source_in.list<metadata::MetadataWritePlanChange>("changes", [](Reader& change_in) {
                metadata::MetadataWritePlanChange change;
                change.field_index = change_in.index("field_index");
                change.canonical_name = change_in.text("canonical_name");
                change.display_name = change_in.text("display_name");
                change.native_name = change_in.text("native_name");
                change.original_present = change_in.flag("original_present");
                change.original_values = change_in.texts("original_values");
                change.intents = change_in.list<metadata::MetadataWritePlanIntent>(
                    "intents", [](Reader& intent_in) {
                        metadata::MetadataWritePlanIntent intent;
                        intent.item_index = intent_in.index("item_index");
                        intent.kind = intent_in.named("kind", patch_kinds);
                        intent.values = intent_in.texts("values");
                        return intent;
                    });
                change.conflicting_intents = change_in.flag("conflicting_intents");
                change.unresolved_non_embedded_target =
                    change_in.flag("unresolved_non_embedded_target");
                change.exact_native_name = change_in.optional_text("exact_native_name");
                return change;
            });
        source.issues = source_in.list<metadata::MetadataWritePlanIssue>("issues", read_issue);
        if (source_in.flag("artwork")) {
            source_in.fail("artwork is not written by the engine yet");
        }
        return source;
    });
    plan.patch_count = in.index("patch_count");
    plan.cue_sheets =
        in.list<metadata::MetadataWritePlanCueSheet>("cue_sheets", [](Reader& sheet_in) {
            metadata::MetadataWritePlanCueSheet sheet;
            sheet.raw_cue_path = sheet_in.bytes("path");
            sheet.expected_revision = sheet_in.optional_revision("expected_revision");
            sheet.observed_revision = sheet_in.optional_revision("observed_revision");
            sheet.tracks =
                sheet_in.list<metadata::MetadataWritePlanCueTrack>("tracks", [](Reader& track_in) {
                    metadata::MetadataWritePlanCueTrack track;
                    track.file_index = track_in.index("file_index");
                    track.track_index = track_in.index("track_index");
                    track.occurrence_indexes = track_in.indexes("occurrence_indexes");
                    track.fields = track_in.list<metadata::MetadataWritePlanLoudnessField>(
                        "fields", read_loudness_field);
                    return track;
                });
            sheet.album_fields = sheet_in.list<metadata::MetadataWritePlanLoudnessField>(
                "album_fields", read_loudness_field);
            sheet.issues = sheet_in.list<metadata::MetadataWritePlanIssue>("issues", read_issue);
            return sheet;
        });
    plan.sidecars = in.list<metadata::MetadataWritePlanSidecar>("sidecars", [](Reader& sidecar_in) {
        metadata::MetadataWritePlanSidecar sidecar;
        sidecar.raw_audio_path = sidecar_in.bytes("path");
        sidecar.expected_revision = sidecar_in.optional_revision("expected_revision");
        sidecar.observed_revision = sidecar_in.optional_revision("observed_revision");
        sidecar.entries = sidecar_in.list<metadata::MetadataWritePlanSidecarEntry>(
            "entries", [](Reader& entry_in) {
                metadata::MetadataWritePlanSidecarEntry entry;
                if (const auto* identity = entry_in.need("identity")) {
                    Reader identity_in{*identity};
                    entry.identity = read_identity(identity_in);
                    if (!identity_in.ok()) {
                        entry_in.adopt(identity_in.error());
                    }
                }
                entry.occurrence_indexes = entry_in.indexes("occurrence_indexes");
                entry.fields = entry_in.list<metadata::MetadataWritePlanLoudnessField>(
                    "fields", read_loudness_field);
                entry.true_peak = entry_in.flag("true_peak");
                return entry;
            });
        sidecar.issues = sidecar_in.list<metadata::MetadataWritePlanIssue>("issues", read_issue);
        return sidecar;
    });
    if (!in.ok()) {
        return std::unexpected(in.error());
    }
    return plan;
}

Json encode(const operations::MetadataApplyResult& result) {
    auto sources =
        encode_list(result.sources, [](const operations::MetadataApplySourceResult& source) {
            Json commit = nullptr;
            if (source.commit) {
                const auto& done = *source.commit;
                commit = Json{{"journal_id", done.journal_id.to_string()},
                              {"source_path", protocol::encode_raw_path(done.source_raw_path)},
                              {"backup_path", protocol::encode_raw_path(done.backup_raw_path)},
                              {"previous_revision", encode(done.previous_revision)},
                              {"published_revision", encode(done.published_revision)},
                              {"document", encode(done.document)},
                              {"occurrence_indexes", done.occurrence_indexes},
                              {"content_kind", name_of(done.content_kind, content_kinds)}};
            }
            return Json{{"source_index", source.source_index},
                        {"path", protocol::encode_raw_path(source.raw_path)},
                        {"state", name_of(source.state, apply_states)},
                        {"commit", std::move(commit)},
                        {"issue", encode_optional_error(source.issue)}};
        });
    auto sheets =
        encode_list(result.cue_sheets, [](const operations::CueReplayGainApplyOutcome& sheet) {
            Json commit = nullptr;
            if (sheet.commit) {
                const auto& done = *sheet.commit;
                auto tracks = encode_list(
                    done.tracks, [](const operations::CueReplayGainAppliedTrack& track) {
                        return Json{{"file_index", track.file_index},
                                    {"track_index", track.track_index},
                                    {"occurrence_indexes", track.occurrence_indexes},
                                    {"fields", encode_list(track.fields, encode_applied_field)}};
                    });
                commit =
                    Json{{"path", protocol::encode_raw_path(done.raw_cue_path)},
                         {"previous_revision", encode(done.previous_revision)},
                         {"published_revision", encode(done.published_revision)},
                         {"album_fields", encode_list(done.album_fields, encode_applied_field)},
                         {"tracks", std::move(tracks)}};
            }
            return Json{{"path", protocol::encode_raw_path(sheet.raw_cue_path)},
                        {"state", name_of(sheet.state, apply_states)},
                        {"commit", std::move(commit)},
                        {"issue", encode_optional_error(sheet.issue)}};
        });
    auto sidecars =
        encode_list(result.sidecars, [](const operations::LoudnessSidecarApplyOutcome& sidecar) {
            Json commit = nullptr;
            if (sidecar.commit) {
                const auto& done = *sidecar.commit;
                auto entries = encode_list(
                    done.entries, [](const operations::LoudnessSidecarAppliedEntry& entry) {
                        return Json{{"identity", encode_identity(entry.identity)},
                                    {"occurrence_indexes", entry.occurrence_indexes},
                                    {"fields", encode_list(entry.fields, encode_applied_field)}};
                    });
                commit = Json{{"path", protocol::encode_raw_path(done.raw_audio_path)},
                              {"sidecar_path", protocol::encode_raw_path(done.sidecar_raw_path)},
                              {"audio_revision", encode(done.audio_revision)},
                              {"sidecar_removed", done.sidecar_removed},
                              {"entries", std::move(entries)}};
            }
            return Json{{"path", protocol::encode_raw_path(sidecar.raw_audio_path)},
                        {"state", name_of(sidecar.state, apply_states)},
                        {"commit", std::move(commit)},
                        {"issue", encode_optional_error(sidecar.issue)}};
        });
    return Json{{"sources", std::move(sources)},
                {"cue_sheets", std::move(sheets)},
                {"sidecars", std::move(sidecars)},
                {"cancellation_requested", result.cancellation_requested}};
}

core::Result<operations::MetadataApplyResult> decode_apply_result(const Json& value) {
    Reader in{value};
    operations::MetadataApplyResult result;
    result.sources =
        in.list<operations::MetadataApplySourceResult>("sources", [](Reader& source_in) {
            operations::MetadataApplySourceResult source;
            source.source_index = source_in.index("source_index");
            source.raw_path = source_in.bytes("path");
            source.state = source_in.named("state", apply_states);
            source.commit = source_in.optional_object<operations::MetadataCommitResult>(
                "commit", [](Reader& commit_in) {
                    operations::MetadataCommitResult commit;
                    if (auto id = core::StableId::parse(commit_in.text("journal_id"))) {
                        commit.journal_id = *id;
                    } else {
                        commit_in.fail("journal_id is not an identity");
                    }
                    commit.source_raw_path = commit_in.bytes("source_path");
                    commit.backup_raw_path = commit_in.bytes("backup_path");
                    commit.previous_revision = commit_in.revision("previous_revision");
                    commit.published_revision = commit_in.revision("published_revision");
                    if (const auto* document = commit_in.need("document")) {
                        if (auto decoded = decode_document(*document)) {
                            commit.document = std::move(*decoded);
                        } else {
                            commit_in.adopt(decoded.error());
                        }
                    }
                    commit.occurrence_indexes = commit_in.indexes("occurrence_indexes");
                    commit.content_kind = commit_in.named("content_kind", content_kinds);
                    return commit;
                });
            source.issue = source_in.optional_error("issue");
            return source;
        });
    result.cue_sheets =
        in.list<operations::CueReplayGainApplyOutcome>("cue_sheets", [](Reader& sheet_in) {
            operations::CueReplayGainApplyOutcome sheet;
            sheet.raw_cue_path = sheet_in.bytes("path");
            sheet.state = sheet_in.named("state", apply_states);
            sheet.commit = sheet_in.optional_object<operations::CueReplayGainCommitResult>(
                "commit", [](Reader& commit_in) {
                    operations::CueReplayGainCommitResult commit;
                    commit.raw_cue_path = commit_in.bytes("path");
                    commit.previous_revision = commit_in.revision("previous_revision");
                    commit.published_revision = commit_in.revision("published_revision");
                    commit.album_fields = commit_in.list<operations::CueReplayGainAppliedField>(
                        "album_fields", read_applied_field);
                    commit.tracks = commit_in.list<operations::CueReplayGainAppliedTrack>(
                        "tracks", [](Reader& track_in) {
                            operations::CueReplayGainAppliedTrack track;
                            track.file_index = track_in.index("file_index");
                            track.track_index = track_in.index("track_index");
                            track.occurrence_indexes = track_in.indexes("occurrence_indexes");
                            track.fields = track_in.list<operations::CueReplayGainAppliedField>(
                                "fields", read_applied_field);
                            return track;
                        });
                    return commit;
                });
            sheet.issue = sheet_in.optional_error("issue");
            return sheet;
        });
    result.sidecars =
        in.list<operations::LoudnessSidecarApplyOutcome>("sidecars", [](Reader& sidecar_in) {
            operations::LoudnessSidecarApplyOutcome sidecar;
            sidecar.raw_audio_path = sidecar_in.bytes("path");
            sidecar.state = sidecar_in.named("state", apply_states);
            sidecar.commit = sidecar_in.optional_object<operations::LoudnessSidecarCommitResult>(
                "commit", [](Reader& commit_in) {
                    operations::LoudnessSidecarCommitResult commit;
                    commit.raw_audio_path = commit_in.bytes("path");
                    commit.sidecar_raw_path = commit_in.bytes("sidecar_path");
                    commit.audio_revision = commit_in.revision("audio_revision");
                    commit.sidecar_removed = commit_in.flag("sidecar_removed");
                    commit.entries = commit_in.list<operations::LoudnessSidecarAppliedEntry>(
                        "entries", [](Reader& entry_in) {
                            operations::LoudnessSidecarAppliedEntry entry;
                            if (const auto* identity = entry_in.need("identity")) {
                                Reader identity_in{*identity};
                                entry.identity = read_identity(identity_in);
                                if (!identity_in.ok()) {
                                    entry_in.adopt(identity_in.error());
                                }
                            }
                            entry.occurrence_indexes = entry_in.indexes("occurrence_indexes");
                            entry.fields = entry_in.list<operations::CueReplayGainAppliedField>(
                                "fields", read_applied_field);
                            return entry;
                        });
                    return commit;
                });
            sidecar.issue = sidecar_in.optional_error("issue");
            return sidecar;
        });
    result.cancellation_requested = in.flag("cancellation_requested");
    if (!in.ok()) {
        return std::unexpected(in.error());
    }
    return result;
}

Json encode(const operations::MetadataApplyProgress& progress) {
    return Json{{"source_index", progress.source_index},
                {"path", protocol::encode_raw_path(progress.raw_path)},
                {"state", name_of(progress.state, apply_states)},
                {"completed_sources", progress.completed_sources},
                {"total_sources", progress.total_sources},
                {"issue", encode_optional_error(progress.issue)}};
}

core::Result<operations::MetadataApplyProgress> decode_apply_progress(const Json& value) {
    Reader in{value};
    operations::MetadataApplyProgress progress;
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
