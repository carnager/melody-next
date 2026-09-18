// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/rule_script_import.hpp"
#include "trackknife/metadata/transformation.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <optional>
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

trackknife::metadata::MetadataField field(std::string name, std::vector<std::string> values) {
    return trackknife::metadata::MetadataField{
        .canonical_name = trackknife::metadata::canonicalize_field_name(name),
        .native_name = std::move(name),
        .values = std::move(values),
        .qualifier = {},
        .provenance = trackknife::metadata::FieldProvenance::embedded,
    };
}

trackknife::metadata::StagedMetadataSelection selection() {
    using trackknife::metadata::MetadataDocument;
    using trackknife::metadata::StagedMetadataSelection;
    using trackknife::metadata::StagedMetadataSource;
    const std::array<std::string_view, 6> preferred{"Title",   "Artist",  "Genre",
                                                    "Comment", "Summary", "Mood"};
    auto result = StagedMetadataSelection::create(
        {
            StagedMetadataSource{
                .raw_path = "/music/one.flac",
                .source_revision = std::nullopt,
                .baseline =
                    MetadataDocument{
                        .fields = {field("TITLE", {"  One  "}), field("ARTIST", {"Alpha", "Beta"}),
                                   field("COMMENT", {"Remove me"})},
                        .unsupported_native_objects = {},
                    },
            },
            StagedMetadataSource{
                .raw_path = "/music/two.flac",
                .source_revision = std::nullopt,
                .baseline =
                    MetadataDocument{
                        .fields = {field("TITLE", {"  Two  "}), field("ARTIST", {"Alpha", "Beta"})},
                        .unsupported_native_objects = {},
                    },
            },
        },
        preferred);
    CHECK(result.has_value());
    return result ? std::move(*result) : StagedMetadataSelection{};
}

void orderedChainsSeeEarlierActionsAndCurrentDraft() {
    using namespace trackknife::metadata;
    const auto baseline = selection();
    StagedMetadataPatchSet draft;
    const auto artist = *baseline.field_index("artist");
    CHECK(draft.replace_values(baseline, 0U, artist, {"Draft Artist"}).has_value());

    MetadataTransformationChain chain{
        .schema_version = 1U,
        .name = "Prepare display fields",
        .actions =
            {
                MetadataTransformValuesAction{.target_field = "Title",
                                              .transform = MetadataValueTransformKind::trim_ascii},
                MetadataTransformValuesAction{.target_field = "Title",
                                              .transform = MetadataValueTransformKind::uppercase},
                MetadataSetValuesAction{.target_field = "Genre", .values = {"Rock", "Alt"}},
                MetadataFormatValueAction{
                    .target_field = "Summary",
                    .dialect = {},
                    .source = "$join(artist, / ) — %title%",
                },
                MetadataRemoveFieldAction{.target_field = "Comment"},
            },
    };
    const std::array items{std::size_t{0U}, std::size_t{1U}};
    const auto preview = plan_metadata_transformation(baseline, draft, items, chain);
    if (!preview) {
        std::cerr << preview.error().message << '\n';
    }
    CHECK(preview.has_value());
    if (!preview) {
        return;
    }
    CHECK(preview->chain.schema_version == chain.schema_version);
    CHECK(preview->chain.name == chain.name);
    CHECK(preview->chain.actions.size() == chain.actions.size());
    CHECK(std::get_if<MetadataTransformValuesAction>(&preview->chain.actions[0]) != nullptr);
    CHECK(std::get_if<MetadataFormatValueAction>(&preview->chain.actions[3]) != nullptr);
    CHECK(preview->item_indexes == (std::vector<std::size_t>{0U, 1U}));
    CHECK(preview->changed_item_count == 2U);
    CHECK(preview->cells.size() == 7U);
    CHECK(preview->cells[0].item_index == 0U);
    CHECK(preview->cells[0].canonical_field == "title");
    CHECK(preview->cells[0].before == std::optional<std::vector<std::string>>{{"  One  "}});
    CHECK(preview->cells[0].after == std::optional<std::vector<std::string>>{{"ONE"}});
    CHECK(preview->cells[0].last_action_index == 1U);
    CHECK(preview->cells[1].canonical_field == "genre");
    CHECK(preview->cells[1].before == std::nullopt);
    CHECK(preview->cells[1].after == (std::optional<std::vector<std::string>>{{"Rock", "Alt"}}));
    CHECK(preview->cells[2].canonical_field == "summary");
    CHECK(preview->cells[2].after ==
          std::optional<std::vector<std::string>>{{"Draft Artist — ONE"}});
    CHECK(preview->cells[3].canonical_field == "comment");
    CHECK(preview->cells[3].after == std::nullopt);
    CHECK(preview->cells[6].canonical_field == "summary");
    CHECK(preview->cells[6].after ==
          std::optional<std::vector<std::string>>{{"Alpha / Beta — TWO"}});
    CHECK(draft.patch_count() == 1U);
    CHECK(draft.patch(0U, artist)->values == (std::vector<std::string>{"Draft Artist"}));
}

