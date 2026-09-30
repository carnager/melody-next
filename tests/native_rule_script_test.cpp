// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/native_rule_script.hpp"

#include <iostream>
#include <span>
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

using namespace trackknife::metadata;

// Every typed step, with text that means something to the statement layer
// wherever it can go: what is exported reads back as exactly these steps.
void everyStepRoundTrips() {
    const std::vector<MetadataTransformationAction> actions{
        MetadataSetValuesAction{.target_field = "GENRE", .values = {"Rock, (live)", "50% $off\\"}},
        MetadataAddValuesAction{.target_field = "COMMENT", .values = {""}},
        MetadataRemoveFieldAction{.target_field = "ENCODER"},
        MetadataRemoveFieldAction{.target_field = "iTunNORM",
                                  .match_mode = MetadataFieldMatchMode::exact_native},
        MetadataRemoveFieldIfAction{.target_field = "DISCNUMBER",
                                    .dialect = {},
                                    .condition = "$eq(%totaldiscs%,1)"},
        MetadataRemoveFieldIfAction{.target_field = "TOTALDISCS",
                                    .dialect = {},
                                    .condition = "$eq(%totaldiscs%,1)",
                                    .match_mode = MetadataFieldMatchMode::exact_native},
        MetadataTransformValuesAction{.target_field = "TITLE",
                                      .transform = MetadataValueTransformKind::capitalize_first},
        MetadataFormatValueAction{.target_field = "ARTISTSORT",
                                  .dialect = {},
                                  .source = "%artist%, the $if(%a%,x,y) \\(live\\)"},
        MetadataCopyFieldAction{.target_field = "DATE", .source_field = "ORIGINALYEAR"},
        MetadataSplitValuesAction{.target_field = "ARTIST", .separator = ", "},
        MetadataJoinValuesAction{.target_field = "ARTIST", .separator = ""},
        MetadataRemoveMatchingValuesAction{.target_field = "GENRE", .match = "Other)"},
        MetadataReplaceMatchingValuesAction{.target_field = "GENRE",
                                            .match = "Rock",
                                            .replacement_values = {"Rock", "Pop,Rock"}},
        MetadataNumberSelectedItemsAction{.target_field = "TRACKNUMBER", .start = 1U, .padding = 2U},
        MetadataNumberGroupedItemsAction{.target_field = "TRACKNUMBER",
                                         .dialect = {},
                                         .group_expression = "%album%, %discnumber%",
                                         .start = 3U,
                                         .padding = 0U},
        MetadataKeepFirstCharactersAction{.target_field = "DATE", .character_count = 4U},
        MetadataCaptureValuesAction{.dialect = {},
                                    .source_kind = MetadataCaptureSourceKind::filename,
                                    .source = {},
                                    .pattern = "%tracknumber%. %title% (live, $1)"},
        MetadataCaptureValuesAction{.dialect = {},
                                    .source_kind = MetadataCaptureSourceKind::full_path,
                                    .source = {},
                                    .pattern = "%%/%album%/%%"},
        MetadataCaptureValuesAction{.dialect = {},
                                    .source_kind = MetadataCaptureSourceKind::formatted,
                                    .source = "%artist% - %title%",
                                    .pattern = "%artist% - %title%"},
        MetadataCaptureValuesAction{.dialect = {},
                                    .source_kind = MetadataCaptureSourceKind::field,
                                    .source = "COMMENT",
                                    .pattern = "Rated \\%%rating%"},
        MetadataBlocklistFieldsAction{.fields = {"ENCODER", "iTunSMPB"}},
        MetadataAllowlistFieldsAction{.fields = {"TITLE", "ARTIST", "ALBUM"}},
    };
    for (std::size_t index = 0U; index < actions.size(); ++index) {
        const auto one = export_native_rule_script(std::span{&actions[index], 1U});
        if (!one) {
            std::cerr << "step " << index << " does not round-trip\n";
            ++failures;
        }
    }
    const auto exported = export_native_rule_script(actions);
    CHECK(exported.has_value());
    if (!exported) {
        std::cerr << exported.error().message << '\n';
        return;
    }
    const auto imported = import_native_rule_script(*exported);
    CHECK(!imported.has_errors());
    CHECK(imported.actions == actions);
    for (const auto& diagnostic : imported.diagnostics) {
        std::cerr << diagnostic.line << ':' << diagnostic.column << ' ' << diagnostic.message
                  << '\n';
    }
}

// The rating conversion as a user writes it, spaced and on several lines.
void writtenScriptReads() {
    const auto imported = import_native_rule_script(
        "$copy(DATE,ORIGINALYEAR)\n"
        "$set(FMPS_RATING,$if($eq(%strangetag%,5),1.0,0.$mul(%strangetag%,2)))\n"
        "$if($not(%strangetag%),$delete(FMPS_RATING))\n"
        "  $DELETE( STRANGETAG )\n");
    CHECK(!imported.has_errors());
    CHECK(imported.actions.size() == 4U);
    if (imported.actions.size() != 4U) {
        return;
    }
    const auto* copy = std::get_if<MetadataCopyFieldAction>(&imported.actions[0]);
    CHECK(copy != nullptr && copy->target_field == "DATE" && copy->source_field == "ORIGINALYEAR");
    const auto* format = std::get_if<MetadataFormatValueAction>(&imported.actions[1]);
    CHECK(format != nullptr && format->target_field == "FMPS_RATING" &&
          format->source == "$if($eq(%strangetag%,5),1.0,0.$mul(%strangetag%,2))");
    const auto* conditional = std::get_if<MetadataRemoveFieldIfAction>(&imported.actions[2]);
    CHECK(conditional != nullptr && conditional->condition == "$not(%strangetag%)" &&
          conditional->match_mode == MetadataFieldMatchMode::logical);
    const auto* removal = std::get_if<MetadataRemoveFieldAction>(&imported.actions[3]);
    CHECK(removal != nullptr && removal->target_field == "STRANGETAG");
}

// One condition over several removals is one typed step each.
void conditionCoversSeveralRemovals() {
    const auto imported = import_native_rule_script(
        "$if($eq(%totaldiscs%,1),$delete(DISCNUMBER)$delete(TOTALDISCS))");
    CHECK(!imported.has_errors());
    CHECK(imported.actions.size() == 2U);
}

// A mistake is named where it is, and nothing is taken from a script with
// one.
void mistakesAreReportedInPlace() {
    const auto unknown = import_native_rule_script("$copy(DATE,YEAR)\n$frobnicate(X)");
    CHECK(unknown.has_errors());
    CHECK(unknown.actions.empty());
    CHECK(!unknown.diagnostics.empty() && unknown.diagnostics.front().line == 2U &&
          unknown.diagnostics.front().column == 1U);

    CHECK(import_native_rule_script("$set(TITLE)").has_errors());
    CHECK(import_native_rule_script("$set(TITLE,%artist)").has_errors());
    CHECK(import_native_rule_script("$set(TITLE,x").has_errors());
    CHECK(import_native_rule_script("TITLE=x").has_errors());
    CHECK(import_native_rule_script("").has_errors());
    CHECK(import_native_rule_script("$if(%a%,$set(B,c))").has_errors());
    CHECK(import_native_rule_script("$delete(A,sometimes)").has_errors());
    CHECK(import_native_rule_script("$number(TRACKNUMBER,one,2)").has_errors());
    CHECK(import_native_rule_script("$setvalues(GENRE,50%)").has_errors());
}

} // namespace

int main() {
    everyStepRoundTrips();
    writtenScriptReads();
    conditionCoversSeveralRemovals();
    mistakesAreReportedInPlace();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
