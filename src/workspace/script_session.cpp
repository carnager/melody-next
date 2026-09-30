// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/script_session.hpp"

#include "bench/metadata_dialog_helpers.hpp"
#include "bench/metadata_transformation_preview_model.hpp"
#include "trackknife/metadata/native_rule_script.hpp"
#include "uicommon/metadata_transformation_interchange.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <iterator>
#include <type_traits>
#include <utility>

namespace trackknife::bench {

ScriptSession::ScriptSession(std::shared_ptr<const metadata::StagedMetadataSelection> selection,
                             metadata::StagedMetadataPatchSet draft,
                             std::vector<std::size_t> item_indexes, QStringList track_labels,
                             StageCallback stage, MetadataTransformationStore store,
                             const std::optional<core::StableId> initially_selected,
                             const bool preview_initially_selected, QObject* parent)
    : QObject(parent), selection_(std::move(selection)), draft_(std::move(draft)),
      item_indexes_(std::move(item_indexes)), track_labels_(std::move(track_labels)),
      stage_(std::move(stage)), store_(std::move(store)), initially_selected_(initially_selected),
      preview_initially_selected_(preview_initially_selected) {
    target_field_candidates_.reserve(selection_->field_count());
    for (std::size_t index = 0U; index < selection_->field_count(); ++index) {
        const auto& field = selection_->field(index);
        if (field.present_item_count > 0U) {
            target_field_candidates_.push_back(metadata::MetadataFieldSuggestionCandidate{
                .display_name = field.display_name,
                .kind = metadata::MetadataFieldSuggestionKind::present,
            });
        }
    }
    // ADR-0178 field filters complete each comma-separated name from the
    // selection's present fields plus the standard conventional and
    // MusicBrainz catalog; any custom name stays freely typable.
    fields_candidates_ = target_field_candidates_;
    for (const auto& candidate : metadata::metadata_field_suggestion_catalog()) {
        fields_candidates_.push_back(candidate);
    }
    preview_timer_.setSingleShot(true);
    preview_timer_.setInterval(400);
    connect(&preview_timer_, &QTimer::timeout, this, &ScriptSession::startPreview);
    connect(&watcher_, &QFutureWatcherBase::finished, this, &ScriptSession::finishPreview);
    clean_chain_ = currentChain();
    refreshRawFromActions();
    loadSaved();
}

ScriptSession::~ScriptSession() {
    cancellation_.request_cancellation();
    if (planning_) {
        watcher_.waitForFinished();
    }
}

const std::vector<ScriptSession::StepKind>& ScriptSession::stepKinds() {
    // Kinds are grouped under unselectable headings; the row therefore does
    // not match the action kind, which each row carries.
    static const std::vector<StepKind> kinds{
        {QStringLiteral("Set values"), -1, {}},
        {QStringLiteral("Set one literal value"), 0, {}},
        {QStringLiteral("Add one literal value"), 1, {}},
        {QStringLiteral("Copy another field"), 7, {}},
        {QStringLiteral("Format with tkfmt-1"), 10,
         QStringLiteral("Build the value from an expression, for example %artist% — %title%")},
        {QStringLiteral("Number by selected-file order"), 13, {}},
        {QStringLiteral("Convert rating to FMPS_RATING"), 20,
         QStringLiteral("Take a rating another player wrote into Trackknife's rating tag, "
                        "converted from its scale -- 4 of 5 stars becomes 0.8")},
        {QStringLiteral("Capture fields with tkcapture-1"), 16,
         QStringLiteral("Extract several fields at once from the filename, the full path, "
                        "formatted text, or another field")},
        {QStringLiteral("Clean up values"), -1, {}},
        {QStringLiteral("Trim each value"), 3, {}},
        {QStringLiteral("Lowercase each value"), 4, {}},
        {QStringLiteral("Uppercase each value"), 5, {}},
        {QStringLiteral("Capitalize first character"), 6, {}},
        {QStringLiteral("Keep first characters of each value"), 14, {}},
        {QStringLiteral("Split & join"), -1, {}},
        {QStringLiteral("Split by exact separator"), 8, {}},
        {QStringLiteral("Join with exact separator"), 9, {}},
        {QStringLiteral("Remove & replace"), -1, {}},
        {QStringLiteral("Remove field"), 2, {}},
        {QStringLiteral("Remove field when condition matches"), 15,
         QStringLiteral("The field is removed when the expression is non-empty, for example "
                        "$not(%totaldiscs%)")},
        {QStringLiteral("Remove exact matching values"), 11, {}},
        {QStringLiteral("Replace exact matching values"), 12, {}},
        {QStringLiteral("Remove listed fields (blocklist)"), 18,
         QStringLiteral("Remove every named field from the selected files")},
        {QStringLiteral("Keep only listed fields (allowlist)"), 19,
         QStringLiteral("Remove every field except the named fields")},
    };
    return kinds;
}

QStringList ScriptSession::ratingScales() {
    return {QStringLiteral("0–5 stars"), QStringLiteral("0–10"), QStringLiteral("0–100")};
}

QStringList ScriptSession::captureSources() {
    return {QStringLiteral("Filename and requested parent folders"), QStringLiteral("Full path"),
            QStringLiteral("Formatted tkfmt-1 text"), QStringLiteral("Metadata field values")};
}

ScriptSession::StepForm ScriptSession::stepForm(const int kind, const int capture_source) {
    StepForm form;
    const auto captures = kind == 16;
    const auto filters_fields = kind == 18 || kind == 19;
    const auto rating = kind == 20;
    // A rating always goes to FMPS_RATING: there is no target to choose.
    form.target = !captures && !filters_fields && !rating;
    form.rating_scale = rating;
    form.input = kind == 0 || kind == 1 || (kind >= 7 && kind <= 12) || kind == 15 || captures ||
                 filters_fields || rating;
    form.replacement = kind == 12;
    form.numbering = kind == 13;
    form.characters = kind == 14;
    form.capture_source = captures;
    form.capture_argument = captures && capture_source >= 2;
    form.field_list = filters_fields;
    form.capture_argument_label = QStringLiteral("Source expression:");
    if (kind == 7) {
        form.input_label = QStringLiteral("Source field:");
        form.input_placeholder = QStringLiteral("For example: Artist");
    } else if (kind == 8 || kind == 9) {
        form.input_label = QStringLiteral("Separator:");
        form.input_placeholder =
            kind == 8 ? QStringLiteral("Required exact separator") : QStringLiteral("May be empty");
    } else if (kind == 10) {
        form.input_label = QStringLiteral("Expression:");
        form.input_placeholder = QStringLiteral("For example: %artist% — %title%");
    } else if (kind == 11 || kind == 12) {
        form.input_label = QStringLiteral("Exact value:");
        form.input_placeholder = QStringLiteral("Case-sensitive; may be empty");
    } else if (rating) {
        form.input_label = QStringLiteral("Rating field:");
        form.input_placeholder = QStringLiteral("For example: RATING");
    } else if (kind == 15) {
        form.input_label = QStringLiteral("Condition:");
        form.input_placeholder = QStringLiteral("For example: $not(%totaldiscs%)");
    } else if (captures) {
        form.input_label = QStringLiteral("Capture pattern:");
        form.input_placeholder = QStringLiteral("For example: %tracknumber%. %title%");
        const auto from_field = capture_source == 3;
        form.capture_argument_label =
            from_field ? QStringLiteral("Source field:") : QStringLiteral("Source expression:");
        form.capture_argument_placeholder = from_field
                                                ? QStringLiteral("For example: Comment")
                                                : QStringLiteral("For example: %artist% — %title%");
    } else if (filters_fields) {
        form.input_label = QStringLiteral("Fields:");
        form.input_placeholder = QStringLiteral("Comma-separated, for example: Comment, Encoder");
    } else {
        form.input_label = QStringLiteral("Value:");
    }
    return form;
}

ScriptSession::RuleScript ScriptSession::translateRuleScript(const QString& source) {
    auto result = metadata::import_metadata_rule_script(encode_utf8(source));
    QStringList lines;
    for (const auto& diagnostic : result.diagnostics) {
        const auto severity =
            diagnostic.severity == metadata::MetadataRuleScriptDiagnosticSeverity::error
                ? QStringLiteral("Error")
                : QStringLiteral("Warning");
        lines.push_back(QStringLiteral("%1 · line %2, column %3 · %4")
                            .arg(severity)
                            .arg(diagnostic.line)
                            .arg(diagnostic.column)
                            .arg(display_utf8(diagnostic.message)));
    }
    if (lines.isEmpty()) {
        lines.push_back(
            result.actions.empty()
                ? QStringLiteral("Paste a script to inspect generated rules.")
                : QStringLiteral("Ready · %1 generated rules").arg(result.actions.size()));
    } else if (!result.has_errors()) {
        lines.prepend(
            QStringLiteral("Ready · %1 generated rules with warnings").arg(result.actions.size()));
    }
    const auto ready = !result.has_errors() && !result.actions.empty();
    return RuleScript{.actions = std::move(result.actions),
                      .diagnostics = lines.join(QChar{'\n'}),
                      .ready = ready};
}

// What is shown.

QStringList ScriptSession::savedNames() const {
    QStringList names{QStringLiteral("New script")};
    for (const auto& entry : catalog_) {
        names.push_back(display_utf8(entry.chain.name));
    }
    return names;
}

int ScriptSession::savedIndex() const {
    if (!selected_saved_) {
        return 0;
    }
    const auto found = std::ranges::find(catalog_, *selected_saved_,
                                         &persistence::SavedMetadataTransformationChain::id);
    return found == catalog_.end() ? 0
                                   : static_cast<int>(std::distance(catalog_.begin(), found)) + 1;
}

QStringList ScriptSession::steps() const {
    QStringList texts;
    for (std::size_t index = 0U; index < actions_.size(); ++index) {
        texts.push_back(actionText(actions_[index], index));
    }
    return texts;
}

QStringList ScriptSession::targetSuggestions(const QString& query) const {
    const auto encoded = query.toUtf8();
    const auto suggestions = metadata::suggest_metadata_field_names(
        std::string_view{encoded.constData(), static_cast<std::size_t>(encoded.size())},
        target_field_candidates_);
    QStringList names;
    names.reserve(static_cast<qsizetype>(suggestions.size()));
    for (const auto& suggestion : suggestions) {
        names.push_back(display_utf8(suggestion.display_name));
    }
    return names;
}

QStringList ScriptSession::fieldListSuggestions(const QString& text) const {
    const auto separator = text.lastIndexOf(QLatin1Char(','));
    const auto token = text.mid(separator + 1).trimmed();
    const auto encoded = token.toUtf8();
    const auto suggestions = metadata::suggest_metadata_field_names(
        std::string_view{encoded.constData(), static_cast<std::size_t>(encoded.size())},
        fields_candidates_);
    QStringList names;
    names.reserve(static_cast<qsizetype>(suggestions.size()));
    for (const auto& suggestion : suggestions) {
        names.push_back(display_utf8(suggestion.display_name));
    }
    return names;
}

QString ScriptSession::completeFieldList(const QString& text, const QString& name) {
    const auto separator = text.lastIndexOf(QLatin1Char(','));
    auto prefix = separator < 0 ? QString{} : text.left(separator + 1) + QLatin1Char(' ');
    prefix.replace(QStringLiteral(",  "), QStringLiteral(", "));
    return prefix + name;
}

QString ScriptSession::suggestedExportName() const {
    auto suggested = name_.trimmed();
    suggested.replace(QChar{'/'}, QChar{'_'});
    if (suggested.isEmpty()) {
        suggested = QStringLiteral("tagging-script");
    }
    return suggested + QStringLiteral(".tbtags.json");
}

QString ScriptSession::actionText(const metadata::MetadataTransformationAction& action,
                                  const std::size_t index) const {
    return std::visit(
        [index](const auto& typed) {
            using Action = std::decay_t<decltype(typed)>;
            const auto field = [&] {
                if constexpr (std::is_same_v<Action, metadata::MetadataCaptureValuesAction> ||
                              std::is_same_v<Action, metadata::MetadataBlocklistFieldsAction> ||
                              std::is_same_v<Action, metadata::MetadataAllowlistFieldsAction>) {
                    return QString{};
                } else {
                    return display_utf8(typed.target_field);
                }
            }();
            if constexpr (std::is_same_v<Action, metadata::MetadataSetValuesAction>) {
                return QStringLiteral("%1. Set %2 to %3")
                    .arg(index + 1U)
                    .arg(field, display_plan_values(typed.values));
            } else if constexpr (std::is_same_v<Action, metadata::MetadataAddValuesAction>) {
                return QStringLiteral("%1. Add %3 to %2")
                    .arg(index + 1U)
                    .arg(field, display_plan_values(typed.values));
            } else if constexpr (std::is_same_v<Action, metadata::MetadataRemoveFieldAction>) {
                return typed.match_mode == metadata::MetadataFieldMatchMode::exact_native
                           ? QStringLiteral("%1. Remove exact native field %2")
                                 .arg(index + 1U)
                                 .arg(field)
                           : QStringLiteral("%1. Remove %2").arg(index + 1U).arg(field);
            } else if constexpr (std::is_same_v<Action, metadata::MetadataRemoveFieldIfAction>) {
                return typed.match_mode == metadata::MetadataFieldMatchMode::exact_native
                           ? QStringLiteral("%1. Remove exact native field %2 when %3")
                                 .arg(index + 1U)
                                 .arg(field, display_utf8(typed.condition))
                           : QStringLiteral("%1. Remove %2 when %3")
                                 .arg(index + 1U)
                                 .arg(field, display_utf8(typed.condition));
            } else if constexpr (std::is_same_v<Action, metadata::MetadataBlocklistFieldsAction>) {
                QStringList fields;
                for (const auto& name : typed.fields) {
                    fields.push_back(display_utf8(name));
                }
                return QStringLiteral("%1. Remove listed fields: %2")
                    .arg(index + 1U)
                    .arg(fields.join(QStringLiteral(", ")));
            } else if constexpr (std::is_same_v<Action, metadata::MetadataAllowlistFieldsAction>) {
                QStringList fields;
                for (const auto& name : typed.fields) {
                    fields.push_back(display_utf8(name));
                }
                return QStringLiteral("%1. Keep only listed fields: %2")
                    .arg(index + 1U)
                    .arg(fields.join(QStringLiteral(", ")));
            } else if constexpr (std::is_same_v<Action, metadata::MetadataTransformValuesAction>) {
                QString verb;
                switch (typed.transform) {
                case metadata::MetadataValueTransformKind::trim_ascii:
                    verb = QStringLiteral("Trim each value of");
                    break;
                case metadata::MetadataValueTransformKind::lowercase:
                    verb = QStringLiteral("Lowercase each value of");
                    break;
                case metadata::MetadataValueTransformKind::uppercase:
                    verb = QStringLiteral("Uppercase each value of");
                    break;
                case metadata::MetadataValueTransformKind::capitalize_first:
                    verb = QStringLiteral("Capitalize first character of each value of");
                    break;
                }
                return QStringLiteral("%1. %2 %3").arg(index + 1U).arg(verb, field);
            } else if constexpr (std::is_same_v<Action, metadata::MetadataCopyFieldAction>) {
                return QStringLiteral("%1. Copy %3 to %2")
                    .arg(index + 1U)
                    .arg(field, display_utf8(typed.source_field));
            } else if constexpr (std::is_same_v<Action, metadata::MetadataConvertRatingAction>) {
                const auto scale = typed.scale == metadata::PlainRatingScale::hundred
                                       ? QStringLiteral("0–100")
                                   : typed.scale == metadata::PlainRatingScale::ten
                                       ? QStringLiteral("0–10")
                                       : QStringLiteral("0–5 stars");
                return QStringLiteral("%1. Convert the rating in %2 (%3) to %4")
                    .arg(index + 1U)
                    .arg(display_utf8(typed.source_field), scale, field);
            } else if constexpr (std::is_same_v<Action, metadata::MetadataSplitValuesAction>) {
                return QStringLiteral("%1. Split %2 by %3")
                    .arg(index + 1U)
                    .arg(field, display_utf8(typed.separator));
            } else if constexpr (std::is_same_v<Action, metadata::MetadataJoinValuesAction>) {
                const auto separator = typed.separator.empty() ? QStringLiteral("(empty separator)")
                                                               : display_utf8(typed.separator);
                return QStringLiteral("%1. Join %2 with %3").arg(index + 1U).arg(field, separator);
            } else if constexpr (std::is_same_v<Action,
                                                metadata::MetadataRemoveMatchingValuesAction>) {
                const auto match = typed.match.empty() ? QStringLiteral("(empty value)")
                                                       : display_utf8(typed.match);
                return QStringLiteral("%1. Remove values of %2 equal to %3")
                    .arg(index + 1U)
                    .arg(field, match);
            } else if constexpr (std::is_same_v<Action,
                                                metadata::MetadataReplaceMatchingValuesAction>) {
                const auto match = typed.match.empty() ? QStringLiteral("(empty value)")
                                                       : display_utf8(typed.match);
                return QStringLiteral("%1. Replace values of %2 equal to %3 with %4")
                    .arg(index + 1U)
                    .arg(field, match, display_plan_values(typed.replacement_values));
            } else if constexpr (std::is_same_v<Action,
                                                metadata::MetadataNumberGroupedItemsAction>) {
                return QStringLiteral("%1. Number %2 from %3 within each %4 group")
                    .arg(index + 1U)
                    .arg(field)
                    .arg(typed.start)
                    .arg(display_utf8(typed.group_expression));
            } else if constexpr (std::is_same_v<Action,
                                                metadata::MetadataNumberSelectedItemsAction>) {
                return typed.padding == 0U
                           ? QStringLiteral("%1. Number %2 from %3 by selected-file order")
                                 .arg(index + 1U)
                                 .arg(field)
                                 .arg(typed.start)
                           : QStringLiteral("%1. Number %2 from %3 by selected-file order "
                                            "(minimum width %4)")
                                 .arg(index + 1U)
                                 .arg(field)
                                 .arg(typed.start)
                                 .arg(typed.padding);
            } else if constexpr (std::is_same_v<Action,
                                                metadata::MetadataKeepFirstCharactersAction>) {
                return QStringLiteral("%1. Keep the first %3 characters of each value of %2")
                    .arg(index + 1U)
                    .arg(field)
                    .arg(typed.character_count);
            } else if constexpr (std::is_same_v<Action, metadata::MetadataFormatValueAction>) {
                return QStringLiteral("%1. Format %2 as %3")
                    .arg(index + 1U)
                    .arg(field, display_utf8(typed.source));
            } else if constexpr (std::is_same_v<Action, metadata::MetadataCaptureValuesAction>) {
                QString source;
                switch (typed.source_kind) {
                case metadata::MetadataCaptureSourceKind::filename:
                    source = QStringLiteral("filename");
                    break;
                case metadata::MetadataCaptureSourceKind::full_path:
                    source = QStringLiteral("full path");
                    break;
                case metadata::MetadataCaptureSourceKind::formatted:
                    source = QStringLiteral("formatted text %1").arg(display_utf8(typed.source));
                    break;
                case metadata::MetadataCaptureSourceKind::field:
                    source = QStringLiteral("field %1").arg(display_utf8(typed.source));
                    break;
                }
                return QStringLiteral("%1. Capture %2 with %3")
                    .arg(index + 1U)
                    .arg(source, display_utf8(typed.pattern));
            }
            return QString{};
        },
        action);
}

QString ScriptSession::capitalizationNoChangeSummary() const {
    if (!preview_ || actions_.empty()) {
        return {};
    }
    for (const auto& action : actions_) {
        const auto* transform = std::get_if<metadata::MetadataTransformValuesAction>(&action);
        if (transform == nullptr ||
            transform->transform != metadata::MetadataValueTransformKind::capitalize_first) {
            return {};
        }
    }
    const auto present = preview_->unchanged_present_cell_count;
    const auto missing = preview_->unchanged_missing_cell_count;
    QString target;
    if (actions_.size() == 1U) {
        target = display_utf8(
            std::get<metadata::MetadataTransformValuesAction>(actions_.front()).target_field);
    }
    if (present == 0U) {
        return target.isEmpty()
                   ? QStringLiteral("No changes: none of the selected files contains the "
                                    "targeted fields; missing fields are skipped.")
                   : QStringLiteral("No changes: none of the selected files contains %1; "
                                    "missing fields are skipped.")
                         .arg(target);
    }
    auto message = target.isEmpty()
                       ? QStringLiteral("No changes: every existing targeted value already "
                                        "starts with its uppercase form.")
                       : QStringLiteral("No changes: every existing %1 value already starts "
                                        "with its uppercase form.")
                             .arg(target);
    if (missing > 0U) {
        const auto verb = missing == 1U ? QStringLiteral("was") : QStringLiteral("were");
        message += QStringLiteral(" %1 targeted %2 %3 missing and %4 skipped.")
                       .arg(missing)
                       .arg(missing == 1U ? QStringLiteral("field") : QStringLiteral("fields"))
                       .arg(verb, verb);
    }
    return message;
}

metadata::MetadataTransformationChain ScriptSession::currentChain() const {
    return metadata::MetadataTransformationChain{
        .schema_version = 1U,
        .name = encode_utf8(name_),
        .actions = actions_,
    };
}

QString ScriptSession::interchangeErrorText(const core::Error& error) {
    auto message = display_utf8(error.message);
    for (const auto& context : error.context) {
        if (context.key == "location") {
            message += QStringLiteral(" · %1").arg(display_utf8(context.value));
        } else if (context.key == "action") {
            message += QStringLiteral(" · step %1")
                           .arg(QString::fromStdString(context.value).toULongLong() + 1U);
        }
    }
    return message;
}

// Enablement.

bool ScriptSession::unsaved() const {
    return raw_modified_ || !clean_chain_ || currentChain() != *clean_chain_;
}

bool ScriptSession::canSelectSaved() const {
    return editing() && static_cast<bool>(store_.load) && !unsaved();
}

QString ScriptSession::saveText() const {
    return selected_saved_ ? QStringLiteral("Save changes") : QStringLiteral("Save");
}

bool ScriptSession::canSave() const {
    return editing() && static_cast<bool>(store_.save) && unsaved() &&
           (!raw_modified_ || raw_valid_) && !actions_.empty() && !name_.trimmed().isEmpty();
}

bool ScriptSession::canSaveAsNew() const {
    return editing() && static_cast<bool>(store_.save) && (!raw_modified_ || raw_valid_) &&
           !actions_.empty() && !name_.trimmed().isEmpty();
}

bool ScriptSession::canDelete() const {
    return editing() && static_cast<bool>(store_.remove) && selected_saved_.has_value();
}

bool ScriptSession::canExport() const {
    return editing() && (!raw_modified_ || raw_valid_) && !actions_.empty() &&
           !name_.trimmed().isEmpty();
}

bool ScriptSession::canRemove(const int row) const {
    return editing() && row >= 0 && static_cast<std::size_t>(row) < actions_.size();
}

bool ScriptSession::canMoveUp(const int row) const { return canRemove(row) && row > 0; }

bool ScriptSession::canMoveDown(const int row) const {
    return canRemove(row) && static_cast<std::size_t>(row) + 1U < actions_.size();
}

bool ScriptSession::canStage() const {
    return editing() && preview_ != nullptr && !preview_->cells.empty();
}

// The saved scripts.

void ScriptSession::loadSaved() {
    if (!store_.load) {
        catalog_status_ = QStringLiteral("Saved scripts are unavailable in this session.");
        emit changed();
        return;
    }
    catalog_busy_ = true;
    catalog_status_ = QStringLiteral("Loading saved scripts…");
    emit changed();
    const QPointer self{this};
    store_.load([self](std::vector<persistence::SavedMetadataTransformationChain> chains,
                       QString error) mutable {
        if (!self) {
            return;
        }
        self->catalog_busy_ = false;
        if (!error.isEmpty()) {
            self->catalog_status_ = QStringLiteral("Could not load saved scripts · %1").arg(error);
            emit self->changed();
            return;
        }
        self->catalog_ = std::move(chains);
        self->repopulateSaved(self->initially_selected_);
        if (self->initially_selected_) {
            self->selectSaved(self->savedIndex());
            if (self->preview_initially_selected_ && !self->actions_.empty()) {
                QTimer::singleShot(0, self, [self] {
                    if (self) {
                        self->startPreview();
                    }
                });
            }
        }
        self->catalog_status_ = QStringLiteral("%1 saved %2 available")
                                    .arg(self->catalog_.size())
                                    .arg(self->catalog_.size() == 1U ? QStringLiteral("script")
                                                                     : QStringLiteral("scripts"));
        emit self->changed();
    });
}

void ScriptSession::repopulateSaved(const std::optional<core::StableId>& selected) {
    std::ranges::sort(catalog_, [](const auto& left, const auto& right) {
        if (left.chain.name != right.chain.name) {
            return left.chain.name < right.chain.name;
        }
        return left.id.to_string() < right.id.to_string();
    });
    selected_saved_ =
        selected && std::ranges::any_of(
                        catalog_, [&selected](const auto& entry) { return entry.id == *selected; })
            ? selected
            : std::nullopt;
    emit savedChanged();
    emit changed();
}

void ScriptSession::selectSaved(const int index) {
    if (catalog_busy_) {
        return;
    }
    loading_definition_ = true;
    if (index <= 0 || static_cast<std::size_t>(index) > catalog_.size()) {
        selected_saved_.reset();
        name_ = QStringLiteral("Untitled script");
        actions_.clear();
        invalidatePreview();
        emit stepsChanged(-1);
        clean_chain_ = currentChain();
        refreshRawFromActions();
        loading_definition_ = false;
        catalog_status_ = QStringLiteral("Editing a new script");
        emit savedChanged();
        emit changed();
        return;
    }
    const auto& selected = catalog_[static_cast<std::size_t>(index) - 1U];
    selected_saved_ = selected.id;
    name_ = display_utf8(selected.chain.name);
    actions_ = selected.chain.actions;
    invalidatePreview();
    emit stepsChanged(0);
    clean_chain_ = currentChain();
    refreshRawFromActions();
    loading_definition_ = false;
    catalog_status_ = QStringLiteral("Loaded script · %1").arg(display_utf8(selected.chain.name));
    emit savedChanged();
    emit changed();
}

void ScriptSession::setName(const QString& name) {
    if (name_ == name) {
        return;
    }
    name_ = name;
    if (!loading_definition_) {
        catalog_status_ = QStringLiteral("Unsaved name change · Save to keep it");
    }
    emit changed();
}

void ScriptSession::importNative(const QString& path) {
    if (catalog_busy_ || path.isEmpty()) {
        return;
    }
    catalog_busy_ = true;
    catalog_status_ = QStringLiteral("Importing native tagging script…");
    emit changed();
    using NativeImportResult = core::Result<metadata::MetadataTransformationChain>;
    auto* watcher = new QFutureWatcher<std::shared_ptr<NativeImportResult>>(this);
    const QPointer self{this};
    connect(watcher, &QFutureWatcherBase::finished, this, [self, watcher] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (!self) {
            return;
        }
        self->catalog_busy_ = false;
        if (!result || !*result) {
            self->catalog_status_ =
                QStringLiteral("Could not import native tagging script · %1")
                    .arg(result ? interchangeErrorText(result->error())
                                : QStringLiteral("The import task returned no result"));
            emit self->changed();
            return;
        }
        auto chain = std::move(**result);
        self->loading_definition_ = true;
        self->selected_saved_.reset();
        emit self->savedChanged();
        self->name_ = display_utf8(chain.name);
        self->actions_ = std::move(chain.actions);
        self->clearPreview();
        emit self->stepsChanged(0);
        self->refreshRawFromActions();
        self->clean_chain_.reset();
        self->loading_definition_ = false;
        self->invalidatePreview();
        self->catalog_status_ =
            QStringLiteral("Imported · review, preview, and Save to keep this script");
        emit self->changed();
    });
    watcher->setFuture(QtConcurrent::run([path] {
        return std::make_shared<NativeImportResult>(ui::loadMetadataTransformationChainFile(path));
    }));
}

