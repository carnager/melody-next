// SPDX-License-Identifier: GPL-3.0-only

#include "bench/metadata_transformation_dialog.hpp"

#include "bench/metadata_dialog_helpers.hpp"
#include "bench/metadata_rule_script_import_dialog.hpp"
#include "bench/metadata_transformation_preview_model.hpp"
#include "trackknife/metadata/draft_document.hpp"
#include "trackknife/metadata/field_suggestions.hpp"
#include "trackknife/metadata/rule_script_import.hpp"
#include "uicommon/metadata_transformation_interchange.hpp"
#include "workspace/language_reference.hpp"
#include "workspace/script_session.hpp"

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
    MetadataTransformationDialog(ScriptSession* session, QWidget* parent,
                                 MetadataDialogLayoutStore layout_store)
        : QDialog(parent), session_(session), layout_store_(std::move(layout_store)) {
        session_->setParent(this);
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
        name_ = new QLineEdit(session_->name(), this);
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
        // Kinds are grouped under unselectable headings; the row therefore does
        // not match the action kind, which lives in the item data.
        for (const auto& kind : ScriptSession::stepKinds()) {
            if (kind.kind < 0) {
                kind_->addItem(kind.label);
                auto* model = qobject_cast<QStandardItemModel*>(kind_->model());
                auto* item = model->item(kind_->count() - 1);
                item->setFlags(Qt::NoItemFlags);
                auto header_font = item->font();
                header_font.setBold(true);
                item->setFont(header_font);
                continue;
            }
            kind_->addItem(kind.label, kind.kind);
            if (!kind.tool_tip.isEmpty()) {
                kind_->setItemData(kind_->count() - 1, kind.tool_tip, Qt::ToolTipRole);
            }
        }
        kind_->setCurrentIndex(ScriptSession::initialStepKind());
        target_label_ = new QLabel(QStringLiteral("Target field:"), this);
        target_ = new QLineEdit(this);
        target_->setObjectName(QStringLiteral("bench-metadata-transformation-target"));
        target_->setPlaceholderText(QStringLiteral("For example: Title or ALBUM ARTIST"));
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
        target_completion_model_->setStringList(session_->targetSuggestions({}));
        input_label_ = new QLabel(QStringLiteral("Value:"), this);
        input_ = new QLineEdit(this);
        input_->setObjectName(QStringLiteral("bench-metadata-transformation-input"));
        // ADR-0178 field filters complete each comma-separated name.
        fields_completion_model_ = new QStringListModel(this);
        fields_completion_model_->setObjectName(
            QStringLiteral("bench-metadata-transformation-fields-completions"));
        fields_completer_ = new QCompleter(fields_completion_model_, this);
        fields_completer_->setObjectName(
            QStringLiteral("bench-metadata-transformation-fields-completer"));
        fields_completer_->setCaseSensitivity(Qt::CaseInsensitive);
        fields_completer_->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
        fields_completer_->setMaxVisibleItems(12);
        fields_completer_->setWidget(input_);
        connect(fields_completer_, QOverload<const QString&>::of(&QCompleter::activated), this,
                [this](const QString& name) { insertFieldsSuggestion(name); });
        connect(input_, &QLineEdit::textEdited, this, [this] { refreshFieldsCompleter(true); });
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
        capture_source_->addItems(ScriptSession::captureSources());
        rating_scale_label_ = new QLabel(QStringLiteral("Its scale:"), this);
        rating_scale_ = new QComboBox(this);
        rating_scale_->setObjectName(QStringLiteral("bench-metadata-transformation-rating-scale"));
        rating_scale_->addItems(ScriptSession::ratingScales());
        rating_scale_->setToolTip(
            QStringLiteral("The scale the other player kept the rating on; it becomes 0.0-1.0"));
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
        step_form->addRow(rating_scale_label_, rating_scale_);
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
        // What can be written here, in the browser.
        auto* reference_row = new QHBoxLayout;
        reference_row->addStretch(1);
        const auto reference = [this, raw_page,
                                reference_row](const QString& name, const QString& text,
                                               const QString& tip, const LanguageReference which) {
            auto* button = new QPushButton(text, raw_page);
            button->setObjectName(name);
            button->setFlat(true);
            button->setToolTip(tip);
            connect(button, &QPushButton::clicked, this, [this, which] {
                if (QString error; !openLanguageReference(which, &error)) {
                    QMessageBox::warning(this, QStringLiteral("Reference"), error);
                }
            });
            reference_row->addWidget(button);
        };
        reference(QStringLiteral("bench-metadata-transformation-script-reference"),
                  QStringLiteral("Script steps"),
                  QStringLiteral("Every step a script can have, written as text"),
                  LanguageReference::scripts);
        reference(QStringLiteral("bench-metadata-transformation-tkfmt-reference"),
                  QStringLiteral("tkfmt-1 reference"),
                  QStringLiteral("The language of values and conditions"),
                  LanguageReference::formatting);
        raw_layout->addLayout(reference_row);
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
        summary_ = new QLabel(preview_pane);
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
            target_completion_model_->setStringList(session_->targetSuggestions(text));
            if (text.trimmed().isEmpty() || target_completion_model_->rowCount() == 0) {
                return;
            }
            QTimer::singleShot(0, this, [this] {
                if (isVisible() && target_->hasFocus()) {
                    target_completer_->complete();
                }
            });
        });
        connect(name_, &QLineEdit::textChanged, session_, &ScriptSession::setName);
        connect(saved_, &QComboBox::currentIndexChanged, session_, &ScriptSession::selectSaved);
        connect(save_, &QPushButton::clicked, this, [this] { focus(session_->save(false)); });
        connect(save_as_, &QPushButton::clicked, this, [this] { focus(session_->save(true)); });
        connect(delete_saved_, &QPushButton::clicked, session_, &ScriptSession::deleteSaved);
        connect(import_native_, &QPushButton::clicked, this, [this] { importNative(); });
        connect(export_native_, &QPushButton::clicked, this, [this] { exportNative(); });
        connect(add_, &QPushButton::clicked, this, [this] { addStep(); });
        for (auto* step_editor : {target_, input_, replacement_, capture_argument_}) {
            connect(step_editor, &QLineEdit::returnPressed, this, [this] { addStep(); });
        }
        connect(import_, &QPushButton::clicked, this, [this] { importRules(); });
        connect(raw_source_, &QPlainTextEdit::textChanged, this,
                [this] { session_->setRawSource(raw_source_->toPlainText()); });
        connect(remove_, &QPushButton::clicked, this,
                [this] { session_->removeStep(steps_->currentRow()); });
        connect(up_, &QPushButton::clicked, this,
                [this] { session_->moveStep(steps_->currentRow(), -1); });
        connect(down_, &QPushButton::clicked, this,
                [this] { session_->moveStep(steps_->currentRow(), 1); });
        connect(steps_, &QListWidget::currentRowChanged, this, [this] { sync(); });
        connect(stage_button_, &QPushButton::clicked, session_, &ScriptSession::stage);
        connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::close);

        connect(session_, &ScriptSession::changed, this, [this] { sync(); });
        connect(session_, &ScriptSession::stepsChanged, this,
                &MetadataTransformationDialog::rebuildSteps);
        connect(session_, &ScriptSession::savedChanged, this,
                &MetadataTransformationDialog::rebuildSaved);
        connect(session_, &ScriptSession::rawChanged, this, [this] {
            const QSignalBlocker blocker{raw_source_};
            raw_source_->setReadOnly(session_->rawReadOnly());
            if (raw_source_->toPlainText() != session_->rawSource()) {
                raw_source_->setPlainText(session_->rawSource());
            }
        });
        connect(session_, &ScriptSession::previewChanged, this,
                [this] { table_->setModel(session_->preview()); });
        connect(session_, &ScriptSession::accepted, this, &QDialog::accept);
        connect(session_, &ScriptSession::closeRequested, this, &QDialog::close);
        {
            const QSignalBlocker blocker{raw_source_};
            raw_source_->setReadOnly(session_->rawReadOnly());
            raw_source_->setPlainText(session_->rawSource());
        }
        // What the session already has: a store may answer at once.
        rebuildSaved();
        rebuildSteps(0);
        table_->setModel(session_->preview());
        updateInputForKind();
        sync();
        restoreLayoutState();
    }

  protected:
    void closeEvent(QCloseEvent* event) override {
        const auto answer = session_->requestClose();
        if (answer == ScriptSession::CloseAnswer::wait) {
            event->ignore();
            return;
        }
        if (answer == ScriptSession::CloseAnswer::confirm_discard) {
            const auto discard = QMessageBox::warning(
                this, QStringLiteral("Discard unsaved script changes?"),
                QStringLiteral("This script differs from its saved version. Save it before "
                               "closing, or explicitly discard the changes."),
                QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
            if (discard != QMessageBox::Discard) {
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

    void rebuildSteps(const int select) {
        const QSignalBlocker blocker{steps_};
        steps_->clear();
        steps_->addItems(session_->steps());
        if (steps_->count() > 0) {
            steps_->setCurrentRow(std::clamp(select, 0, steps_->count() - 1));
        }
        sync();
    }

    void rebuildSaved() {
        const QSignalBlocker blocker{saved_};
        saved_->clear();
        saved_->addItems(session_->savedNames());
        saved_->setCurrentIndex(session_->savedIndex());
    }

    void focus(const ScriptSession::Focus where) {
        switch (where) {
        case ScriptSession::Focus::target:
            target_->setFocus(Qt::OtherFocusReason);
            break;
        case ScriptSession::Focus::input:
            input_->setFocus(Qt::OtherFocusReason);
            break;
        case ScriptSession::Focus::capture_argument:
            capture_argument_->setFocus(Qt::OtherFocusReason);
            break;
        case ScriptSession::Focus::name:
            name_->setFocus(Qt::OtherFocusReason);
            break;
        case ScriptSession::Focus::none:
            break;
        }
    }

    void refreshFieldsCompleter(const bool popup) {
        if (!ScriptSession::stepForm(currentStepKind(), capture_source_->currentIndex())
                 .field_list) {
            return;
        }
        const auto names = session_->fieldListSuggestions(input_->text());
        fields_completion_model_->setStringList(names);
        if (popup && !names.isEmpty()) {
            QTimer::singleShot(0, this, [this] {
                if (isVisible() && input_->hasFocus()) {
                    fields_completer_->complete();
                }
            });
        }
    }

    void insertFieldsSuggestion(const QString& name) {
        input_->setText(ScriptSession::completeFieldList(input_->text(), name));
        input_->setFocus(Qt::OtherFocusReason);
    }

    // The action kind is stored as item data; header rows have none and are
    // not selectable.
    [[nodiscard]] int currentStepKind() const {
        const auto kind_data = kind_->currentData();
        return kind_data.isValid() ? kind_data.toInt() : -1;
    }

    void updateInputForKind() {
        const auto form =
            ScriptSession::stepForm(currentStepKind(), capture_source_->currentIndex());
        target_label_->setVisible(form.target);
        target_->setVisible(form.target);
        input_label_->setVisible(form.input);
        input_->setVisible(form.input);
        input_label_->setText(form.input_label);
        input_->setPlaceholderText(form.input_placeholder);
        replacement_label_->setVisible(form.replacement);
        replacement_->setVisible(form.replacement);
        number_start_label_->setVisible(form.numbering);
        number_start_->setVisible(form.numbering);
        number_padding_label_->setVisible(form.numbering);
        number_padding_->setVisible(form.numbering);
        character_count_label_->setVisible(form.characters);
        character_count_->setVisible(form.characters);
        capture_source_label_->setVisible(form.capture_source);
        capture_source_->setVisible(form.capture_source);
        rating_scale_label_->setVisible(form.rating_scale);
        rating_scale_->setVisible(form.rating_scale);
        capture_argument_label_->setVisible(form.capture_argument);
        capture_argument_->setVisible(form.capture_argument);
        capture_argument_label_->setText(form.capture_argument_label);
        capture_argument_->setPlaceholderText(form.capture_argument_placeholder);
    }

    void addStep() {
        const auto before = session_->steps().size();
        const auto where = session_->addStep(ScriptSession::StepInput{
            .kind = currentStepKind(),
            .target = target_->text(),
            .input = input_->text(),
            .replacement = replacement_->text(),
            .number_start = number_start_->value(),
            .number_padding = number_padding_->value(),
            .character_count = character_count_->value(),
            .capture_source = capture_source_->currentIndex(),
            .capture_argument = capture_argument_->text(),
            .rating_scale = rating_scale_->currentIndex(),
        });
        if (session_->steps().size() != before) {
            target_->clear();
            input_->clear();
            replacement_->clear();
        }
        focus(where);
    }

    [[nodiscard]] bool confirmDiscardBeforeImport() {
        if (!session_->unsaved()) {
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
        if (!session_->canImport() || !confirmDiscardBeforeImport()) {
            return;
        }
        session_->importNative(QFileDialog::getOpenFileName(
            this, QStringLiteral("Import Trackknife tagging script"), {},
            QStringLiteral("Trackknife tagging scripts (*.tbtags.json *.json);;All files (*)")));
    }

    void exportNative() {
        if (!session_->canExport()) {
            return;
        }
        session_->exportNative(QFileDialog::getSaveFileName(
            this, QStringLiteral("Export Trackknife tagging script"),
            session_->suggestedExportName(),
            QStringLiteral("Trackknife tagging scripts (*.tbtags.json);;JSON files (*.json);;All "
                           "files (*)")));
    }

    void importRules() {
        MetadataRuleScriptImportDialog dialog{this};
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        session_->importRuleScript(dialog.sourceText(),
                                   dialog.importMode() ==
                                       MetadataRuleScriptImportDialog::ImportMode::append);
    }

    void sync() {
        const auto& session = *session_;
        const auto row = steps_->currentRow();
        const auto editing = session.editing();
        setWindowModified(session.unsaved());
        if (name_->text() != session.name()) {
            const QSignalBlocker blocker{name_};
            name_->setText(session.name());
        }
        summary_->setText(session.summary());
        catalog_status_->setText(session.catalogStatus());
        raw_diagnostics_->setPlainText(session.rawDiagnostics());
        save_->setText(session.saveText());
        saved_->setEnabled(session.canSelectSaved());
        save_->setEnabled(session.canSave());
        save_as_->setEnabled(session.canSaveAsNew());
        delete_saved_->setEnabled(session.canDelete());
        import_native_->setEnabled(session.canImport());
        export_native_->setEnabled(session.canExport());
        name_->setEnabled(editing);
        kind_->setEnabled(editing);
        target_->setEnabled(editing);
        input_->setEnabled(editing);
        replacement_->setEnabled(editing);
        number_start_->setEnabled(editing);
        number_padding_->setEnabled(editing);
        character_count_->setEnabled(editing);
        capture_source_->setEnabled(editing);
        rating_scale_->setEnabled(editing);
        capture_argument_->setEnabled(editing);
        raw_source_->setEnabled(editing);
        import_->setEnabled(editing);
        add_->setEnabled(session.canAdd());
        steps_->setEnabled(editing);
        remove_->setEnabled(session.canRemove(row));
        up_->setEnabled(session.canMoveUp(row));
        down_->setEnabled(session.canMoveDown(row));
        stage_button_->setEnabled(session.canStage());
    }

    ScriptSession* session_;
    MetadataDialogLayoutStore layout_store_;
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
    QStringListModel* target_completion_model_{nullptr};
    QCompleter* target_completer_{nullptr};
    QStringListModel* fields_completion_model_{nullptr};
    QCompleter* fields_completer_{nullptr};
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
    QLabel* rating_scale_label_{nullptr};
    QComboBox* rating_scale_{nullptr};
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
    QLabel* summary_{nullptr};
    QTreeView* table_{nullptr};
    QDialogButtonBox* buttons_{nullptr};
    QPushButton* stage_button_{nullptr};
    QSplitter* content_splitter_{nullptr};
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
        new ScriptSession(std::move(selection), std::move(draft), std::move(item_indexes),
                          std::move(track_labels), std::move(stage), std::move(store),
                          initially_selected, preview_initially_selected),
        parent, std::move(layout_store));
}

} // namespace trackknife::bench