void exactAddCopySplitAndJoinPreserveOrderedState() {
    using namespace trackknife::metadata;
    const auto baseline = selection();
    const StagedMetadataPatchSet draft;
    const std::array items{std::size_t{0U}};
    const MetadataTransformationChain chain{
        .schema_version = 1U,
        .name = "Exact value operations",
        .actions =
            {
                MetadataAddValuesAction{.target_field = "Artist", .values = {"Alpha", ""}},
                MetadataCopyFieldAction{.target_field = "Credits", .source_field = "Artist"},
                MetadataSetValuesAction{.target_field = "Tags", .values = {"one||two", ""}},
                MetadataSplitValuesAction{.target_field = "Tags", .separator = "|"},
                MetadataAddValuesAction{.target_field = "Tags", .values = {"tail"}},
                MetadataJoinValuesAction{.target_field = "Tags", .separator = "::"},
                MetadataCopyFieldAction{.target_field = "Comment", .source_field = "Missing"},
                MetadataSetValuesAction{.target_field = "Mood",
                                        .values = {"élan", "two WORDS", ""}},
                MetadataTransformValuesAction{.target_field = "Mood",
                                              .transform =
                                                  MetadataValueTransformKind::capitalize_first},
            },
    };
    CHECK(validate_metadata_transformation_chain(chain).has_value());
    const auto preview = plan_metadata_transformation(baseline, draft, items, chain);
    CHECK(preview.has_value());
    if (!preview) {
        return;
    }
    CHECK(preview->cells.size() == 5U);
    CHECK(preview->cells[0].canonical_field == "artist");
    CHECK(preview->cells[0].after ==
          (std::optional<std::vector<std::string>>{{"Alpha", "Beta", "Alpha", ""}}));
    CHECK(preview->cells[1].canonical_field == "credits");
    CHECK(preview->cells[1].after ==
          (std::optional<std::vector<std::string>>{{"Alpha", "Beta", "Alpha", ""}}));
    CHECK(preview->cells[2].canonical_field == "tags");
    CHECK(preview->cells[2].after ==
          (std::optional<std::vector<std::string>>{{"one::::two::::tail"}}));
    CHECK(preview->cells[3].canonical_field == "comment");
    CHECK(preview->cells[3].before == (std::optional<std::vector<std::string>>{{"Remove me"}}));
    CHECK(preview->cells[3].after == std::nullopt);
    CHECK(preview->cells[4].canonical_field == "mood");
    CHECK(preview->cells[4].after ==
          (std::optional<std::vector<std::string>>{{"Élan", "Two WORDS", ""}}));
}

void fieldListsRemoveOnlyPreviewedMetadata() {
    using namespace trackknife::metadata;
    const auto baseline = selection();
    const StagedMetadataPatchSet draft;
    const std::array items{std::size_t{0U}, std::size_t{1U}};

    const MetadataTransformationChain blocklist{
        .schema_version = 1U,
        .name = "Remove unwanted fields",
        .actions = {MetadataBlocklistFieldsAction{.fields = {"Comment", "Encoder"}}},
    };
    const auto blocked = plan_metadata_transformation(baseline, draft, items, blocklist);
    CHECK(blocked.has_value());
    CHECK(blocked && blocked->cells.size() == 1U);
    CHECK(blocked && blocked->cells.front().canonical_field == "comment");
    CHECK(blocked && blocked->cells.front().after == std::nullopt);

    const MetadataTransformationChain allowlist{
        .schema_version = 1U,
        .name = "Keep portable fields",
        .actions = {MetadataAllowlistFieldsAction{.fields = {"TITLE"}}},
    };
    const auto allowed = plan_metadata_transformation(baseline, draft, items, allowlist);
    CHECK(allowed.has_value());
    CHECK(allowed && allowed->changed_item_count == 2U);
    CHECK(allowed && allowed->cells.size() == 3U);
    CHECK(allowed && std::ranges::all_of(allowed->cells, [](const auto& cell) {
              return cell.canonical_field != "title" && cell.after == std::nullopt;
          }));

    CHECK(!validate_metadata_transformation_chain(MetadataTransformationChain{
        .schema_version = 1U,
        .name = "Empty filter",
        .actions = {MetadataBlocklistFieldsAction{}},
    }));
}

void capitalizationNoOpCountsPresentAndMissingTargets() {
    using namespace trackknife::metadata;
    const auto baseline = selection();
    const StagedMetadataPatchSet draft;
    const std::array items{std::size_t{0U}, std::size_t{1U}};
    const MetadataTransformationChain chain{
        .schema_version = 1U,
        .name = "Already capitalized",
        .actions =
            {
                MetadataTransformValuesAction{.target_field = "Artist",
                                              .transform =
                                                  MetadataValueTransformKind::capitalize_first},
                MetadataTransformValuesAction{.target_field = "Comment",
                                              .transform =
                                                  MetadataValueTransformKind::capitalize_first},
            },
    };
    const auto preview = plan_metadata_transformation(baseline, draft, items, chain);
    CHECK(preview.has_value());
    if (!preview) {
        return;
    }
    CHECK(preview->cells.empty());
    CHECK(preview->changed_item_count == 0U);
    CHECK(preview->unchanged_present_cell_count == 3U);
    CHECK(preview->unchanged_missing_cell_count == 1U);
}