void ScriptSession::exportNative(const QString& path) {
    if (!canExport() || path.isEmpty()) {
        return;
    }
    catalog_busy_ = true;
    catalog_status_ = QStringLiteral("Exporting native tagging script…");
    emit changed();
    using NativeExportResult = core::Result<void>;
    auto* watcher = new QFutureWatcher<std::shared_ptr<NativeExportResult>>(this);
    const QPointer self{this};
    connect(watcher, &QFutureWatcherBase::finished, this, [self, watcher] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (!self) {
            return;
        }
        self->catalog_busy_ = false;
        self->catalog_status_ =
            !result || !*result
                ? QStringLiteral("Could not export native tagging script · %1")
                      .arg(result ? interchangeErrorText(result->error())
                                  : QStringLiteral("The export task returned no result"))
                : QStringLiteral("Native tagging script exported");
        emit self->changed();
    });
    watcher->setFuture(QtConcurrent::run([path, chain = currentChain()] {
        return std::make_shared<NativeExportResult>(
            ui::saveMetadataTransformationChainFile(path, chain));
    }));
}

ScriptSession::Focus ScriptSession::save(const bool as_new) {
    if (catalog_busy_ || !store_.save || actions_.empty()) {
        return Focus::none;
    }
    if (name_.trimmed().isEmpty()) {
        catalog_status_ = QStringLiteral("Enter a script name before saving.");
        emit changed();
        return Focus::name;
    }
    auto chain = currentChain();
    if (const auto valid = metadata::validate_metadata_transformation_chain(chain); !valid) {
        catalog_status_ =
            QStringLiteral("Cannot save script · %1").arg(display_utf8(valid.error().message));
        emit changed();
        return Focus::none;
    }
    persistence::SavedMetadataTransformationChain saved_chain{
        .id = !as_new && selected_saved_ ? *selected_saved_ : core::StableId::random(),
        .chain = std::move(chain),
        .automatic = false,
    };
    if (!as_new && selected_saved_) {
        const auto existing = std::ranges::find(catalog_, *selected_saved_,
                                                &persistence::SavedMetadataTransformationChain::id);
        if (existing != catalog_.end()) {
            saved_chain.automatic = existing->automatic;
        }
    }
    catalog_busy_ = true;
    catalog_status_ = QStringLiteral("Saving…");
    emit changed();
    const QPointer self{this};
    auto retained_chain = saved_chain;
    store_.save(std::move(saved_chain), [self, saved_chain = std::move(retained_chain)](
                                            QString error) mutable {
        if (!self) {
            return;
        }
        self->catalog_busy_ = false;
        if (!error.isEmpty()) {
            self->catalog_status_ = QStringLiteral("Could not save script · %1").arg(error);
            emit self->changed();
            return;
        }
        const auto found = std::ranges::find(self->catalog_, saved_chain.id,
                                             &persistence::SavedMetadataTransformationChain::id);
        if (found == self->catalog_.end()) {
            self->catalog_.push_back(saved_chain);
        } else {
            *found = saved_chain;
        }
        self->repopulateSaved(saved_chain.id);
        self->clean_chain_ = saved_chain.chain;
        self->raw_modified_ = false;
        self->catalog_status_ =
            QStringLiteral("Saved · %1").arg(display_utf8(saved_chain.chain.name));
        emit self->changed();
    });
    return Focus::none;
}

