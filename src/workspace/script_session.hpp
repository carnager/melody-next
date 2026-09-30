// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/metadata/field_suggestions.hpp"
#include "trackknife/metadata/rule_script_import.hpp"
#include "trackknife/metadata/staged_patch.hpp"
#include "trackknife/metadata/transformation.hpp"
#include "workspace/tagger_session.hpp"

#include <QAbstractItemModel>
#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::bench {

// The tagging script editor (ADR-0178): a saved script -- steps that set,
// clean, split, number, capture or remove fields -- edited as typed steps
// or as raw Picard-style source, previewed on the selected files as it
// changes, saved, imported and exported as native JSON, and added to the
// tag editor's draft. Both windows' script editors draw it.
class ScriptSession final : public QObject {
    Q_OBJECT

  public:
    using StageCallback =
        std::function<bool(const metadata::MetadataTransformationPreview& preview)>;

    // A kind of step the editor adds, or a heading grouping them.
    struct StepKind {
        QString label;
        int kind{-1};
        QString tool_tip;
    };
    // What the step form asks for a kind (and capture source).
    struct StepForm {
        bool target{true};
        bool input{false};
        QString input_label;
        QString input_placeholder;
        bool replacement{false};
        bool numbering{false};
        bool characters{false};
        bool capture_source{false};
        bool capture_argument{false};
        QString capture_argument_label;
        QString capture_argument_placeholder;
        // The input is a comma-separated field list.
        bool field_list{false};
        // A choice of the scale a rating is kept on (ratingScales()).
        bool rating_scale{false};
    };
    // What the step form holds when a step is added.
    struct StepInput {
        int kind{-1};
        QString target;
        QString input;
        QString replacement;
        int number_start{1};
        int number_padding{0};
        int character_count{4};
        int capture_source{0};
        QString capture_argument;
        // An index into ratingScales().
        int rating_scale{0};
    };
    // Where the view should put the cursor after adding a step fails or
    // succeeds.
    enum class Focus { none, target, input, capture_argument, name };
    // A pasted Picard-style script, translated.
    struct RuleScript {
        std::vector<metadata::MetadataTransformationAction> actions;
        QString diagnostics;
        bool ready{false};
    };
    enum class CloseAnswer { close, confirm_discard, wait };

    ScriptSession(std::shared_ptr<const metadata::StagedMetadataSelection> selection,
                  metadata::StagedMetadataPatchSet draft, std::vector<std::size_t> item_indexes,
                  QStringList track_labels, StageCallback stage, MetadataTransformationStore store,
                  std::optional<core::StableId> initially_selected = std::nullopt,
                  bool preview_initially_selected = false, QObject* parent = nullptr);
    ~ScriptSession() override;

    [[nodiscard]] static const std::vector<StepKind>& stepKinds();
    // The row of stepKinds() the form starts on.
    [[nodiscard]] static int initialStepKind() { return 1; }
    [[nodiscard]] static StepForm stepForm(int kind, int capture_source);
    [[nodiscard]] static QStringList captureSources();
    // The scales a rating to convert can be kept on, as the step offers them.
    [[nodiscard]] static QStringList ratingScales();
    [[nodiscard]] static RuleScript translateRuleScript(const QString& source);

    // The saved scripts: "New script" first.
    [[nodiscard]] QStringList savedNames() const;
    [[nodiscard]] int savedIndex() const;
    [[nodiscard]] QString name() const { return name_; }
    [[nodiscard]] QStringList steps() const;
    [[nodiscard]] QString rawSource() const { return raw_source_; }
    [[nodiscard]] bool rawReadOnly() const { return raw_read_only_; }
    [[nodiscard]] QString rawDiagnostics() const { return raw_diagnostics_; }
    [[nodiscard]] QString summary() const { return summary_; }
    [[nodiscard]] QString catalogStatus() const { return catalog_status_; }
    [[nodiscard]] QAbstractItemModel* preview() const { return preview_model_; }
    [[nodiscard]] QStringList targetSuggestions(const QString& query) const;
    // The names to offer for the last comma-separated name in `text`, and
    // `text` with that name completed.
    [[nodiscard]] QStringList fieldListSuggestions(const QString& text) const;
    [[nodiscard]] static QString completeFieldList(const QString& text, const QString& name);
    [[nodiscard]] QString suggestedExportName() const;