void keepFirstCharactersUsesUnicodeAndRetainsShortValues() {
    using namespace trackknife::metadata;
    auto selected = StagedMetadataSelection::create({
        StagedMetadataSource{
            .raw_path = "/music/date-one.flac",
            .source_revision = std::nullopt,
            .baseline =
                MetadataDocument{
                    .fields = {field("DATE", {"2024-08-30", "ééééé"})},
                    .unsupported_native_objects = {},
                },
        },
        StagedMetadataSource{
            .raw_path = "/music/date-two.flac",
            .source_revision = std::nullopt,
            .baseline =
                MetadataDocument{
                    .fields = {field("DATE", {"1999"})},
                    .unsupported_native_objects = {},
                },
        },
    });
    CHECK(selected.has_value());
    if (!selected) {
        return;
    }
    const StagedMetadataPatchSet draft;
    const std::array items{std::size_t{0U}, std::size_t{1U}};
    const MetadataTransformationChain chain{
        .schema_version = 1U,
        .name = "Keep year",
        .actions = {MetadataKeepFirstCharactersAction{
            .target_field = "Date",
            .character_count = 4U,
        }},
    };
    CHECK(validate_metadata_transformation_chain(chain).has_value());
    const auto preview = plan_metadata_transformation(*selected, draft, items, chain);
    CHECK(preview.has_value());
    if (!preview) {
        return;
    }
    CHECK(preview->changed_item_count == 1U);
    CHECK(preview->cells.size() == 1U);
    CHECK(preview->cells.front().before ==
          (std::optional<std::vector<std::string>>{{"2024-08-30", "ééééé"}}));
    CHECK(preview->cells.front().after ==
          (std::optional<std::vector<std::string>>{{"2024", "éééé"}}));
    CHECK(preview->unchanged_present_cell_count == 1U);
}

void pastedCleanupScriptGeneratesTypedPreviewedRules() {
    using namespace trackknife::metadata;
    const auto two_argument_if =
        import_metadata_rule_script("$if($not(%totaldiscs%),$unset(discnumber))");
    CHECK(!two_argument_if.has_errors());
    CHECK(two_argument_if.actions.size() == 1U);
    CHECK(two_argument_if.actions.size() == 1U &&
          std::get_if<MetadataRemoveFieldIfAction>(&two_argument_if.actions.front()) != nullptr);
    const auto* conditional_remove =
        std::get_if<MetadataRemoveFieldIfAction>(&two_argument_if.actions.front());
    CHECK(conditional_remove != nullptr &&
          conditional_remove->match_mode == MetadataFieldMatchMode::exact_native);

    const auto four_argument_if =
        import_metadata_rule_script("$if(%date%,$unset(one),$unset(two),$unset(three))");
    CHECK(four_argument_if.has_errors());
    CHECK(std::ranges::any_of(four_argument_if.diagnostics, [](const auto& diagnostic) {
        return diagnostic.message.find("accepts 2 or 3 arguments") != std::string::npos &&
               diagnostic.message.find("this call has 4") != std::string::npos;
    }));

    constexpr auto source = R"($delete(albumartist)
$delete(albumartistsort)
$delete(releasestatus)
$delete(releasetype)
$delete(asin)
$delete(language)
$delete(script)
$delete(catalognumber)
$delete(comment:)
$delete(barcode)
$delete(label)
$delete(media)
$delete(musicbrainz_discid)
$delete(totaltracks)
$delete(discid)
$delete(cddb_discid)

$if($or($not(%totaldiscs%),$eq(%totaldiscs%,1)),$delete(discnumber)$delete(totaldiscs))

$if(%originaldate%,$set(date,$left(%originaldate%,4))$set(originaldate,$left(%originaldate%,4)),$set(date,$left(%date%,4)))
)";
    const auto imported = import_metadata_rule_script(source);
    CHECK(!imported.has_errors());
    CHECK(imported.actions.size() == 20U);
    const auto* first_remove = std::get_if<MetadataRemoveFieldAction>(&imported.actions[0]);
    CHECK(first_remove != nullptr &&
          first_remove->match_mode == MetadataFieldMatchMode::exact_native);
    CHECK(std::get_if<MetadataRemoveFieldIfAction>(&imported.actions[16]) != nullptr);
    CHECK(std::get_if<MetadataRemoveFieldIfAction>(&imported.actions[17]) != nullptr);
    const auto* date = std::get_if<MetadataFormatValueAction>(&imported.actions[18]);
    CHECK(date != nullptr);
    CHECK(date != nullptr &&
          date->source == "$if(%originaldate%,$left(%originaldate%,4),$left(%date%,4))");
    const auto* original = std::get_if<MetadataKeepFirstCharactersAction>(&imported.actions[19]);
    CHECK(original != nullptr);
    CHECK(original != nullptr && original->target_field == "originaldate" &&
          original->character_count == 4U);
    CHECK(std::ranges::any_of(imported.diagnostics, [](const auto& diagnostic) {
        return diagnostic.severity == MetadataRuleScriptDiagnosticSeverity::warning &&
               diagnostic.message.find("default comment target") != std::string::npos;
    }));

    const auto exported = export_metadata_rule_script(imported.actions);
    CHECK(exported.has_value());
    const auto reimported =
        exported ? import_metadata_rule_script(*exported) : MetadataRuleScriptImportResult{};
    CHECK(exported && !reimported.has_errors());
    CHECK(exported && reimported.actions == imported.actions);
    const std::array typed_only{MetadataTransformationAction{
        MetadataAddValuesAction{.target_field = "genre", .values = {"Rock"}}}};
    const auto unsupported_export = export_metadata_rule_script(typed_only);
    CHECK(!unsupported_export);
    CHECK(!unsupported_export &&
          unsupported_export.error().code == trackknife::core::ErrorCode::unsupported);

    auto selected = StagedMetadataSelection::create({
        StagedMetadataSource{
            .raw_path = "/music/one.flac",
            .source_revision = std::nullopt,
            .baseline =
                MetadataDocument{
                    .fields = {field("COMMENT", {"remove"}), field("TOTALDISCS", {"1"}),
                               field("DISCNUMBER", {"1"}), field("DATE", {"2024-08-30"}),
                               field("ORIGINALDATE", {"1988-03-04"})},
                    .unsupported_native_objects = {},
                },
        },
        StagedMetadataSource{
            .raw_path = "/music/two.flac",
            .source_revision = std::nullopt,
            .baseline =
                MetadataDocument{
                    .fields = {field("TOTALDISCS", {"2"}), field("DISCNUMBER", {"2"}),
                               field("DATE", {"2001-07-09"})},
                    .unsupported_native_objects = {},
                },
        },
        StagedMetadataSource{
            .raw_path = "/music/three.flac",
            .source_revision = std::nullopt,
            .baseline =
                MetadataDocument{
                    .fields = {field("DISCNUMBER", {"1"}), field("DATE", {"1999"})},
                    .unsupported_native_objects = {},
                },
        },
    });
    CHECK(selected.has_value());
    if (!selected) {
        return;
    }
    const MetadataTransformationChain chain{
        .schema_version = 1U,
        .name = "Imported cleanup",
        .actions = imported.actions,
    };
    const StagedMetadataPatchSet draft;
    const std::array items{std::size_t{0U}, std::size_t{1U}, std::size_t{2U}};
    const auto preview = plan_metadata_transformation(*selected, draft, items, chain);
    CHECK(preview.has_value());
    if (!preview) {
        return;
    }
    const auto cell = [&preview](const std::size_t item, const std::string_view canonical) {
        return std::ranges::find_if(preview->cells, [item, canonical](const auto& candidate) {
            return candidate.item_index == item && candidate.canonical_field == canonical;
        });
    };
    CHECK(cell(0U, "comment") != preview->cells.end() && !cell(0U, "comment")->after);
    CHECK(cell(0U, "discnumber") != preview->cells.end() && !cell(0U, "discnumber")->after);
    CHECK(cell(0U, "totaldiscs") != preview->cells.end() && !cell(0U, "totaldiscs")->after);
    CHECK(cell(0U, "date") != preview->cells.end() &&
          cell(0U, "date")->after == std::optional<std::vector<std::string>>{{"1988"}});
    CHECK(cell(0U, "originaldate") != preview->cells.end() &&
          cell(0U, "originaldate")->after == std::optional<std::vector<std::string>>{{"1988"}});
    CHECK(cell(1U, "discnumber") == preview->cells.end());
    CHECK(cell(1U, "totaldiscs") == preview->cells.end());
    CHECK(cell(1U, "date") != preview->cells.end() &&
          cell(1U, "date")->after == std::optional<std::vector<std::string>>{{"2001"}});
    CHECK(cell(2U, "discnumber") != preview->cells.end() && !cell(2U, "discnumber")->after);

    const auto unsupported = import_metadata_rule_script("$rreplace(%title%,x,y)");
    CHECK(unsupported.has_errors());
    CHECK(unsupported.actions.empty());
    const auto unsafe_conditional_set =
        import_metadata_rule_script("$if(%foo%,$set(bar,$left(%bar%,4)))");
    CHECK(unsafe_conditional_set.has_errors());

    const auto recovered_punctuation = import_metadata_rule_script("$delete(foo),$delete(bar))");
    CHECK(!recovered_punctuation.has_errors());
    CHECK(recovered_punctuation.actions.size() == 2U);
    CHECK(recovered_punctuation.diagnostics.size() == 2U);
}