void ScriptSession::deleteSaved() {
    if (catalog_busy_ || !store_.remove || !selected_saved_) {
        return;
    }
    const auto id = *selected_saved_;
    catalog_busy_ = true;
    catalog_status_ = QStringLiteral("Deleting saved script…");
    emit changed();
    const QPointer self{this};
    store_.remove(id, [self, id](QString error) {
        if (!self) {
            return;
        }
        self->catalog_busy_ = false;
        if (!error.isEmpty()) {
            self->catalog_status_ = QStringLiteral("Could not delete saved script · %1").arg(error);
            emit self->changed();
            return;
        }
        std::erase_if(self->catalog_, [id](const auto& entry) { return entry.id == id; });
        self->selected_saved_.reset();
        self->repopulateSaved();
        self->name_ = QStringLiteral("Untitled script");
        self->actions_.clear();
        self->invalidatePreview();
        emit self->stepsChanged(-1);
        self->clean_chain_ = self->currentChain();
        self->refreshRawFromActions();
        self->catalog_status_ = QStringLiteral("Saved script deleted");
        emit self->changed();
    });
}

// The steps.

void ScriptSession::stepsEdited(const int select) {
    invalidatePreview();
    emit stepsChanged(select);
    refreshRawFromActions();
    catalog_status_ = QStringLiteral("Unsaved changes · Save to keep them");
    emit changed();
}