    // Enablement.
    [[nodiscard]] bool unsaved() const;
    [[nodiscard]] bool editing() const { return !planning_ && !catalog_busy_; }
    [[nodiscard]] bool canSelectSaved() const;
    [[nodiscard]] QString saveText() const;
    [[nodiscard]] bool canSave() const;
    [[nodiscard]] bool canSaveAsNew() const;
    [[nodiscard]] bool canDelete() const;
    [[nodiscard]] bool canImport() const { return editing(); }
    [[nodiscard]] bool canExport() const;
    [[nodiscard]] bool canAdd() const { return editing() && actions_.size() < 256U; }
    [[nodiscard]] bool canRemove(int row) const;
    [[nodiscard]] bool canMoveUp(int row) const;
    [[nodiscard]] bool canMoveDown(int row) const;
    [[nodiscard]] bool canStage() const;

    void selectSaved(int index);
    void setName(const QString& name);
    [[nodiscard]] Focus addStep(const StepInput& step);
    void removeStep(int row);
    void moveStep(int row, int offset);
    // Raw source as typed: compiled into the steps when it is valid.
    void setRawSource(const QString& source);
    void importRuleScript(const QString& source, bool append);
    void importNative(const QString& path);
    void exportNative(const QString& path);
    // Saving needs a name; the view puts the cursor there when refused.
    [[nodiscard]] Focus save(bool as_new);
    void deleteSaved();
    void stage();
    [[nodiscard]] CloseAnswer requestClose();

  signals:
    void changed();
    // The steps changed; `select` is the row to make current, or -1.
    void stepsChanged(int select);
    void savedChanged();
    // The raw source was replaced (not typed).
    void rawChanged();
    void previewChanged();
    // Staged into the draft: the editor is done.
    void accepted();
    // A close asked for while the preview ran, now possible.
    void closeRequested();

  private:
    using PreviewResult = core::Result<metadata::MetadataTransformationPreview>;

    [[nodiscard]] QString actionText(const metadata::MetadataTransformationAction& action,
                                     std::size_t index) const;
    [[nodiscard]] QString capitalizationNoChangeSummary() const;
    [[nodiscard]] metadata::MetadataTransformationChain currentChain() const;
    [[nodiscard]] static QString interchangeErrorText(const core::Error& error);
    void loadSaved();
    void repopulateSaved(const std::optional<core::StableId>& selected = std::nullopt);
    void refreshRawFromActions();
    void clearPreview();
    void invalidatePreview();
    void startPreview();
    void finishPreview();
    void stepsEdited(int select);

    QFutureWatcher<std::shared_ptr<PreviewResult>> watcher_;
    std::shared_ptr<const metadata::StagedMetadataSelection> selection_;
    metadata::StagedMetadataPatchSet draft_;
    std::vector<std::size_t> item_indexes_;
    QStringList track_labels_;
    StageCallback stage_;
    MetadataTransformationStore store_;
    std::optional<core::StableId> initially_selected_;
    bool preview_initially_selected_{false};
    std::vector<metadata::MetadataFieldSuggestionCandidate> target_field_candidates_;
    std::vector<metadata::MetadataFieldSuggestionCandidate> fields_candidates_;
    std::vector<persistence::SavedMetadataTransformationChain> catalog_;
    std::optional<core::StableId> selected_saved_;
    QString name_{QStringLiteral("Untitled script")};
    std::vector<metadata::MetadataTransformationAction> actions_;
    std::optional<metadata::MetadataTransformationChain> clean_chain_;
    metadata::MetadataRuleScriptImportResult raw_import_;
    QString raw_source_;
    QString raw_diagnostics_;
    bool raw_read_only_{false};
    std::shared_ptr<const metadata::MetadataTransformationPreview> preview_;
    QPointer<QAbstractItemModel> preview_model_;
    core::CancellationSource cancellation_;
    QTimer preview_timer_;
    QString summary_{QStringLiteral("Add a step to see a preview.")};
    QString catalog_status_;
    bool planning_{false};
    bool catalog_busy_{false};
    bool close_requested_{false};
    bool loading_definition_{false};
    bool raw_modified_{false};
    bool raw_valid_{false};
};

} // namespace trackknife::bench