void importedDeleteKeepsSimilarConventionalAndFreeformFieldsSeparate() {
    using namespace trackknife::metadata;
    const MetadataDocument document{
        .fields =
            {
                MetadataField{.canonical_name = "albumartist",
                              .native_name = "ALBUMARTIST",
                              .values = {"Conventional"},
                              .qualifier = {},
                              .provenance = FieldProvenance::embedded},
                MetadataField{.canonical_name = "album artist",
                              .native_name = "ALBUM ARTIST",
                              .values = {"Legacy custom"},
                              .qualifier = {},
                              .provenance = FieldProvenance::embedded},
            },
        .unsupported_native_objects = {},
    };
    auto selection = StagedMetadataSelection::create({StagedMetadataSource{
        .raw_path = "/music/aliases.flac",
        .source_revision = std::nullopt,
        .baseline = document,
    }});
    CHECK(selection.has_value());
    if (!selection) {
        return;
    }
    const auto conventional = selection->field_index("albumartist");
    const auto legacy = selection->exact_native_field_index("album artist");
    CHECK(conventional.has_value());
    CHECK(legacy.has_value());
    CHECK(conventional != legacy);
    CHECK(conventional &&
          selection->cell(0U, *conventional)->values == std::vector<std::string>{"Conventional"});
    CHECK(legacy &&
          selection->cell(0U, *legacy)->values == std::vector<std::string>{"Legacy custom"});

    const auto imported = import_metadata_rule_script("$delete(album artist)");
    CHECK(!imported.has_errors());
    const MetadataTransformationChain chain{
        .schema_version = 1U,
        .name = "Remove legacy custom field",
        .actions = imported.actions,
    };
    const std::array items{std::size_t{0U}};
    const auto preview =
        plan_metadata_transformation(*selection, StagedMetadataPatchSet{}, items, chain);
    CHECK(preview.has_value());
    CHECK(preview && preview->cells.size() == 1U);
    CHECK(preview && preview->cells.front().match_mode == MetadataFieldMatchMode::exact_native);
    CHECK(preview && preview->cells.front().display_field == "album artist");
    CHECK(preview && preview->cells.front().before ==
                         std::optional<std::vector<std::string>>{{"Legacy custom"}});
    CHECK(preview && !preview->cells.front().after);

    const MetadataTransformationChain capture_chain{
        .schema_version = 1U,
        .name = "Capture separate identities",
        .actions = {MetadataCaptureValuesAction{
            .dialect = {},
            .source_kind = MetadataCaptureSourceKind::formatted,
            .source = "Captured semantic|Captured freeform",
            .pattern = "%albumartist%|%album artist%",
        }},
    };
    const auto capture_preview =
        plan_metadata_transformation(*selection, StagedMetadataPatchSet{}, items, capture_chain);
    CHECK(capture_preview.has_value());
    CHECK(capture_preview && capture_preview->cells.size() == 2U);
    if (capture_preview && capture_preview->cells.size() == 2U) {
        CHECK(capture_preview->cells[0].match_mode == MetadataFieldMatchMode::logical);
        CHECK(capture_preview->cells[0].after ==
              std::optional<std::vector<std::string>>{{"Captured semantic"}});
        CHECK(capture_preview->cells[1].match_mode == MetadataFieldMatchMode::exact_native);
        CHECK(capture_preview->cells[1].display_field == "album artist");
        CHECK(capture_preview->cells[1].after ==
              std::optional<std::vector<std::string>>{{"Captured freeform"}});
    }
}