ScriptSession::Focus ScriptSession::addStep(const StepInput& step) {
    const auto kind = step.kind;
    const auto refuse = [this](const QString& text, const Focus focus) {
        summary_ = text;
        emit changed();
        return focus;
    };
    const auto field = step.target.trimmed();
    if (kind != 16 && kind != 18 && kind != 19 && kind != 20 && field.isEmpty()) {
        return refuse(QStringLiteral("Enter a target field before adding the step."),
                      Focus::target);
    }
    const auto target = encode_utf8(field);
    switch (kind) {
    case 0:
        actions_.push_back(metadata::MetadataSetValuesAction{.target_field = target,
                                                             .values = {encode_utf8(step.input)}});
        break;
    case 1:
        actions_.push_back(metadata::MetadataAddValuesAction{.target_field = target,
                                                             .values = {encode_utf8(step.input)}});
        break;
    case 2:
        actions_.push_back(metadata::MetadataRemoveFieldAction{.target_field = target});
        break;
    case 3:
        actions_.push_back(metadata::MetadataTransformValuesAction{
            .target_field = target, .transform = metadata::MetadataValueTransformKind::trim_ascii});
        break;
    case 4:
        actions_.push_back(metadata::MetadataTransformValuesAction{
            .target_field = target, .transform = metadata::MetadataValueTransformKind::lowercase});
        break;
    case 5:
        actions_.push_back(metadata::MetadataTransformValuesAction{
            .target_field = target, .transform = metadata::MetadataValueTransformKind::uppercase});
        break;
    case 6:
        actions_.push_back(metadata::MetadataTransformValuesAction{
            .target_field = target,
            .transform = metadata::MetadataValueTransformKind::capitalize_first});
        break;
    case 7: {
        const auto source = step.input.trimmed();
        if (source.isEmpty()) {
            return refuse(QStringLiteral("Enter a source field for the copy step."), Focus::input);
        }
        actions_.push_back(metadata::MetadataCopyFieldAction{.target_field = target,
                                                             .source_field = encode_utf8(source)});
        break;
    }
    case 8:
        if (step.input.isEmpty()) {
            return refuse(QStringLiteral("Split requires a non-empty exact separator."),
                          Focus::input);
        }
        actions_.push_back(metadata::MetadataSplitValuesAction{
            .target_field = target, .separator = encode_utf8(step.input)});
        break;
    case 9:
        actions_.push_back(metadata::MetadataJoinValuesAction{
            .target_field = target, .separator = encode_utf8(step.input)});
        break;
    case 10:
        actions_.push_back(metadata::MetadataFormatValueAction{
            .target_field = target, .dialect = {}, .source = encode_utf8(step.input)});
        break;
    case 11:
        actions_.push_back(metadata::MetadataRemoveMatchingValuesAction{
            .target_field = target, .match = encode_utf8(step.input)});
        break;
    case 12:
        actions_.push_back(metadata::MetadataReplaceMatchingValuesAction{
            .target_field = target,
            .match = encode_utf8(step.input),
            .replacement_values = {encode_utf8(step.replacement)},
        });
        break;
    case 13:
        actions_.push_back(metadata::MetadataNumberSelectedItemsAction{
            .target_field = target,
            .start = static_cast<std::uint32_t>(step.number_start),
            .padding = static_cast<std::uint32_t>(step.number_padding),
        });
        break;
    case 14:
        actions_.push_back(metadata::MetadataKeepFirstCharactersAction{
            .target_field = target,
            .character_count = static_cast<std::uint32_t>(step.character_count),
        });
        break;
    case 15:
        if (step.input.trimmed().isEmpty()) {
            return refuse(QStringLiteral("Conditional removal requires a non-empty condition."),
                          Focus::input);
        }
        actions_.push_back(metadata::MetadataRemoveFieldIfAction{
            .target_field = target,
            .dialect = {},
            .condition = encode_utf8(step.input),
        });
        break;
    case 16: {
        if (step.input.isEmpty()) {
            return refuse(QStringLiteral("Enter a tkcapture-1 pattern."), Focus::input);
        }
        const auto source_kind =
            static_cast<metadata::MetadataCaptureSourceKind>(std::clamp(step.capture_source, 0, 3));
        const auto needs_argument = source_kind == metadata::MetadataCaptureSourceKind::formatted ||
                                    source_kind == metadata::MetadataCaptureSourceKind::field;
        if (needs_argument && step.capture_argument.trimmed().isEmpty()) {
            return refuse(QStringLiteral("Enter the capture source argument."),
                          Focus::capture_argument);
        }
        const auto source = source_kind == metadata::MetadataCaptureSourceKind::field
                                ? step.capture_argument.trimmed()
                                : step.capture_argument;
        actions_.push_back(metadata::MetadataCaptureValuesAction{
            .dialect = {},
            .source_kind = source_kind,
            .source = needs_argument ? encode_utf8(source) : std::string{},
            .pattern = encode_utf8(step.input),
        });
        break;
    }
    case 20: {
        const auto source = step.input.trimmed();
        if (source.isEmpty()) {
            return refuse(QStringLiteral("Enter the field the rating is in."), Focus::input);
        }
        const auto scale = step.rating_scale == 2   ? metadata::PlainRatingScale::hundred
                           : step.rating_scale == 1 ? metadata::PlainRatingScale::ten
                                                    : metadata::PlainRatingScale::five;
        actions_.push_back(metadata::MetadataConvertRatingAction{
            .target_field = std::string{metadata::fmps_rating_field},
            .source_field = encode_utf8(source),
            .scale = scale});
        break;
    }
    case 18:
    case 19: {
        std::vector<std::string> fields;
        for (const auto& part : step.input.split(QChar{','}, Qt::SkipEmptyParts)) {
            const auto trimmed = part.trimmed();
            if (!trimmed.isEmpty()) {
                fields.push_back(encode_utf8(trimmed));
            }
        }
        if (fields.empty()) {
            return refuse(QStringLiteral("Enter at least one comma-separated field name."),
                          Focus::input);
        }
        if (kind == 18) {
            actions_.push_back(
                metadata::MetadataBlocklistFieldsAction{.fields = std::move(fields)});
        } else {
            actions_.push_back(
                metadata::MetadataAllowlistFieldsAction{.fields = std::move(fields)});
        }
        break;
    }
    default:
        return Focus::none;
    }
    stepsEdited(static_cast<int>(actions_.size()) - 1);
    return kind == 16 ? Focus::input : Focus::target;
}

