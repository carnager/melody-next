// SPDX-License-Identifier: GPL-3.0-only

#include "bench/metadata_transformation_dialog.hpp"

#include "bench/metadata_dialog_helpers.hpp"
#include "bench/metadata_rule_script_import_dialog.hpp"
#include "bench/metadata_transformation_preview_model.hpp"
#include "trackknife/metadata/draft_document.hpp"
#include "trackknife/metadata/field_suggestions.hpp"
#include "trackknife/metadata/rule_script_import.hpp"
#include "uicommon/metadata_transformation_interchange.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStringListModel>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace trackknife::bench {
namespace {

constexpr auto transformation_geometry_key = "workspace/metadata-transformation-geometry-v1";
constexpr auto transformation_splitter_key = "workspace/metadata-transformation-splitter-v1";

class MetadataTransformationDialog final : public QDialog {
  public:
    using StageCallback =
        std::function<bool(const metadata::MetadataTransformationPreview& preview)>;
    using PreviewResult = core::Result<metadata::MetadataTransformationPreview>;
    using NativeImportResult = core::Result<metadata::MetadataTransformationChain>;
    using NativeExportResult = core::Result<void>;

    MetadataTransformationDialog(std::shared_ptr<const metadata::StagedMetadataSelection> selection,
                                 metadata::StagedMetadataPatchSet draft,
                                 std::vector<std::size_t> item_indexes, QStringList track_labels,
                                 StageCallback stage, MetadataTransformationStore store,
                                 QWidget* parent,
                                 std::optional<core::StableId> initially_selected = std::nullopt,
                                 const bool preview_initially_selected = false,
                                 MetadataDialogLayoutStore layout_store = {})
        : QDialog(parent), watcher_(this), selection_(std::move(selection)),
          draft_(std::move(draft)), item_indexes_(std::move(item_indexes)),
          track_labels_(std::move(track_labels)), stage_(std::move(stage)),
          store_(std::move(store)), initially_selected_(initially_selected),
          preview_initially_selected_(preview_initially_selected),
          layout_store_(std::move(layout_store)) {
        setObjectName(QStringLiteral("bench-metadata-transformation"));
        setWindowTitle(QStringLiteral("Tagging script editor[*]"));
        setWindowModality(Qt::WindowModal);
        setAttribute(Qt::WA_DeleteOnClose);
        resize(1'080, 620);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 8, 10, 8);
        layout->setSpacing(6);

        auto* catalog_row = new QHBoxLayout;
        catalog_row->setSpacing(6);
        catalog_row->addWidget(new QLabel(QStringLiteral("Script:"), this));
        saved_ = new QComboBox(this);
        saved_->setObjectName(QStringLiteral("bench-metadata-transformation-saved"));
        saved_->addItem(QStringLiteral("New script"));
        catalog_row->addWidget(saved_, 1);
        save_ = new QPushButton(QStringLiteral("Save"), this);
        save_->setObjectName(QStringLiteral("bench-metadata-transformation-save"));
        save_as_ = new QPushButton(QStringLiteral("Save as new"), this);
        save_as_->setObjectName(QStringLiteral("bench-metadata-transformation-save-as-new"));
        delete_saved_ = new QPushButton(QStringLiteral("Delete"), this);
        delete_saved_->setObjectName(QStringLiteral("bench-metadata-transformation-delete"));
        import_native_ = new QPushButton(QStringLiteral("Import…"), this);
        import_native_->setObjectName(
            QStringLiteral("bench-metadata-transformation-import-native"));
        import_native_->setToolTip(
            QStringLiteral("Open a complete native Trackknife tagging script as an unsaved "
                           "definition for review"));
        export_native_ = new QPushButton(QStringLiteral("Export…"), this);
        export_native_->setObjectName(
            QStringLiteral("bench-metadata-transformation-export-native"));
        export_native_->setToolTip(
            QStringLiteral("Write the complete current typed definition as versioned native "
                           "JSON; saved identity and automatic state are not included"));
        catalog_row->addWidget(save_);
        catalog_row->addWidget(save_as_);
        catalog_row->addWidget(delete_saved_);
        catalog_row->addWidget(import_native_);
        catalog_row->addWidget(export_native_);
        layout->addLayout(catalog_row);

        content_splitter_ = new QSplitter(Qt::Horizontal, this);
        content_splitter_->setObjectName(QStringLiteral("bench-metadata-transformation-splitter"));
        content_splitter_->setChildrenCollapsible(false);

        auto* editor_pane = new QWidget(content_splitter_);
        auto* editor_layout = new QVBoxLayout(editor_pane);
        editor_layout->setContentsMargins(0, 0, 0, 0);
        editor_layout->setSpacing(6);
        auto* name_form = new QFormLayout;
        name_ = new QLineEdit(QStringLiteral("Untitled script"), this);
        name_->setObjectName(QStringLiteral("bench-metadata-transformation-name"));
        name_form->addRow(QStringLiteral("Name:"), name_);
        editor_layout->addLayout(name_form);

        editor_tabs_ = new QTabWidget(editor_pane);
        editor_tabs_->setObjectName(QStringLiteral("bench-metadata-transformation-editor-tabs"));
        auto* rules_page = new QWidget(editor_tabs_);
        auto* rules_layout = new QVBoxLayout(rules_page);
        rules_layout->setContentsMargins(6, 6, 6, 6);
        auto* step_form = new QFormLayout;
        kind_ = new QComboBox(this);
        kind_->setObjectName(QStringLiteral("bench-metadata-transformation-kind"));
        // Kinds are grouped under unselectable headers; the row index therefore
        // no longer matches the action kind, which lives in the item data.
        const auto add_kind_header = [this](const QString& text) {
            kind_->addItem(text);
            auto* model = qobject_cast<QStandardItemModel*>(kind_->model());
            auto* item = model->item(kind_->count() - 1);
            item->setFlags(Qt::NoItemFlags);
            auto header_font = item->font();
            header_font.setBold(true);
            item->setFont(header_font);
        };
        const auto add_kind = [this](const QString& text, const int kind,
                                     const QString& tool_tip = {}) {
            kind_->addItem(text, kind);
            if (!tool_tip.isEmpty()) {
                kind_->setItemData(kind_->count() - 1, tool_tip, Qt::ToolTipRole);
            }
        };
        add_kind_header(QStringLiteral("Set values"));
        add_kind(QStringLiteral("Set one literal value"), 0);
        add_kind(QStringLiteral("Add one literal value"), 1);
        add_kind(QStringLiteral("Copy another field"), 7);
        add_kind(QStringLiteral("Format with tkfmt-1"), 10,
                 QStringLiteral("Build the value from an expression, "
                                "for example %artist% — %title%"));
        add_kind(QStringLiteral("Number by selected-file order"), 13);
        add_kind(QStringLiteral("Capture fields with tkcapture-1"), 16,
                 QStringLiteral("Extract several fields at once from the filename, the full "
                                "path, formatted text, or another field"));
        add_kind_header(QStringLiteral("Clean up values"));
        add_kind(QStringLiteral("Trim each value"), 3);
        add_kind(QStringLiteral("Lowercase each value"), 4);
        add_kind(QStringLiteral("Uppercase each value"), 5);
        add_kind(QStringLiteral("Capitalize first character"), 6);
        add_kind(QStringLiteral("Keep first characters of each value"), 14);
        add_kind_header(QStringLiteral("Split & join"));
        add_kind(QStringLiteral("Split by exact separator"), 8);
        add_kind(QStringLiteral("Join with exact separator"), 9);
        add_kind_header(QStringLiteral("Remove & replace"));
        add_kind(QStringLiteral("Remove field"), 2);
        add_kind(QStringLiteral("Remove field when condition matches"), 15,
                 QStringLiteral("The field is removed when the expression is non-empty, "
                                "for example $not(%totaldiscs%)"));
        add_kind(QStringLiteral("Remove exact matching values"), 11);
        add_kind(QStringLiteral("Replace exact matching values"), 12);
        add_kind(QStringLiteral("Remove listed fields (blocklist)"), 18,
                 QStringLiteral("Remove every named field from the selected files"));
        add_kind(QStringLiteral("Keep only listed fields (allowlist)"), 19,
                 QStringLiteral("Remove every field except the named fields"));
        kind_->setCurrentIndex(1);
        target_label_ = new QLabel(QStringLiteral("Target field:"), this);
        target_ = new QLineEdit(this);
        target_->setObjectName(QStringLiteral("bench-metadata-transformation-target"));
        target_->setPlaceholderText(QStringLiteral("For example: Title or ALBUM ARTIST"));
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
        target_completion_model_ = new QStringListModel(this);
        target_completion_model_->setObjectName(
            QStringLiteral("bench-metadata-transformation-target-completions"));
        target_completer_ = new QCompleter(target_completion_model_, this);
        target_completer_->setObjectName(
            QStringLiteral("bench-metadata-transformation-target-completer"));
        target_completer_->setCaseSensitivity(Qt::CaseInsensitive);
        target_completer_->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
        target_completer_->setMaxVisibleItems(12);
        target_->setCompleter(target_completer_);
        target_completion_model_->setStringList(targetFieldSuggestions({}));
        input_label_ = new QLabel(QStringLiteral("Value:"), this);
        input_ = new QLineEdit(this);
        input_->setObjectName(QStringLiteral("bench-metadata-transformation-input"));
        replacement_label_ = new QLabel(QStringLiteral("Replacement:"), this);
        replacement_ = new QLineEdit(this);
        replacement_->setObjectName(QStringLiteral("bench-metadata-transformation-replacement"));
        number_start_label_ = new QLabel(QStringLiteral("Start at:"), this);
        number_start_ = new QSpinBox(this);
        number_start_->setObjectName(QStringLiteral("bench-metadata-transformation-number-start"));
        number_start_->setRange(1, 1'000'000'000);
        number_start_->setValue(1);
        number_padding_label_ = new QLabel(QStringLiteral("Minimum width:"), this);
        number_padding_ = new QSpinBox(this);
        number_padding_->setObjectName(
            QStringLiteral("bench-metadata-transformation-number-padding"));
        number_padding_->setRange(0, 32);
        number_padding_->setValue(0);
        character_count_label_ = new QLabel(QStringLiteral("Characters to keep:"), this);
        character_count_ = new QSpinBox(this);
        character_count_->setObjectName(
            QStringLiteral("bench-metadata-transformation-character-count"));
        character_count_->setRange(1, 1'000'000);
        character_count_->setValue(4);
        capture_source_label_ = new QLabel(QStringLiteral("Capture source:"), this);
        capture_source_ = new QComboBox(this);
        capture_source_->setObjectName(
            QStringLiteral("bench-metadata-transformation-capture-source"));
        capture_source_->addItems(
            {QStringLiteral("Filename and requested parent folders"), QStringLiteral("Full path"),
             QStringLiteral("Formatted tkfmt-1 text"), QStringLiteral("Metadata field values")});
        capture_argument_label_ = new QLabel(QStringLiteral("Source expression:"), this);
        capture_argument_ = new QLineEdit(this);
        capture_argument_->setObjectName(
            QStringLiteral("bench-metadata-transformation-capture-argument"));
        step_form->addRow(QStringLiteral("New step:"), kind_);
        step_form->addRow(target_label_, target_);
        step_form->addRow(input_label_, input_);
        step_form->addRow(replacement_label_, replacement_);
        step_form->addRow(number_start_label_, number_start_);
        step_form->addRow(number_padding_label_, number_padding_);
        step_form->addRow(character_count_label_, character_count_);
        step_form->addRow(capture_source_label_, capture_source_);
        step_form->addRow(capture_argument_label_, capture_argument_);
        rules_layout->addLayout(step_form);

        auto* add_row = new QHBoxLayout;
        add_ = new QPushButton(QStringLiteral("Add step"), this);
        add_->setObjectName(QStringLiteral("bench-metadata-transformation-add"));
        import_ = new QPushButton(QStringLiteral("Paste script…"), this);
        import_->setObjectName(QStringLiteral("bench-metadata-transformation-import-script"));
        add_row->addWidget(add_);
        add_row->addWidget(import_);
        add_row->addStretch(1);
        rules_layout->addLayout(add_row);

        steps_ = new QListWidget(this);
        steps_->setObjectName(QStringLiteral("bench-metadata-transformation-steps"));
        steps_->setAlternatingRowColors(true);
        rules_layout->addWidget(steps_, 1);

        auto* order_row = new QHBoxLayout;
        remove_ = new QPushButton(QStringLiteral("Remove step"), this);
        remove_->setObjectName(QStringLiteral("bench-metadata-transformation-remove"));
        up_ = new QPushButton(QStringLiteral("Move up"), this);
        up_->setObjectName(QStringLiteral("bench-metadata-transformation-up"));
        down_ = new QPushButton(QStringLiteral("Move down"), this);
        down_->setObjectName(QStringLiteral("bench-metadata-transformation-down"));
        order_row->addWidget(remove_);
        order_row->addWidget(up_);
        order_row->addWidget(down_);
        order_row->addStretch(1);
        rules_layout->addLayout(order_row);
        rules_page->setToolTip(
            QStringLiteral("Steps run in order; each step sees the result of every earlier step"));
        editor_tabs_->addTab(rules_page, QStringLiteral("Steps"));

        auto* raw_page = new QWidget(editor_tabs_);
        auto* raw_layout = new QVBoxLayout(raw_page);
        raw_layout->setContentsMargins(6, 6, 6, 6);
        raw_source_ = new QPlainTextEdit(raw_page);
        raw_source_->setToolTip(
            QStringLiteral("Valid source compiles into the steps on the Steps tab; arbitrary "
                           "script is never executed"));
        raw_source_->setObjectName(QStringLiteral("bench-metadata-transformation-raw-source"));
        raw_source_->setPlaceholderText(
            QStringLiteral("$if($eq(%totaldiscs%,1),$delete(discnumber)$delete(totaldiscs))"));
        raw_layout->addWidget(raw_source_, 1);
        raw_diagnostics_ = new QPlainTextEdit(raw_page);
        raw_diagnostics_->setObjectName(
            QStringLiteral("bench-metadata-transformation-raw-diagnostics"));
        raw_diagnostics_->setReadOnly(true);
        raw_diagnostics_->setMaximumBlockCount(128);
        raw_diagnostics_->setMaximumHeight(110);
        raw_layout->addWidget(raw_diagnostics_);
        editor_tabs_->addTab(raw_page, QStringLiteral("Raw script"));
        editor_layout->addWidget(editor_tabs_, 1);
        content_splitter_->addWidget(editor_pane);

        auto* preview_pane = new QWidget(content_splitter_);
        auto* preview_layout = new QVBoxLayout(preview_pane);
        preview_layout->setContentsMargins(0, 0, 0, 0);
        preview_layout->setSpacing(6);
        auto* preview_heading = new QLabel(QStringLiteral("Preview"), preview_pane);
        auto preview_heading_font = preview_heading->font();
        preview_heading_font.setBold(true);
        preview_heading->setFont(preview_heading_font);
        preview_heading->setToolTip(
            QStringLiteral("Updates automatically as you edit; nothing enters the draft until "
                           "you add the previewed changes"));
        preview_layout->addWidget(preview_heading);
        summary_ = new QLabel(QStringLiteral("Add a step to see a preview."), preview_pane);
        summary_->setObjectName(QStringLiteral("bench-metadata-transformation-summary"));
        summary_->setWordWrap(true);
        preview_layout->addWidget(summary_);
        table_ = new QTreeView(preview_pane);
        table_->setObjectName(QStringLiteral("bench-metadata-transformation-table"));
        table_->setAlternatingRowColors(true);
        table_->setWordWrap(false);
        table_->setTextElideMode(Qt::ElideRight);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->setRootIsDecorated(true);
        table_->setItemsExpandable(true);
        table_->setExpandsOnDoubleClick(true);
        table_->setUniformRowHeights(true);
        table_->setIndentation(18);
        table_->header()->setSectionResizeMode(QHeaderView::Interactive);
        table_->header()->setStretchLastSection(true);
        table_->setColumnWidth(0, 150);
        table_->setColumnWidth(1, 200);
        preview_layout->addWidget(table_, 1);
        content_splitter_->addWidget(preview_pane);
        content_splitter_->setStretchFactor(0, 0);
        content_splitter_->setStretchFactor(1, 1);
        content_splitter_->setSizes({460, 600});
        layout->addWidget(content_splitter_, 1);

        catalog_status_ = new QLabel(this);
        catalog_status_->setObjectName(
            QStringLiteral("bench-metadata-transformation-catalog-status"));
        catalog_status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        buttons_ = new QDialogButtonBox(QDialogButtonBox::Close, this);
        stage_button_ =
            buttons_->addButton(QStringLiteral("Add to draft"), QDialogButtonBox::AcceptRole);
        stage_button_->setObjectName(QStringLiteral("bench-metadata-transformation-stage"));
        stage_button_->setEnabled(false);
        // No default button: Enter inside the step-entry fields adds a step
        // instead of accidentally staging and closing the dialog.
        stage_button_->setAutoDefault(false);
        if (auto* close_button = buttons_->button(QDialogButtonBox::Close)) {
            close_button->setAutoDefault(false);
        }
        auto* footer_row = new QHBoxLayout;
        footer_row->setSpacing(8);
        footer_row->addWidget(catalog_status_, 1);
        footer_row->addWidget(buttons_);
        layout->addLayout(footer_row);

        connect(kind_, &QComboBox::currentIndexChanged, this, [this] { updateInputForKind(); });
        connect(capture_source_, &QComboBox::currentIndexChanged, this,
                [this] { updateInputForKind(); });
        connect(target_, &QLineEdit::textChanged, this, [this](const QString& text) {
            target_completion_model_->setStringList(targetFieldSuggestions(text));
            if (text.trimmed().isEmpty() || target_completion_model_->rowCount() == 0) {
                return;
            }
            QTimer::singleShot(0, this, [this] {
                if (isVisible() && target_->hasFocus()) {
                    target_completer_->complete();
                }
            });
        });
        connect(name_, &QLineEdit::textChanged, this, [this] {
            if (!loading_definition_) {
                catalog_status_->setText(QStringLiteral("Unsaved name change · Save to keep it"));
            }
            updateActions();
        });
        connect(saved_, &QComboBox::currentIndexChanged, this,
                [this](const int index) { selectSaved(index); });
        connect(save_, &QPushButton::clicked, this, [this] { saveCurrent(false); });
        connect(save_as_, &QPushButton::clicked, this, [this] { saveCurrent(true); });
        connect(delete_saved_, &QPushButton::clicked, this, [this] { deleteSaved(); });
        connect(import_native_, &QPushButton::clicked, this, [this] { importNative(); });
        connect(export_native_, &QPushButton::clicked, this, [this] { exportNative(); });
        connect(add_, &QPushButton::clicked, this, [this] { addStep(); });
        for (auto* step_editor : {target_, input_, replacement_, capture_argument_}) {
            connect(step_editor, &QLineEdit::returnPressed, this, [this] { addStep(); });
        }
        connect(import_, &QPushButton::clicked, this, [this] { importRules(); });
        connect(raw_source_, &QPlainTextEdit::textChanged, this,
                [this] { updateRawTranslation(); });
        connect(remove_, &QPushButton::clicked, this, [this] { removeStep(); });
        connect(up_, &QPushButton::clicked, this, [this] { moveStep(-1); });
        connect(down_, &QPushButton::clicked, this, [this] { moveStep(1); });
        connect(steps_, &QListWidget::currentRowChanged, this, [this] { updateActions(); });
        preview_timer_ = new QTimer(this);
        preview_timer_->setObjectName(
            QStringLiteral("bench-metadata-transformation-preview-timer"));
        preview_timer_->setSingleShot(true);
        preview_timer_->setInterval(400);
        connect(preview_timer_, &QTimer::timeout, this, [this] { startPreview(); });
        connect(stage_button_, &QPushButton::clicked, this, [this] { stagePreview(); });
        connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::close);
        connect(&watcher_, &QFutureWatcherBase::finished, this, [this] { finishPreview(); });
        clean_chain_ = currentChain();
        refreshRawFromActions();
        updateInputForKind();
        updateActions();
        loadSaved();
        restoreLayoutState();
    }

    ~MetadataTransformationDialog() override {
        cancellation_.request_cancellation();
        if (planning_) {
            watcher_.waitForFinished();
        }
    }

  protected:
    void closeEvent(QCloseEvent* event) override {
        if (planning_) {
            close_requested_ = true;
            cancellation_.request_cancellation();
            summary_->setText(QStringLiteral("Cancelling preview…"));
            event->ignore();
            return;
        }
        if (hasUnsavedChanges()) {
            const auto answer = QMessageBox::warning(
                this, QStringLiteral("Discard unsaved script changes?"),
                QStringLiteral("This script differs from its saved version. Save it before "
                               "closing, or explicitly discard the changes."),
                QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
            if (answer != QMessageBox::Discard) {
                event->ignore();
                return;
            }
        }
        persistLayoutState();
        QDialog::closeEvent(event);
    }

  private:
    void restoreLayoutState() {
        if (!layout_store_.load) {
            return;
        }
        const QPointer self{this};
        layout_store_.load(QString::fromLatin1(transformation_geometry_key),
                           [self](QByteArray state, const QString& error) {
                               if (self && error.isEmpty() && !state.isEmpty()) {
                                   static_cast<void>(self->restoreGeometry(state));
                               }
                           });
        layout_store_.load(QString::fromLatin1(transformation_splitter_key),
                           [self](QByteArray state, const QString& error) {
                               if (self && error.isEmpty() && !state.isEmpty()) {
                                   static_cast<void>(self->content_splitter_->restoreState(state));
                               }
                           });
    }

    void persistLayoutState() {
        if (layout_state_saved_ || !layout_store_.save) {
            return;
        }
        layout_state_saved_ = true;
        layout_store_.save(QString::fromLatin1(transformation_geometry_key), saveGeometry(), {});
        layout_store_.save(QString::fromLatin1(transformation_splitter_key),
                           content_splitter_->saveState(), {});
    }

    [[nodiscard]] QStringList targetFieldSuggestions(const QString& query) const {
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

    [[nodiscard]] QString actionText(const metadata::MetadataTransformationAction& action,
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
                } else if constexpr (std::is_same_v<Action,
                                                    metadata::MetadataRemoveFieldIfAction>) {
                    return typed.match_mode == metadata::MetadataFieldMatchMode::exact_native
                               ? QStringLiteral("%1. Remove exact native field %2 when %3")
                                     .arg(index + 1U)
                                     .arg(field, display_utf8(typed.condition))
                               : QStringLiteral("%1. Remove %2 when %3")
                                     .arg(index + 1U)
                                     .arg(field, display_utf8(typed.condition));
                } else if constexpr (std::is_same_v<Action,
                                                    metadata::MetadataBlocklistFieldsAction>) {
                    QStringList fields;
                    for (const auto& name : typed.fields) {
                        fields.push_back(display_utf8(name));
                    }
                    return QStringLiteral("%1. Remove listed fields: %2")
                        .arg(index + 1U)
                        .arg(fields.join(QStringLiteral(", ")));
                } else if constexpr (std::is_same_v<Action,
                                                    metadata::MetadataAllowlistFieldsAction>) {
                    QStringList fields;
                    for (const auto& name : typed.fields) {
                        fields.push_back(display_utf8(name));
                    }
                    return QStringLiteral("%1. Keep only listed fields: %2")
                        .arg(index + 1U)
                        .arg(fields.join(QStringLiteral(", ")));
                } else if constexpr (std::is_same_v<Action,
                                                    metadata::MetadataTransformValuesAction>) {
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
                } else if constexpr (std::is_same_v<Action, metadata::MetadataSplitValuesAction>) {
                    return QStringLiteral("%1. Split %2 by %3")
                        .arg(index + 1U)
                        .arg(field, display_utf8(typed.separator));
                } else if constexpr (std::is_same_v<Action, metadata::MetadataJoinValuesAction>) {
                    const auto separator = typed.separator.empty()
                                               ? QStringLiteral("(empty separator)")
                                               : display_utf8(typed.separator);
                    return QStringLiteral("%1. Join %2 with %3")
                        .arg(index + 1U)
                        .arg(field, separator);
                } else if constexpr (std::is_same_v<Action,
                                                    metadata::MetadataRemoveMatchingValuesAction>) {
                    const auto match = typed.match.empty() ? QStringLiteral("(empty value)")
                                                           : display_utf8(typed.match);
                    return QStringLiteral("%1. Remove values of %2 equal to %3")
                        .arg(index + 1U)
                        .arg(field, match);
                } else if constexpr (std::is_same_v<
                                         Action, metadata::MetadataReplaceMatchingValuesAction>) {
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
                } else if constexpr (std::is_same_v<Action,
                                                    metadata::MetadataCaptureValuesAction>) {
                    QString source;
                    switch (typed.source_kind) {
                    case metadata::MetadataCaptureSourceKind::filename:
                        source = QStringLiteral("filename");
                        break;
                    case metadata::MetadataCaptureSourceKind::full_path:
                        source = QStringLiteral("full path");
                        break;
                    case metadata::MetadataCaptureSourceKind::formatted:
                        source =
                            QStringLiteral("formatted text %1").arg(display_utf8(typed.source));
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

    [[nodiscard]] QString capitalizationNoChangeSummary() const {
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

    void rebuildSteps(const int selected_row) {
        steps_->clear();
        for (std::size_t index = 0U; index < actions_.size(); ++index) {
            steps_->addItem(actionText(actions_[index], index));
        }
        if (!actions_.empty()) {
            steps_->setCurrentRow(
                std::clamp(selected_row, 0, static_cast<int>(actions_.size()) - 1));
        }
        updateActions();
    }

    void repopulateSaved(const std::optional<core::StableId>& selected = std::nullopt) {
        std::ranges::sort(catalog_, [](const auto& left, const auto& right) {
            if (left.chain.name != right.chain.name) {
                return left.chain.name < right.chain.name;
            }
            return left.id.to_string() < right.id.to_string();
        });
        const QSignalBlocker blocker{saved_};
        saved_->clear();
        saved_->addItem(QStringLiteral("New script"));
        auto selected_index = 0;
        for (std::size_t index = 0U; index < catalog_.size(); ++index) {
            const auto& entry = catalog_[index];
            saved_->addItem(display_utf8(entry.chain.name),
                            QString::fromStdString(entry.id.to_string()));
            if (selected && entry.id == *selected) {
                selected_index = static_cast<int>(index) + 1;
            }
        }
        saved_->setCurrentIndex(selected_index);
        selected_saved_ = selected_index > 0 ? selected : std::nullopt;
        updateActions();
    }

    void loadSaved() {
        if (!store_.load) {
            catalog_status_->setText(
                QStringLiteral("Saved scripts are unavailable in this session."));
            updateActions();
            return;
        }
        catalog_busy_ = true;
        catalog_status_->setText(QStringLiteral("Loading saved scripts…"));
        updateActions();
        const QPointer<MetadataTransformationDialog> self{this};
        store_.load([self](std::vector<persistence::SavedMetadataTransformationChain> chains,
                           QString error) mutable {
            if (!self) {
                return;
            }
            self->catalog_busy_ = false;
            if (!error.isEmpty()) {
                self->catalog_status_->setText(
                    QStringLiteral("Could not load saved scripts · %1").arg(error));
                self->updateActions();
                return;
            }
            self->catalog_ = std::move(chains);
            self->repopulateSaved(self->initially_selected_);
            if (self->initially_selected_) {
                self->selectSaved(self->saved_->currentIndex());
                if (self->preview_initially_selected_ && !self->actions_.empty()) {
                    QTimer::singleShot(0, self, [self] {
                        if (self) {
                            self->startPreview();
                        }
                    });
                }
            }
            self->catalog_status_->setText(QStringLiteral("%1 saved %2 available")
                                               .arg(self->catalog_.size())
                                               .arg(self->catalog_.size() == 1U
                                                        ? QStringLiteral("script")
                                                        : QStringLiteral("scripts")));
        });
    }

    void selectSaved(const int index) {
        if (catalog_busy_) {
            return;
        }
        loading_definition_ = true;
        if (index <= 0 || static_cast<std::size_t>(index) > catalog_.size()) {
            selected_saved_.reset();
            name_->setText(QStringLiteral("Untitled script"));
            actions_.clear();
            invalidatePreview();
            rebuildSteps(-1);
            clean_chain_ = currentChain();
            refreshRawFromActions();
            loading_definition_ = false;
            catalog_status_->setText(QStringLiteral("Editing a new script"));
            return;
        }
        const auto& selected = catalog_[static_cast<std::size_t>(index) - 1U];
        selected_saved_ = selected.id;
        name_->setText(display_utf8(selected.chain.name));
        actions_ = selected.chain.actions;
        invalidatePreview();
        rebuildSteps(0);
        clean_chain_ = currentChain();
        refreshRawFromActions();
        loading_definition_ = false;
        catalog_status_->setText(
            QStringLiteral("Loaded script · %1").arg(display_utf8(selected.chain.name)));
    }

    [[nodiscard]] metadata::MetadataTransformationChain currentChain() const {
        return metadata::MetadataTransformationChain{
            .schema_version = 1U,
            .name = encode_utf8(name_->text()),
            .actions = actions_,
        };
    }

    [[nodiscard]] bool hasUnsavedChanges() const {
        return raw_modified_ || !clean_chain_ || currentChain() != *clean_chain_;
    }

    [[nodiscard]] static QString interchangeErrorText(const core::Error& error) {
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

    [[nodiscard]] bool confirmDiscardBeforeImport() {
        if (!hasUnsavedChanges()) {
            return true;
        }
        QMessageBox confirmation{
            QMessageBox::Warning,
            QStringLiteral("Discard unsaved script changes?"),
            QStringLiteral("Importing a native tagging script replaces the current unsaved "
                           "editor contents. Explicitly discard those changes to continue."),
            QMessageBox::Discard | QMessageBox::Cancel,
            this,
        };
        confirmation.setDefaultButton(QMessageBox::Cancel);
        confirmation.setOption(QMessageBox::Option::DontUseNativeDialog);
        return confirmation.exec() == QMessageBox::Discard;
    }

    void importNative() {
        if (catalog_busy_ || !confirmDiscardBeforeImport()) {
            return;
        }
        const auto path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Import Trackknife tagging script"), {},
            QStringLiteral("Trackknife tagging scripts (*.tbtags.json *.json);;All files (*)"));
        if (path.isEmpty()) {
            return;
        }

        catalog_busy_ = true;
        catalog_status_->setText(QStringLiteral("Importing native tagging script…"));
        updateActions();
        auto* watcher = new QFutureWatcher<std::shared_ptr<NativeImportResult>>(this);
        const QPointer<MetadataTransformationDialog> self{this};
        connect(watcher, &QFutureWatcherBase::finished, this, [self, watcher] {
            const auto result = watcher->result();
            watcher->deleteLater();
            if (!self) {
                return;
            }
            self->catalog_busy_ = false;
            if (!result || !*result) {
                self->catalog_status_->setText(
                    QStringLiteral("Could not import native tagging script · %1")
                        .arg(result ? interchangeErrorText(result->error())
                                    : QStringLiteral("The import task returned no result")));
                self->updateActions();
                return;
            }

            auto chain = std::move(**result);
            self->loading_definition_ = true;
            self->selected_saved_.reset();
            {
                const QSignalBlocker blocker{self->saved_};
                self->saved_->setCurrentIndex(0);
            }
            self->name_->setText(display_utf8(chain.name));
            self->actions_ = std::move(chain.actions);
            self->clearPreview();
            self->rebuildSteps(0);
            self->refreshRawFromActions();
            self->clean_chain_.reset();
            self->loading_definition_ = false;
            self->invalidatePreview();
            self->catalog_status_->setText(
                QStringLiteral("Imported · review, preview, and Save to keep this script"));
            self->updateActions();
        });
        watcher->setFuture(QtConcurrent::run([path] {
            return std::make_shared<NativeImportResult>(
                ui::loadMetadataTransformationChainFile(path));
        }));
    }

    void exportNative() {
        if (catalog_busy_ || actions_.empty() || (raw_modified_ && !raw_valid_)) {
            return;
        }
        auto suggested_name = name_->text().trimmed();
        suggested_name.replace(QChar{'/'}, QChar{'_'});
        if (suggested_name.isEmpty()) {
            suggested_name = QStringLiteral("tagging-script");
        }
        suggested_name += QStringLiteral(".tbtags.json");
        const auto path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Export Trackknife tagging script"), suggested_name,
            QStringLiteral("Trackknife tagging scripts (*.tbtags.json);;JSON files (*.json);;All "
                           "files (*)"));
        if (path.isEmpty()) {
            return;
        }

        catalog_busy_ = true;
        catalog_status_->setText(QStringLiteral("Exporting native tagging script…"));
        updateActions();
        auto* watcher = new QFutureWatcher<std::shared_ptr<NativeExportResult>>(this);
        const QPointer<MetadataTransformationDialog> self{this};
        connect(watcher, &QFutureWatcherBase::finished, this, [self, watcher] {
            const auto result = watcher->result();
            watcher->deleteLater();
            if (!self) {
                return;
            }
            self->catalog_busy_ = false;
            if (!result || !*result) {
                self->catalog_status_->setText(
                    QStringLiteral("Could not export native tagging script · %1")
                        .arg(result ? interchangeErrorText(result->error())
                                    : QStringLiteral("The export task returned no result")));
            } else {
                self->catalog_status_->setText(QStringLiteral("Native tagging script exported"));
            }
            self->updateActions();
        });
        auto chain = currentChain();
        watcher->setFuture(QtConcurrent::run([path, chain = std::move(chain)] {
            return std::make_shared<NativeExportResult>(
                ui::saveMetadataTransformationChainFile(path, chain));
        }));
    }

    void refreshRawFromActions() {
        const QSignalBlocker blocker{raw_source_};
        raw_modified_ = false;
        raw_valid_ = false;
        if (actions_.empty()) {
            raw_source_->setReadOnly(false);
            raw_source_->clear();
            raw_diagnostics_->setPlainText(
                QStringLiteral("Enter cleanup source to generate typed rules."));
            updateActions();
            return;
        }

        const auto exported = metadata::export_metadata_rule_script(actions_);
        if (!exported) {
            raw_source_->clear();
            raw_source_->setReadOnly(true);
            auto message = QStringLiteral("Raw mode is unavailable: %1")
                               .arg(display_utf8(exported.error().message));
            for (const auto& context : exported.error().context) {
                if (context.key == "action") {
                    message += QStringLiteral(" · typed step %1")
                                   .arg(QString::fromStdString(context.value).toULongLong() + 1U);
                }
            }
            raw_diagnostics_->setPlainText(message);
            updateActions();
            return;
        }

        raw_source_->setReadOnly(false);
        raw_source_->setPlainText(display_utf8(*exported));
        raw_import_ = metadata::import_metadata_rule_script(*exported);
        raw_valid_ = !raw_import_.has_errors();
        raw_diagnostics_->setPlainText(
            QStringLiteral("Ready · %1 typed rules · canonical source is regenerated after "
                           "structured edits")
                .arg(actions_.size()));
        updateActions();
    }

    void updateRawTranslation() {
        if (raw_source_->isReadOnly()) {
            return;
        }
        raw_modified_ = true;
        raw_import_ =
            metadata::import_metadata_rule_script(encode_utf8(raw_source_->toPlainText()));
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
        if (raw_valid_) {
            actions_ = raw_import_.actions;
            diagnostics.prepend(
                QStringLiteral("Ready · %1 generated typed rules").arg(actions_.size()));
            invalidatePreview();
            rebuildSteps(static_cast<int>(actions_.size()) - 1);
        } else {
            invalidatePreview();
            updateActions();
        }
        raw_diagnostics_->setPlainText(diagnostics.join(QChar{'\n'}));
        catalog_status_->setText(
            raw_valid_
                ? QStringLiteral("Unsaved changes · Save to keep them")
                : QStringLiteral(
                      "Raw script has errors · the preview and Save wait until they are fixed"));
        updateActions();
    }

    void saveCurrent(const bool as_new) {
        if (catalog_busy_ || !store_.save || actions_.empty()) {
            return;
        }
        if (name_->text().trimmed().isEmpty()) {
            catalog_status_->setText(QStringLiteral("Enter a script name before saving."));
            name_->setFocus(Qt::OtherFocusReason);
            return;
        }
        auto chain = currentChain();
        if (const auto valid = metadata::validate_metadata_transformation_chain(chain); !valid) {
            catalog_status_->setText(
                QStringLiteral("Cannot save script · %1").arg(display_utf8(valid.error().message)));
            return;
        }
        persistence::SavedMetadataTransformationChain saved_chain{
            .id = !as_new && selected_saved_ ? *selected_saved_ : core::StableId::random(),
            .chain = std::move(chain),
            .automatic = false,
        };
        if (!as_new && selected_saved_) {
            const auto existing = std::ranges::find(
                catalog_, *selected_saved_, &persistence::SavedMetadataTransformationChain::id);
            if (existing != catalog_.end()) {
                saved_chain.automatic = existing->automatic;
            }
        }
        catalog_busy_ = true;
        catalog_status_->setText(QStringLiteral("Saving…"));
        updateActions();
        const QPointer<MetadataTransformationDialog> self{this};
        auto retained_chain = saved_chain;
        store_.save(std::move(saved_chain),
                    [self, saved_chain = std::move(retained_chain)](QString error) mutable {
                        if (!self) {
                            return;
                        }
                        self->catalog_busy_ = false;
                        if (!error.isEmpty()) {
                            self->catalog_status_->setText(
                                QStringLiteral("Could not save script · %1").arg(error));
                            self->updateActions();
                            return;
                        }
                        const auto found =
                            std::ranges::find(self->catalog_, saved_chain.id,
                                              &persistence::SavedMetadataTransformationChain::id);
                        if (found == self->catalog_.end()) {
                            self->catalog_.push_back(saved_chain);
                        } else {
                            *found = saved_chain;
                        }
                        self->repopulateSaved(saved_chain.id);
                        self->clean_chain_ = saved_chain.chain;
                        self->raw_modified_ = false;
                        self->updateActions();
                        self->catalog_status_->setText(
                            QStringLiteral("Saved · %1").arg(display_utf8(saved_chain.chain.name)));
                    });
    }

    void deleteSaved() {
        if (catalog_busy_ || !store_.remove || !selected_saved_) {
            return;
        }
        const auto id = *selected_saved_;
        catalog_busy_ = true;
        catalog_status_->setText(QStringLiteral("Deleting saved script…"));
        updateActions();
        const QPointer<MetadataTransformationDialog> self{this};
        store_.remove(id, [self, id](QString error) {
            if (!self) {
                return;
            }
            self->catalog_busy_ = false;
            if (!error.isEmpty()) {
                self->catalog_status_->setText(
                    QStringLiteral("Could not delete saved script · %1").arg(error));
                self->updateActions();
                return;
            }
            std::erase_if(self->catalog_, [id](const auto& entry) { return entry.id == id; });
            self->selected_saved_.reset();
            self->repopulateSaved();
            self->name_->setText(QStringLiteral("Untitled script"));
            self->actions_.clear();
            self->invalidatePreview();
            self->rebuildSteps(-1);
            self->clean_chain_ = self->currentChain();
            self->refreshRawFromActions();
            self->catalog_status_->setText(QStringLiteral("Saved script deleted"));
        });
    }

    // The action kind is stored as item data; header rows have none and are
    // not selectable.
    [[nodiscard]] int currentStepKind() const {
        const auto kind_data = kind_->currentData();
        return kind_data.isValid() ? kind_data.toInt() : -1;
    }

    void updateInputForKind() {
        const auto kind = currentStepKind();
        const auto captures = kind == 16;
        const auto filters_fields = kind == 18 || kind == 19;
        target_label_->setVisible(!captures && !filters_fields);
        target_->setVisible(!captures && !filters_fields);
        const auto has_input = kind == 0 || kind == 1 || (kind >= 7 && kind <= 12) || kind == 15 ||
                               captures || filters_fields;
        input_label_->setVisible(has_input);
        input_->setVisible(has_input);
        const auto has_replacement = kind == 12;
        replacement_label_->setVisible(has_replacement);
        replacement_->setVisible(has_replacement);
        const auto has_numbering = kind == 13;
        number_start_label_->setVisible(has_numbering);
        number_start_->setVisible(has_numbering);
        number_padding_label_->setVisible(has_numbering);
        number_padding_->setVisible(has_numbering);
        const auto keeps_first = kind == 14;
        character_count_label_->setVisible(keeps_first);
        character_count_->setVisible(keeps_first);
        capture_source_label_->setVisible(captures);
        capture_source_->setVisible(captures);
        const auto has_capture_argument = captures && capture_source_->currentIndex() >= 2;
        capture_argument_label_->setVisible(has_capture_argument);
        capture_argument_->setVisible(has_capture_argument);
        if (kind == 7) {
            input_label_->setText(QStringLiteral("Source field:"));
            input_->setPlaceholderText(QStringLiteral("For example: Artist"));
        } else if (kind == 8 || kind == 9) {
            input_label_->setText(QStringLiteral("Separator:"));
            input_->setPlaceholderText(kind == 8 ? QStringLiteral("Required exact separator")
                                                 : QStringLiteral("May be empty"));
        } else if (kind == 10) {
            input_label_->setText(QStringLiteral("Expression:"));
            input_->setPlaceholderText(QStringLiteral("For example: %artist% — %title%"));
        } else if (kind == 11 || kind == 12) {
            input_label_->setText(QStringLiteral("Exact value:"));
            input_->setPlaceholderText(QStringLiteral("Case-sensitive; may be empty"));
        } else if (kind == 15) {
            input_label_->setText(QStringLiteral("Condition:"));
            input_->setPlaceholderText(QStringLiteral("For example: $not(%totaldiscs%)"));
        } else if (captures) {
            input_label_->setText(QStringLiteral("Capture pattern:"));
            input_->setPlaceholderText(QStringLiteral("For example: %tracknumber%. %title%"));
            const auto from_field = capture_source_->currentIndex() == 3;
            capture_argument_label_->setText(from_field ? QStringLiteral("Source field:")
                                                        : QStringLiteral("Source expression:"));
            capture_argument_->setPlaceholderText(
                from_field ? QStringLiteral("For example: Comment")
                           : QStringLiteral("For example: %artist% — %title%"));
        } else if (filters_fields) {
            input_label_->setText(QStringLiteral("Fields:"));
            input_->setPlaceholderText(
                QStringLiteral("Comma-separated, for example: Comment, Encoder"));
        } else {
            input_label_->setText(QStringLiteral("Value:"));
            input_->setPlaceholderText(QString{});
        }
    }

    void addStep() {
        const auto kind = currentStepKind();
        const auto field = target_->text().trimmed();
        if (kind != 16 && kind != 18 && kind != 19 && field.isEmpty()) {
            summary_->setText(QStringLiteral("Enter a target field before adding the step."));
            target_->setFocus(Qt::OtherFocusReason);
            return;
        }
        const auto target = encode_utf8(field);
        switch (kind) {
        case 0:
            actions_.push_back(metadata::MetadataSetValuesAction{
                .target_field = target, .values = {encode_utf8(input_->text())}});
            break;
        case 1:
            actions_.push_back(metadata::MetadataAddValuesAction{
                .target_field = target, .values = {encode_utf8(input_->text())}});
            break;
        case 2:
            actions_.push_back(metadata::MetadataRemoveFieldAction{.target_field = target});
            break;
        case 3:
            actions_.push_back(metadata::MetadataTransformValuesAction{
                .target_field = target,
                .transform = metadata::MetadataValueTransformKind::trim_ascii});
            break;
        case 4:
            actions_.push_back(metadata::MetadataTransformValuesAction{
                .target_field = target,
                .transform = metadata::MetadataValueTransformKind::lowercase});
            break;
        case 5:
            actions_.push_back(metadata::MetadataTransformValuesAction{
                .target_field = target,
                .transform = metadata::MetadataValueTransformKind::uppercase});
            break;
        case 6:
            actions_.push_back(metadata::MetadataTransformValuesAction{
                .target_field = target,
                .transform = metadata::MetadataValueTransformKind::capitalize_first});
            break;
        case 7: {
            const auto source = input_->text().trimmed();
            if (source.isEmpty()) {
                summary_->setText(QStringLiteral("Enter a source field for the copy step."));
                input_->setFocus(Qt::OtherFocusReason);
                return;
            }
            actions_.push_back(metadata::MetadataCopyFieldAction{
                .target_field = target, .source_field = encode_utf8(source)});
            break;
        }
        case 8:
            if (input_->text().isEmpty()) {
                summary_->setText(QStringLiteral("Split requires a non-empty exact separator."));
                input_->setFocus(Qt::OtherFocusReason);
                return;
            }
            actions_.push_back(metadata::MetadataSplitValuesAction{
                .target_field = target, .separator = encode_utf8(input_->text())});
            break;
        case 9:
            actions_.push_back(metadata::MetadataJoinValuesAction{
                .target_field = target, .separator = encode_utf8(input_->text())});
            break;
        case 10:
            actions_.push_back(metadata::MetadataFormatValueAction{
                .target_field = target, .dialect = {}, .source = encode_utf8(input_->text())});
            break;
        case 11:
            actions_.push_back(metadata::MetadataRemoveMatchingValuesAction{
                .target_field = target, .match = encode_utf8(input_->text())});
            break;
        case 12:
            actions_.push_back(metadata::MetadataReplaceMatchingValuesAction{
                .target_field = target,
                .match = encode_utf8(input_->text()),
                .replacement_values = {encode_utf8(replacement_->text())},
            });
            break;
        case 13:
            actions_.push_back(metadata::MetadataNumberSelectedItemsAction{
                .target_field = target,
                .start = static_cast<std::uint32_t>(number_start_->value()),
                .padding = static_cast<std::uint32_t>(number_padding_->value()),
            });
            break;
        case 14:
            actions_.push_back(metadata::MetadataKeepFirstCharactersAction{
                .target_field = target,
                .character_count = static_cast<std::uint32_t>(character_count_->value()),
            });
            break;
        case 15:
            if (input_->text().trimmed().isEmpty()) {
                summary_->setText(
                    QStringLiteral("Conditional removal requires a non-empty condition."));
                input_->setFocus(Qt::OtherFocusReason);
                return;
            }
            actions_.push_back(metadata::MetadataRemoveFieldIfAction{
                .target_field = target,
                .dialect = {},
                .condition = encode_utf8(input_->text()),
            });
            break;
        case 16: {
            if (input_->text().isEmpty()) {
                summary_->setText(QStringLiteral("Enter a tkcapture-1 pattern."));
                input_->setFocus(Qt::OtherFocusReason);
                return;
            }
            const auto source_kind =
                static_cast<metadata::MetadataCaptureSourceKind>(capture_source_->currentIndex());
            const auto needs_argument =
                source_kind == metadata::MetadataCaptureSourceKind::formatted ||
                source_kind == metadata::MetadataCaptureSourceKind::field;
            if (needs_argument && capture_argument_->text().trimmed().isEmpty()) {
                summary_->setText(QStringLiteral("Enter the capture source argument."));
                capture_argument_->setFocus(Qt::OtherFocusReason);
                return;
            }
            const auto source = source_kind == metadata::MetadataCaptureSourceKind::field
                                    ? capture_argument_->text().trimmed()
                                    : capture_argument_->text();
            actions_.push_back(metadata::MetadataCaptureValuesAction{
                .dialect = {},
                .source_kind = source_kind,
                .source = needs_argument ? encode_utf8(source) : std::string{},
                .pattern = encode_utf8(input_->text()),
            });
            break;
        }
        case 18:
        case 19: {
            std::vector<std::string> fields;
            for (const auto& part : input_->text().split(QChar{','}, Qt::SkipEmptyParts)) {
                const auto trimmed = part.trimmed();
                if (!trimmed.isEmpty()) {
                    fields.push_back(encode_utf8(trimmed));
                }
            }
            if (fields.empty()) {
                summary_->setText(QStringLiteral("Enter at least one comma-separated field name."));
                input_->setFocus(Qt::OtherFocusReason);
                return;
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
            return;
        }
        invalidatePreview();
        rebuildSteps(static_cast<int>(actions_.size()) - 1);
        refreshRawFromActions();
        catalog_status_->setText(QStringLiteral("Unsaved changes · Save to keep them"));
        target_->clear();
        input_->clear();
        replacement_->clear();
        if (kind == 16) {
            input_->setFocus(Qt::OtherFocusReason);
        } else {
            target_->setFocus(Qt::OtherFocusReason);
        }
    }

    void removeStep() {
        const auto row = steps_->currentRow();
        if (row < 0 || static_cast<std::size_t>(row) >= actions_.size()) {
            return;
        }
        actions_.erase(actions_.begin() + row);
        invalidatePreview();
        rebuildSteps(std::min(row, static_cast<int>(actions_.size()) - 1));
        refreshRawFromActions();
        catalog_status_->setText(QStringLiteral("Unsaved changes · Save to keep them"));
    }

    void importRules() {
        MetadataRuleScriptImportDialog dialog{this};
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        const auto raw_source = dialog.source();
        auto imported = dialog.takeActions();
        if (dialog.importMode() == MetadataRuleScriptImportDialog::ImportMode::append) {
            if (actions_.size() > 256U || imported.size() > 256U - actions_.size()) {
                summary_->setText(
                    QStringLiteral("Appending those rules would exceed the 256-step limit."));
                return;
            }
            actions_.insert(actions_.end(), std::make_move_iterator(imported.begin()),
                            std::make_move_iterator(imported.end()));
        } else {
            actions_ = std::move(imported);
        }
        invalidatePreview();
        rebuildSteps(static_cast<int>(actions_.size()) - 1);
        if (dialog.importMode() == MetadataRuleScriptImportDialog::ImportMode::replace) {
            const QSignalBlocker blocker{raw_source_};
            raw_source_->setReadOnly(false);
            raw_source_->setPlainText(display_utf8(raw_source));
            raw_import_ = metadata::import_metadata_rule_script(raw_source);
            raw_valid_ = !raw_import_.has_errors() && !raw_import_.actions.empty();
            raw_modified_ = true;
            raw_diagnostics_->setPlainText(
                QStringLiteral("Ready · %1 generated typed rules · unsaved").arg(actions_.size()));
        } else {
            refreshRawFromActions();
        }
        catalog_status_->setText(
            QStringLiteral("Unsaved · generated %1 typed rules from the pasted script. Review, "
                           "preview, then click Save to keep them.")
                .arg(actions_.size()));
        updateActions();
    }

    void moveStep(const int offset) {
        const auto row = steps_->currentRow();
        const auto destination = row + offset;
        if (row < 0 || destination < 0 ||
            static_cast<std::size_t>(destination) >= actions_.size()) {
            return;
        }
        std::swap(actions_[static_cast<std::size_t>(row)],
                  actions_[static_cast<std::size_t>(destination)]);
        invalidatePreview();
        rebuildSteps(destination);
        refreshRawFromActions();
        catalog_status_->setText(QStringLiteral("Unsaved changes · Save to keep them"));
    }

    void clearPreview() {
        preview_.reset();
        if (auto* model = table_->model()) {
            table_->setModel(nullptr);
            model->deleteLater();
        }
        stage_button_->setEnabled(false);
    }

    // Invalidation schedules a fresh debounced preview whenever the current
    // script could produce one, so the preview pane tracks edits by itself.
    void invalidatePreview() {
        clearPreview();
        if (!planning_) {
            summary_->setText(actions_.empty() ? QStringLiteral("Add a step to see a preview.")
                                               : QStringLiteral("Updating preview…"));
        }
        if (!actions_.empty() && (!raw_modified_ || raw_valid_)) {
            preview_timer_->start();
        } else {
            preview_timer_->stop();
        }
    }

    void updateActions() {
        const auto row = steps_->currentRow();
        const auto valid = row >= 0 && static_cast<std::size_t>(row) < actions_.size();
        const auto editing_enabled = !planning_ && !catalog_busy_;
        const auto raw_ready = !raw_modified_ || raw_valid_;
        const auto unsaved = hasUnsavedChanges();
        setWindowModified(unsaved);
        save_->setText(selected_saved_ ? QStringLiteral("Save changes") : QStringLiteral("Save"));
        saved_->setEnabled(editing_enabled && static_cast<bool>(store_.load) && !unsaved);
        save_->setEnabled(editing_enabled && static_cast<bool>(store_.save) && unsaved &&
                          raw_ready && !actions_.empty() && !name_->text().trimmed().isEmpty());
        save_as_->setEnabled(editing_enabled && static_cast<bool>(store_.save) && raw_ready &&
                             !actions_.empty() && !name_->text().trimmed().isEmpty());
        delete_saved_->setEnabled(editing_enabled && static_cast<bool>(store_.remove) &&
                                  selected_saved_.has_value());
        import_native_->setEnabled(editing_enabled);
        export_native_->setEnabled(editing_enabled && raw_ready && !actions_.empty() &&
                                   !name_->text().trimmed().isEmpty());
        name_->setEnabled(editing_enabled);
        kind_->setEnabled(editing_enabled);
        target_->setEnabled(editing_enabled);
        input_->setEnabled(editing_enabled);
        replacement_->setEnabled(editing_enabled);
        number_start_->setEnabled(editing_enabled);
        number_padding_->setEnabled(editing_enabled);
        character_count_->setEnabled(editing_enabled);
        capture_source_->setEnabled(editing_enabled);
        capture_argument_->setEnabled(editing_enabled);
        raw_source_->setEnabled(editing_enabled);
        import_->setEnabled(editing_enabled);
        add_->setEnabled(editing_enabled && actions_.size() < 256U);
        steps_->setEnabled(editing_enabled);
        remove_->setEnabled(editing_enabled && valid);
        up_->setEnabled(editing_enabled && valid && row > 0);
        down_->setEnabled(editing_enabled && valid && row + 1 < steps_->count());
        stage_button_->setEnabled(editing_enabled && preview_ != nullptr &&
                                  !preview_->cells.empty());
    }

    void startPreview() {
        if (actions_.empty() || (raw_modified_ && !raw_valid_)) {
            return;
        }
        if (planning_) {
            preview_timer_->start();
            return;
        }
        clearPreview();
        cancellation_.request_cancellation();
        cancellation_ = core::CancellationSource{};
        auto chain = currentChain();
        const auto selection = selection_;
        const auto draft = draft_;
        const auto items = item_indexes_;
        const auto cancellation = cancellation_.token();
        planning_ = true;
        summary_->setText(QStringLiteral("Updating preview…"));
        updateActions();
        watcher_.setFuture(QtConcurrent::run(
            [selection, draft, items, chain = std::move(chain), cancellation]() mutable {
                return std::make_shared<PreviewResult>(metadata::plan_metadata_transformation(
                    *selection, draft, items, std::move(chain), cancellation));
            }));
    }

    void finishPreview() {
        planning_ = false;
        if (close_requested_) {
            close();
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
            summary_->setText(QStringLiteral("Transformation preview failed · %1").arg(message));
            updateActions();
            return;
        }
        preview_ =
            std::make_shared<const metadata::MetadataTransformationPreview>(std::move(**result));
        auto* old_model = table_->model();
        table_->setModel(createMetadataTransformationPreviewModel(preview_, track_labels_, table_));
        if (old_model != nullptr) {
            old_model->deleteLater();
        }
        const auto capitalization_summary = capitalizationNoChangeSummary();
        summary_->setText(
            preview_->cells.empty()
                ? (capitalization_summary.isEmpty()
                       ? QStringLiteral("The script produces no changes for the selected files.")
                       : capitalization_summary)
                : QStringLiteral("%1 final cell %2 across %3 selected %4 · add to draft when "
                                 "ready")
                      .arg(preview_->cells.size())
                      .arg(preview_->cells.size() == 1U ? QStringLiteral("change")
                                                        : QStringLiteral("changes"))
                      .arg(preview_->changed_item_count)
                      .arg(preview_->changed_item_count == 1U ? QStringLiteral("file")
                                                              : QStringLiteral("files")));
        updateActions();
    }

    void stagePreview() {
        if (!preview_ || preview_->cells.empty() || !stage_) {
            return;
        }
        if (!stage_(*preview_)) {
            invalidatePreview();
            summary_->setText(QStringLiteral(
                "The preview is stale or could not fit in the draft. It will refresh "
                "automatically."));
            return;
        }
        accept();
    }

    QFutureWatcher<std::shared_ptr<PreviewResult>> watcher_;
    std::shared_ptr<const metadata::StagedMetadataSelection> selection_;
    metadata::StagedMetadataPatchSet draft_;
    std::vector<std::size_t> item_indexes_;
    QStringList track_labels_;
    StageCallback stage_;
    MetadataTransformationStore store_;
    std::optional<core::StableId> initially_selected_;
    bool preview_initially_selected_{false};
    MetadataDialogLayoutStore layout_store_;
    std::vector<persistence::SavedMetadataTransformationChain> catalog_;
    std::optional<core::StableId> selected_saved_;
    std::vector<metadata::MetadataTransformationAction> actions_;
    std::optional<metadata::MetadataTransformationChain> clean_chain_;
    metadata::MetadataRuleScriptImportResult raw_import_;
    std::shared_ptr<const metadata::MetadataTransformationPreview> preview_;
    core::CancellationSource cancellation_;
    QComboBox* saved_{nullptr};
    QPushButton* save_{nullptr};
    QPushButton* save_as_{nullptr};
    QPushButton* delete_saved_{nullptr};
    QPushButton* import_native_{nullptr};
    QPushButton* export_native_{nullptr};
    QLabel* catalog_status_{nullptr};
    QLineEdit* name_{nullptr};
    QTabWidget* editor_tabs_{nullptr};
    QComboBox* kind_{nullptr};
    QLabel* target_label_{nullptr};
    QLineEdit* target_{nullptr};
    std::vector<metadata::MetadataFieldSuggestionCandidate> target_field_candidates_;
    QStringListModel* target_completion_model_{nullptr};
    QCompleter* target_completer_{nullptr};
    QLabel* input_label_{nullptr};
    QLineEdit* input_{nullptr};
    QLabel* replacement_label_{nullptr};
    QLineEdit* replacement_{nullptr};
    QLabel* number_start_label_{nullptr};
    QSpinBox* number_start_{nullptr};
    QLabel* number_padding_label_{nullptr};
    QSpinBox* number_padding_{nullptr};
    QLabel* character_count_label_{nullptr};
    QSpinBox* character_count_{nullptr};
    QLabel* capture_source_label_{nullptr};
    QComboBox* capture_source_{nullptr};
    QLabel* capture_argument_label_{nullptr};
    QLineEdit* capture_argument_{nullptr};
    QPushButton* add_{nullptr};
    QPushButton* import_{nullptr};
    QPlainTextEdit* raw_source_{nullptr};
    QPlainTextEdit* raw_diagnostics_{nullptr};
    QListWidget* steps_{nullptr};
    QPushButton* remove_{nullptr};
    QPushButton* up_{nullptr};
    QPushButton* down_{nullptr};
    QTimer* preview_timer_{nullptr};
    QLabel* summary_{nullptr};
    QTreeView* table_{nullptr};
    QDialogButtonBox* buttons_{nullptr};
    QPushButton* stage_button_{nullptr};
    QSplitter* content_splitter_{nullptr};
    bool planning_{false};
    bool catalog_busy_{false};
    bool close_requested_{false};
    bool loading_definition_{false};
    bool raw_modified_{false};
    bool raw_valid_{false};
    bool layout_state_saved_{false};
};

} // namespace

QDialog* createMetadataTransformationDialog(
    std::shared_ptr<const metadata::StagedMetadataSelection> selection,
    metadata::StagedMetadataPatchSet draft, std::vector<std::size_t> item_indexes,
    QStringList track_labels, MetadataTransformationStageCallback stage,
    MetadataTransformationStore store, QWidget* parent,
    std::optional<core::StableId> initially_selected, const bool preview_initially_selected,
    MetadataDialogLayoutStore layout_store) {
    return new MetadataTransformationDialog(
        std::move(selection), std::move(draft), std::move(item_indexes), std::move(track_labels),
        std::move(stage), std::move(store), parent, initially_selected, preview_initially_selected,
        std::move(layout_store));
}

} // namespace trackknife::bench