void groupedNumberingRestartsPerGroupValue() {
    using trackknife::metadata::MetadataDocument;
    using trackknife::metadata::StagedMetadataSelection;
    using trackknife::metadata::StagedMetadataSource;
    // Two interleaved albums: counters are per evaluated group value, not
    // adjacency, so each album numbers 1..N in selection order.
    const auto source = [](const std::string& path, std::optional<std::string> album,
                           const std::string& title) {
        MetadataDocument document;
        if (album) {
            document.fields.push_back(field("ALBUM", {*album}));
        }
        document.fields.push_back(field("TITLE", {title}));
        return StagedMetadataSource{
            .raw_path = path,
            .source_revision = std::nullopt,
            .baseline = std::move(document),
        };
    };
    const auto selection = StagedMetadataSelection::create({
        source("/g/a1.flac", "Alpha", "a1"),
        source("/g/b1.flac", "Beta", "b1"),
        source("/g/a2.flac", "Alpha", "a2"),
        source("/g/b2.flac", "Beta", "b2"),
        source("/g/loose.flac", std::nullopt, "loose"),
    });
    CHECK(selection.has_value());
    if (!selection) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet patches;
    const std::array<std::size_t, 5> items{0U, 1U, 2U, 3U, 4U};
    trackknife::metadata::MetadataTransformationChain chain{
        .schema_version = 1U,
        .name = "grouped numbering",
        .actions = {trackknife::metadata::MetadataNumberGroupedItemsAction{
            .target_field = "Track Number",
            .dialect = {},
            .group_expression = "%album%",
            .start = 1U,
            .padding = 2U,
        }},
    };
    const auto preview =
        trackknife::metadata::plan_metadata_transformation(*selection, patches, items, chain);
    CHECK(preview.has_value());
    if (!preview) {
        std::cerr << preview.error().message << '\n';
        return;
    }
    const auto value_for = [&preview](const std::size_t item) -> std::string {
        for (const auto& cell : preview->cells) {
            if (cell.item_index == item && cell.canonical_field == "tracknumber" && cell.after) {
                return cell.after->front();
            }
        }
        return {};
    };
    CHECK(value_for(0U) == "01");
    CHECK(value_for(1U) == "01");
    CHECK(value_for(2U) == "02");
    CHECK(value_for(3U) == "02");
    // The file with no album shares the empty-group counter.
    CHECK(value_for(4U) == "01");

    // Broken group expressions fail typed before any item is touched.
    chain.actions = {trackknife::metadata::MetadataNumberGroupedItemsAction{
        .target_field = "Track Number",
        .dialect = {},
        .group_expression = "$unknown(%album%)",
        .start = 1U,
        .padding = 0U,
    }};
    CHECK(!trackknife::metadata::plan_metadata_transformation(*selection, patches, items, chain)
               .has_value());
}

void exactMatchingAndSelectionNumberingComposeInOrder() {
    using namespace trackknife::metadata;
    const auto baseline = selection();
    const StagedMetadataPatchSet draft;
    const std::array items{std::size_t{0U}, std::size_t{1U}};
    const MetadataTransformationChain chain{
        .schema_version = 1U,
        .name = "Match and number",
        .actions =
            {
                MetadataRemoveMatchingValuesAction{.target_field = "Artist", .match = "alpha"},
                MetadataRemoveMatchingValuesAction{.target_field = "Artist", .match = "Alpha"},
                MetadataRemoveMatchingValuesAction{.target_field = "Comment", .match = "Remove me"},
                MetadataReplaceMatchingValuesAction{.target_field = "Title",
                                                    .match = "  One  ",
                                                    .replacement_values = {"First", "Alternate"}},
                MetadataNumberSelectedItemsAction{
                    .target_field = "Track Number", .start = 7U, .padding = 2U},
                MetadataFormatValueAction{
                    .target_field = "Summary",
                    .dialect = {},
                    .source = "%track number%: $join(artist, / ) — $join(title, / )",
                },
            },
    };
    CHECK(validate_metadata_transformation_chain(chain).has_value());
    const auto preview = plan_metadata_transformation(baseline, draft, items, chain);
    if (!preview) {
        std::cerr << preview.error().message << '\n';
    }
    CHECK(preview.has_value());
    if (!preview) {
        return;
    }
    CHECK(preview->changed_item_count == 2U);
    CHECK(preview->cells.size() == 8U);
    CHECK(preview->cells[0].canonical_field == "artist");
    CHECK(preview->cells[0].after == std::optional<std::vector<std::string>>{{"Beta"}});
    CHECK(preview->cells[0].last_action_index == 1U);
    CHECK(preview->cells[1].canonical_field == "comment");
    CHECK(preview->cells[1].after == std::nullopt);
    CHECK(preview->cells[2].canonical_field == "title");
    CHECK(preview->cells[2].after ==
          (std::optional<std::vector<std::string>>{{"First", "Alternate"}}));
    CHECK(preview->cells[3].canonical_field == "tracknumber");
    CHECK(preview->cells[3].after == std::optional<std::vector<std::string>>{{"07"}});
    CHECK(preview->cells[4].canonical_field == "summary");
    CHECK(preview->cells[4].after ==
          std::optional<std::vector<std::string>>{{"07: Beta — First / Alternate"}});
    CHECK(preview->cells[5].canonical_field == "artist");
    CHECK(preview->cells[5].after == std::optional<std::vector<std::string>>{{"Beta"}});
    CHECK(preview->cells[6].canonical_field == "tracknumber");
    CHECK(preview->cells[6].after == std::optional<std::vector<std::string>>{{"08"}});
    CHECK(preview->cells[7].canonical_field == "summary");
    CHECK(preview->cells[7].after ==
          std::optional<std::vector<std::string>>{{"08: Beta —   Two  "}});
}