void ScriptSession::removeStep(const int row) {
    if (row < 0 || static_cast<std::size_t>(row) >= actions_.size()) {
        return;
    }
    actions_.erase(actions_.begin() + row);
    stepsEdited(std::min(row, static_cast<int>(actions_.size()) - 1));
}

void ScriptSession::moveStep(const int row, const int offset) {
    const auto destination = row + offset;
    if (row < 0 || destination < 0 || static_cast<std::size_t>(row) >= actions_.size() ||
        static_cast<std::size_t>(destination) >= actions_.size()) {
        return;
    }
    std::swap(actions_[static_cast<std::size_t>(row)],
              actions_[static_cast<std::size_t>(destination)]);
    stepsEdited(destination);
}

void ScriptSession::importRuleScript(const QString& source, const bool append) {
    auto translated = translateRuleScript(source);
    if (!translated.ready) {
        return;
    }
    auto imported = std::move(translated.actions);
    if (append) {
        if (actions_.size() > 256U || imported.size() > 256U - actions_.size()) {
            summary_ = QStringLiteral("Appending those rules would exceed the 256-step limit.");
            emit changed();
            return;
        }
        actions_.insert(actions_.end(), std::make_move_iterator(imported.begin()),
                        std::make_move_iterator(imported.end()));
    } else {
        actions_ = std::move(imported);
    }
    invalidatePreview();
    emit stepsChanged(static_cast<int>(actions_.size()) - 1);
    // The Raw tab shows what the paste became, in Trackknife's own terms: the
    // Picard source was only ever an import (ADR-0241).
    refreshRawFromActions();
    catalog_status_ = QStringLiteral("Unsaved · generated %1 typed rules from the pasted script. "
                                     "Review, preview, then click Save to keep them.")
                          .arg(actions_.size());
    emit changed();
}