void capturePatternGrammarRejectsGuessingAndPreservesExactValues() {
    using namespace trackknife::metadata;
    const auto compiled = compile_capture_pattern("%artist% - %% - %title%");
    CHECK(compiled.has_value());
    if (!compiled) {
        return;
    }
    CHECK(compiled->named_capture_count == 2U);
    const auto matched = match_capture_pattern(*compiled, "Alpha - ignored - Song");
    CHECK(matched.has_value());
    CHECK(matched && matched->kind == CapturePatternMatchKind::unique);
    const std::vector<CapturedMetadataValue> expected{{.field = "artist", .value = "Alpha"},
                                                      {.field = "title", .value = "Song"}};
    CHECK(matched && matched->values == expected);

    const auto escaped = compile_capture_pattern("100\\%%title%");
    CHECK(escaped.has_value());
    if (!escaped) {
        return;
    }
    const auto escaped_match = match_capture_pattern(*escaped, "100%é");
    CHECK(escaped_match && escaped_match->kind == CapturePatternMatchKind::unique);
    CHECK(escaped_match && escaped_match->values.front().value == "é");

    const auto ambiguous = compile_capture_pattern("%artist% - %title%");
    CHECK(ambiguous.has_value());
    if (!ambiguous) {
        return;
    }
    const auto ambiguous_match = match_capture_pattern(*ambiguous, "A - B - C");
    CHECK(ambiguous_match && ambiguous_match->kind == CapturePatternMatchKind::ambiguous);
    const auto unmatched = match_capture_pattern(*ambiguous, "No delimiter");
    CHECK(unmatched && unmatched->kind == CapturePatternMatchKind::unmatched);

    const auto empty = compile_capture_pattern("%artist%--%title%");
    CHECK(empty.has_value());
    if (!empty) {
        return;
    }
    const auto empty_match = match_capture_pattern(*empty, "--Title");
    CHECK(empty_match && empty_match->kind == CapturePatternMatchKind::unique);
    CHECK(empty_match && empty_match->values.front().value.empty());
    CHECK(!compile_capture_pattern("literal only"));
    CHECK(!compile_capture_pattern("%unclosed"));
    CHECK(!compile_capture_pattern("%title%\\"));
    CHECK(!compile_capture_pattern("%one%tail", {},
                                   CapturePatternLimits{.pattern_bytes = 64U,
                                                        .source_bytes = 64U,
                                                        .tokens = 1U,
                                                        .captures = 1U,
                                                        .match_steps = 64U}));
    const auto bounded = compile_capture_pattern("%one%x");
    CHECK(bounded.has_value());
    if (bounded) {
        const auto limited = match_capture_pattern(*bounded, "aaaaaaaa",
                                                   CapturePatternLimits{.pattern_bytes = 64U,
                                                                        .source_bytes = 64U,
                                                                        .tokens = 8U,
                                                                        .captures = 8U,
                                                                        .match_steps = 2U});
        CHECK(!limited);
        CHECK(!limited && limited.error().code == trackknife::core::ErrorCode::limit_exceeded);
    }
}

void captureSourcesComposeAsOneMultiTargetTransformation() {
    using namespace trackknife::metadata;
    auto selected = StagedMetadataSelection::create({
        StagedMetadataSource{
            .raw_path = "/library/Alpha/Record [2024]/03. Song.flac",
            .source_revision = std::nullopt,
            .baseline =
                MetadataDocument{.fields = {field("CUSTOM SOURCE", {"Guest:One", "Guest:Two"})},
                                 .unsupported_native_objects = {}},
        },
    });
    CHECK(selected.has_value());
    if (!selected) {
        return;
    }
    const MetadataTransformationChain chain{
        .schema_version = 1U,
        .name = "Capture filename",
        .actions =
            {
                MetadataCaptureValuesAction{
                    .dialect = {},
                    .source_kind = MetadataCaptureSourceKind::filename,
                    .source = {},
                    .pattern = "%artist%/%album% [%date%]/%tracknumber%. %title%",
                },
                MetadataCaptureValuesAction{
                    .dialect = {},
                    .source_kind = MetadataCaptureSourceKind::full_path,
                    .source = {},
                    .pattern = "/library/%pathartist%/%pathalbum% [%pathdate%]/%pathtrack%. "
                               "%pathtitle%.flac",
                },
                MetadataCaptureValuesAction{
                    .dialect = {},
                    .source_kind = MetadataCaptureSourceKind::formatted,
                    .source = "%artist% | %title%",
                    .pattern = "%formattedartist% | %formattedtitle%",
                },
                MetadataCaptureValuesAction{
                    .dialect = {},
                    .source_kind = MetadataCaptureSourceKind::formatted,
                    .source = "%artist% | %title%",
                    .pattern = "%alias% | %alias%",
                },
                MetadataFormatValueAction{
                    .target_field = "Summary", .dialect = {}, .source = "%tracknumber%: %title%"},
                MetadataCaptureValuesAction{
                    .dialect = {},
                    .source_kind = MetadataCaptureSourceKind::field,
                    .source = "CUSTOM SOURCE",
                    .pattern = "%%:%guest%",
                },
                MetadataCaptureValuesAction{
                    .dialect = {},
                    .source_kind = MetadataCaptureSourceKind::field,
                    .source = "guest",
                    .pattern = "%nextguest%",
                },
            },
    };
    CHECK(validate_metadata_transformation_chain(chain).has_value());
    const std::array items{std::size_t{0U}};
    const auto preview =
        plan_metadata_transformation(*selected, StagedMetadataPatchSet{}, items, chain);
    if (!preview) {
        std::cerr << preview.error().message << '\n';
    }
    CHECK(preview.has_value());
    if (!preview) {
        return;
    }
    const auto value = [&preview](const std::string_view field_name) {
        const auto found = std::ranges::find_if(
            preview->cells, [&](const auto& cell) { return cell.canonical_field == field_name; });
        return found == preview->cells.end() ? std::optional<std::vector<std::string>>{}
                                             : found->after;
    };
    CHECK(value("artist") == std::optional<std::vector<std::string>>{{"Alpha"}});
    CHECK(value("album") == std::optional<std::vector<std::string>>{{"Record"}});
    CHECK(value("date") == std::optional<std::vector<std::string>>{{"2024"}});
    CHECK(value("tracknumber") == std::optional<std::vector<std::string>>{{"03"}});
    CHECK(value("title") == std::optional<std::vector<std::string>>{{"Song"}});
    CHECK(value("pathartist") == std::optional<std::vector<std::string>>{{"Alpha"}});
    CHECK(value("pathtitle") == std::optional<std::vector<std::string>>{{"Song"}});
    CHECK(value("formattedartist") == std::optional<std::vector<std::string>>{{"Alpha"}});
    CHECK(value("formattedtitle") == std::optional<std::vector<std::string>>{{"Song"}});
    CHECK(value("alias") == (std::optional<std::vector<std::string>>{{"Alpha", "Song"}}));
    CHECK(value("summary") == std::optional<std::vector<std::string>>{{"03: Song"}});
    CHECK(value("guest") == (std::optional<std::vector<std::string>>{{"One", "Two"}}));
    CHECK(value("nextguest") == (std::optional<std::vector<std::string>>{{"One", "Two"}}));

    auto ambiguous_chain = chain;
    ambiguous_chain.actions = {MetadataCaptureValuesAction{
        .dialect = {},
        .source_kind = MetadataCaptureSourceKind::full_path,
        .source = {},
        .pattern = "%prefix%/%middle%/%title%",
    }};
    const auto ambiguous =
        plan_metadata_transformation(*selected, StagedMetadataPatchSet{}, items, ambiguous_chain);
    CHECK(!ambiguous);
    CHECK(!ambiguous && ambiguous.error().code == trackknife::core::ErrorCode::conflict);
}