// The raw source.

void ScriptSession::refreshRawFromActions() {
    raw_modified_ = false;
    raw_valid_ = false;
    if (actions_.empty()) {
        raw_read_only_ = false;
        raw_source_.clear();
        raw_diagnostics_ = QStringLiteral(
            "Write steps such as $set(FIELD,tkfmt-1 value) or $delete(FIELD), one per line.");
        emit rawChanged();
        emit changed();
        return;
    }
    const auto exported = metadata::export_native_rule_script(actions_);
    if (!exported) {
        raw_source_.clear();
        raw_read_only_ = true;
        auto message = QStringLiteral("Raw mode is unavailable: %1")
                           .arg(display_utf8(exported.error().message));
        for (const auto& context : exported.error().context) {
            if (context.key == "action") {
                message += QStringLiteral(" · typed step %1")
                               .arg(QString::fromStdString(context.value).toULongLong() + 1U);
            }
        }
        raw_diagnostics_ = message;
        emit rawChanged();
        emit changed();
        return;
    }
    raw_read_only_ = false;
    raw_source_ = display_utf8(*exported);
    raw_import_ = metadata::import_native_rule_script(*exported);
    raw_valid_ = !raw_import_.has_errors();
    raw_diagnostics_ = QStringLiteral("Ready · %1 steps · rewritten in canonical form after "
                                      "each edit on the Steps tab")
                           .arg(actions_.size());
    emit rawChanged();
    emit changed();
}