void plansRejectInvalidDialectInputLimitsAndCancellation() {
    using namespace trackknife;
    const auto baseline = selection();
    const metadata::StagedMetadataPatchSet draft;
    const std::array items{std::size_t{0U}, std::size_t{1U}};

    const auto empty = metadata::plan_metadata_transformation(
        baseline, draft, items,
        metadata::MetadataTransformationChain{.schema_version = 1U, .name = {}, .actions = {}});
    CHECK(!empty);
    CHECK(empty.error().code == core::ErrorCode::invalid_argument);

    auto invalid_expression = metadata::MetadataTransformationChain{
        .schema_version = 1U,
        .name = {},
        .actions = {metadata::MetadataFormatValueAction{
            .target_field = "Title", .dialect = {}, .source = "$unknown(%title%)"}},
    };
    const auto invalid =
        metadata::plan_metadata_transformation(baseline, draft, items, invalid_expression);
    CHECK(!invalid);
    CHECK(invalid.error().message.find("unknown format function") != std::string::npos);

    auto* format_action =
        std::get_if<metadata::MetadataFormatValueAction>(&invalid_expression.actions.front());
    CHECK(format_action != nullptr);
    if (format_action == nullptr) {
        return;
    }
    format_action->dialect.dialect_version = 2U;
    const auto dialect =
        metadata::plan_metadata_transformation(baseline, draft, items, invalid_expression);
    CHECK(!dialect);
    CHECK(dialect.error().message.find("unsupported dialect") != std::string::npos);

    const auto invalid_split =
        metadata::validate_metadata_transformation_chain(metadata::MetadataTransformationChain{
            .schema_version = 1U,
            .name = "Invalid split",
            .actions = {metadata::MetadataSplitValuesAction{.target_field = "Genre",
                                                            .separator = {}}},
        });
    CHECK(!invalid_split);
    CHECK(invalid_split.error().message.find("non-empty separator") != std::string::npos);

    const auto invalid_copy =
        metadata::validate_metadata_transformation_chain(metadata::MetadataTransformationChain{
            .schema_version = 1U,
            .name = "Invalid copy",
            .actions = {metadata::MetadataCopyFieldAction{.target_field = "Genre",
                                                          .source_field = {}}},
        });
    CHECK(!invalid_copy);

    const auto invalid_replacement =
        metadata::validate_metadata_transformation_chain(metadata::MetadataTransformationChain{
            .schema_version = 1U,
            .name = "Invalid replacement",
            .actions = {metadata::MetadataReplaceMatchingValuesAction{
                .target_field = "Genre", .match = "Rock", .replacement_values = {}}},
        });
    CHECK(!invalid_replacement);

    const auto invalid_number_start =
        metadata::validate_metadata_transformation_chain(metadata::MetadataTransformationChain{
            .schema_version = 1U,
            .name = "Invalid number start",
            .actions = {metadata::MetadataNumberSelectedItemsAction{
                .target_field = "Track Number", .start = 0U, .padding = 2U}},
        });
    CHECK(!invalid_number_start);
    const auto invalid_number_padding =
        metadata::validate_metadata_transformation_chain(metadata::MetadataTransformationChain{
            .schema_version = 1U,
            .name = "Invalid number padding",
            .actions = {metadata::MetadataNumberSelectedItemsAction{
                .target_field = "Track Number", .start = 1U, .padding = 33U}},
        });
    CHECK(!invalid_number_padding);

    const auto invalid_keep_count =
        metadata::validate_metadata_transformation_chain(metadata::MetadataTransformationChain{
            .schema_version = 1U,
            .name = "Invalid keep count",
            .actions = {metadata::MetadataKeepFirstCharactersAction{.target_field = "Date",
                                                                    .character_count = 0U}},
        });
    CHECK(!invalid_keep_count);

    const auto invalid_condition =
        metadata::validate_metadata_transformation_chain(metadata::MetadataTransformationChain{
            .schema_version = 1U,
            .name = "Invalid condition",
            .actions = {metadata::MetadataRemoveFieldIfAction{
                .target_field = "Disc Number",
                .dialect = trackknife::titleformat::DialectVersion{},
                .condition = "$unknown()"}},
        });
    CHECK(!invalid_condition);

    const auto invalid_capture =
        metadata::validate_metadata_transformation_chain(metadata::MetadataTransformationChain{
            .schema_version = 1U,
            .name = "Invalid capture",
            .actions = {metadata::MetadataCaptureValuesAction{
                .dialect = {},
                .source_kind = metadata::MetadataCaptureSourceKind::filename,
                .source = "unexpected",
                .pattern = "%title%",
            }},
        });
    CHECK(!invalid_capture);
    auto wrong_capture_dialect = metadata::MetadataTransformationChain{
        .schema_version = 1U,
        .name = "Invalid capture dialect",
        .actions = {metadata::MetadataCaptureValuesAction{
            .dialect = {},
            .source_kind = metadata::MetadataCaptureSourceKind::filename,
            .source = {},
            .pattern = "%title%",
        }},
    };
    auto* wrong_capture =
        std::get_if<metadata::MetadataCaptureValuesAction>(&wrong_capture_dialect.actions.front());
    CHECK(wrong_capture != nullptr);
    if (wrong_capture == nullptr) {
        return;
    }
    wrong_capture->dialect.dialect_version = 2U;
    CHECK(!metadata::validate_metadata_transformation_chain(wrong_capture_dialect));

    const metadata::MetadataTransformationChain bounded{
        .schema_version = 1U,
        .name = {},
        .actions = {metadata::MetadataSetValuesAction{.target_field = "Genre", .values = {"Rock"}}},
    };
    const auto limited = metadata::plan_metadata_transformation(
        baseline, draft, items, bounded, {},
        metadata::MetadataTransformationLimits{.items = 2U,
                                               .actions = 1U,
                                               .addressed_cells = 1U,
                                               .values_per_cell = 1U,
                                               .total_preview_text_bytes = 64U,
                                               .field_name_bytes = 64U,
                                               .chain_name_bytes = 64U,
                                               .action_text_bytes = 64U});
    CHECK(!limited);
    CHECK(limited.error().code == core::ErrorCode::limit_exceeded);

    core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto cancelled = metadata::plan_metadata_transformation(baseline, draft, items, bounded,
                                                                  cancellation.token());
    CHECK(!cancelled);
    CHECK(cancelled.error().code == core::ErrorCode::cancelled);
}

} // namespace

int main() {
    orderedChainsSeeEarlierActionsAndCurrentDraft();
    exactAddCopySplitAndJoinPreserveOrderedState();
    fieldListsRemoveOnlyPreviewedMetadata();
    capitalizationNoOpCountsPresentAndMissingTargets();
    keepFirstCharactersUsesUnicodeAndRetainsShortValues();
    pastedCleanupScriptGeneratesTypedPreviewedRules();
    importedDeleteKeepsSimilarConventionalAndFreeformFieldsSeparate();
    exactMatchingAndSelectionNumberingComposeInOrder();
    groupedNumberingRestartsPerGroupValue();
    capturePatternGrammarRejectsGuessingAndPreservesExactValues();
    captureSourcesComposeAsOneMultiTargetTransformation();
    plansRejectInvalidDialectInputLimitsAndCancellation();
    return failures == 0 ? 0 : 1;
}