void ScriptSession::setRawSource(const QString& source) {
    if (raw_read_only_ || source == raw_source_) {
        return;
    }
    raw_source_ = source;
    raw_modified_ = true;
    raw_import_ = metadata::import_native_rule_script(encode_utf8(source));
    QStringList diagnostics;
    for (const auto& diagnostic : raw_import_.diagnostics) {
        const auto severity =
            diagnostic.severity == metadata::MetadataRuleScriptDiagnosticSeverity::error
                ? QStringLiteral("Error")
                : QStringLiteral("Warning");
        diagnostics.push_back(QStringLiteral("%1 · line %2, column %3 · %4")
                                  .arg(severity)
                                  .arg(diagnostic.line)
                                  .arg(diagnostic.column)
                                  .arg(display_utf8(diagnostic.message)));
    }
    raw_valid_ = !raw_import_.has_errors() && !raw_import_.actions.empty();
    invalidatePreview();
    if (raw_valid_) {
        actions_ = raw_import_.actions;
        diagnostics.prepend(
            QStringLiteral("Ready · %1 steps").arg(actions_.size()));
        emit stepsChanged(static_cast<int>(actions_.size()) - 1);
    }
    raw_diagnostics_ = diagnostics.join(QChar{'\n'});
    catalog_status_ =
        raw_valid_ ? QStringLiteral("Unsaved changes · Save to keep them")
                   : QStringLiteral(
                         "Raw script has errors · the preview and Save wait until they are fixed");
    emit changed();
}

// The preview.

void ScriptSession::clearPreview() {
    preview_.reset();
    if (preview_model_ != nullptr) {
        preview_model_->deleteLater();
        preview_model_ = nullptr;
        emit previewChanged();
    }
}

// Invalidation schedules a fresh debounced preview whenever the current
// script could produce one, so the preview tracks edits by itself.
void ScriptSession::invalidatePreview() {
    clearPreview();
    if (!planning_) {
        summary_ = actions_.empty() ? QStringLiteral("Add a step to see a preview.")
                                    : QStringLiteral("Updating preview…");
    }
    if (!actions_.empty() && (!raw_modified_ || raw_valid_)) {
        preview_timer_.start();
    } else {
        preview_timer_.stop();
    }
}

void ScriptSession::startPreview() {
    if (actions_.empty() || (raw_modified_ && !raw_valid_)) {
        return;
    }
    if (planning_) {
        preview_timer_.start();
        return;
    }
    clearPreview();
    cancellation_.request_cancellation();
    cancellation_ = core::CancellationSource{};
    planning_ = true;
    summary_ = QStringLiteral("Updating preview…");
    emit changed();
    watcher_.setFuture(
        QtConcurrent::run([selection = selection_, draft = draft_, items = item_indexes_,
                           chain = currentChain(), cancellation = cancellation_.token()]() mutable {
            return std::make_shared<PreviewResult>(metadata::plan_metadata_transformation(
                *selection, draft, items, std::move(chain), cancellation));
        }));
}

void ScriptSession::finishPreview() {
    planning_ = false;
    if (close_requested_) {
        emit closeRequested();
        return;
    }
    const auto result = watcher_.result();
    if (!result || !*result) {
        auto message = result ? display_utf8(result->error().message)
                              : QStringLiteral("The preview task returned no result");
        if (result) {
            for (const auto& entry : result->error().context) {
                if (entry.key == "action") {
                    message += QStringLiteral(" · step %1")
                                   .arg(QString::fromStdString(entry.value).toULongLong() + 1U);
                } else if (entry.key == "item") {
                    message += QStringLiteral(" · file row %1")
                                   .arg(QString::fromStdString(entry.value).toULongLong() + 1U);
                }
            }
        }
        summary_ = QStringLiteral("Transformation preview failed · %1").arg(message);
        emit changed();
        return;
    }
    preview_ = std::make_shared<const metadata::MetadataTransformationPreview>(std::move(**result));
    preview_model_ = createMetadataTransformationPreviewModel(preview_, track_labels_, this);
    emit previewChanged();
    const auto capitalization_summary = capitalizationNoChangeSummary();
    summary_ =
        preview_->cells.empty()
            ? (capitalization_summary.isEmpty()
                   ? QStringLiteral("The script produces no changes for the selected files.")
                   : capitalization_summary)
            : QStringLiteral("%1 final cell %2 across %3 selected %4 · add to draft when ready")
                  .arg(preview_->cells.size())
                  .arg(preview_->cells.size() == 1U ? QStringLiteral("change")
                                                    : QStringLiteral("changes"))
                  .arg(preview_->changed_item_count)
                  .arg(preview_->changed_item_count == 1U ? QStringLiteral("file")
                                                          : QStringLiteral("files"));
    emit changed();
}

void ScriptSession::stage() {
    if (!preview_ || preview_->cells.empty() || !stage_) {
        return;
    }
    if (!stage_(*preview_)) {
        invalidatePreview();
        summary_ = QStringLiteral("The preview is stale or could not fit in the draft. It will "
                                  "refresh automatically.");
        emit changed();
        return;
    }
    emit accepted();
}

ScriptSession::CloseAnswer ScriptSession::requestClose() {
    if (planning_) {
        close_requested_ = true;
        cancellation_.request_cancellation();
        summary_ = QStringLiteral("Cancelling preview…");
        emit changed();
        return CloseAnswer::wait;
    }
    return unsaved() ? CloseAnswer::confirm_discard : CloseAnswer::close;
}

} // namespace trackknife::bench
