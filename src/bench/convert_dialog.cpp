// SPDX-License-Identifier: GPL-3.0-only

#include "convert_dialog.hpp"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <utility>

namespace trackknife::bench {
namespace {

void fill(QComboBox* combo, const std::vector<ConvertJob::Choice>& choices) {
    for (const auto& choice : choices) {
        combo->addItem(choice.label, choice.value);
    }
}

void choose(QComboBox* combo, const QVariant& value) {
    const QSignalBlocker blocker{combo};
    if (const auto position = combo->findData(value); position >= 0) {
        combo->setCurrentIndex(position);
    }
}

void setText(QLineEdit* field, const QString& text) {
    if (field->text() != text) {
        const QSignalBlocker blocker{field};
        field->setText(text);
    }
}

} // namespace

// Creates a new saved preset, prefilled from whichever preset was selected;
// built-ins stay immutable, so "editing" always saves a new profile.
class EncoderPresetEditor final : public QDialog {
  public:
    explicit EncoderPresetEditor(const ConvertJob::PresetDraft& base, QWidget* parent)
        : QDialog(parent) {
        setWindowTitle(QStringLiteral("New encoder preset"));
        setObjectName(QStringLiteral("bench-preset-editor"));
        auto* form = new QFormLayout(this);

        name_ = new QLineEdit(this);
        name_->setObjectName(QStringLiteral("bench-preset-editor-name"));
        name_->setPlaceholderText(QStringLiteral("Preset name"));
        form->addRow(QStringLiteral("Name:"), name_);

        format_ = new QComboBox(this);
        format_->setObjectName(QStringLiteral("bench-preset-editor-format"));
        format_->addItems(ConvertJob::editorFormats());
        form->addRow(QStringLiteral("Format:"), format_);

        rate_mode_ = new QComboBox(this);
        rate_mode_->setObjectName(QStringLiteral("bench-preset-editor-rate-mode"));
        rate_mode_->addItem(QStringLiteral("Bit rate"));
        rate_mode_->addItem(QStringLiteral("VBR quality"));
        form->addRow(QStringLiteral("Rate control:"), rate_mode_);

        bitrate_ = new QSpinBox(this);
        bitrate_->setObjectName(QStringLiteral("bench-preset-editor-bitrate"));
        bitrate_->setRange(8, 2'000);
        bitrate_->setSuffix(QStringLiteral(" kbps"));
        form->addRow(QStringLiteral("Bit rate:"), bitrate_);

        quality_ = new QSpinBox(this);
        quality_->setObjectName(QStringLiteral("bench-preset-editor-quality"));
        quality_->setRange(-2, 12);
        form->addRow(QStringLiteral("Quality:"), quality_);

        auto* buttons =
            new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        buttons->setObjectName(QStringLiteral("bench-preset-editor-buttons"));
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        form->addRow(buttons);
        save_button_ = buttons->button(QDialogButtonBox::Save);

        const auto refresh_controls = [this] {
            const auto lossless = ConvertJob::editorFormatLossless(format_->currentIndex());
            rate_mode_->setEnabled(!lossless);
            const auto quality_mode = rate_mode_->currentIndex() == 1;
            bitrate_->setEnabled(!lossless && !quality_mode);
            quality_->setEnabled(!lossless && quality_mode);
            save_button_->setEnabled(!name_->text().trimmed().isEmpty());
        };
        connect(format_, &QComboBox::currentIndexChanged, this, refresh_controls);
        connect(rate_mode_, &QComboBox::currentIndexChanged, this, refresh_controls);
        connect(name_, &QLineEdit::textChanged, this, refresh_controls);

        format_->setCurrentIndex(base.format);
        rate_mode_->setCurrentIndex(base.quality_mode ? 1 : 0);
        bitrate_->setValue(base.bitrate_kbps);
        quality_->setValue(base.quality);
        refresh_controls();
    }

    [[nodiscard]] ConvertJob::PresetDraft result() const {
        return ConvertJob::PresetDraft{.name = name_->text(),
                                       .format = format_->currentIndex(),
                                       .quality_mode = rate_mode_->currentIndex() == 1,
                                       .bitrate_kbps = bitrate_->value(),
                                       .quality = quality_->value()};
    }

  private:
    QLineEdit* name_{nullptr};
    QComboBox* format_{nullptr};
    QComboBox* rate_mode_{nullptr};
    QSpinBox* bitrate_{nullptr};
    QSpinBox* quality_{nullptr};
    QPushButton* save_button_{nullptr};
};

ConvertDialog::ConvertDialog(std::vector<ConvertDialogItem> items, ConvertProfilesLoader profiles,
                             ConvertPresetStore preset_store, QWidget* parent)
    : QDialog(parent) {
    const auto preset_store_available = static_cast<bool>(preset_store.load);
    job_ = new ConvertJob(std::move(items), std::move(profiles), std::move(preset_store), this);
    setWindowTitle(job_->title());
    setObjectName(QStringLiteral("bench-convert-dialog"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(720, 520);

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    form_ = form;

    auto* preset_row = new QHBoxLayout;
    preset_ = new QComboBox(this);
    preset_->setObjectName(QStringLiteral("bench-convert-preset"));
    preset_row->addWidget(preset_, 1);
    preset_new_ = new QPushButton(QStringLiteral("New…"), this);
    preset_new_->setObjectName(QStringLiteral("bench-convert-preset-new"));
    preset_new_->setToolTip(
        QStringLiteral("Save a new encoder preset starting from the selected one"));
    connect(preset_new_, &QPushButton::clicked, this, &ConvertDialog::openPresetEditor);
    preset_row->addWidget(preset_new_);
    preset_new_->setVisible(preset_store_available);
    preset_export_ = new QPushButton(QStringLiteral("Export…"), this);
    preset_export_->setObjectName(QStringLiteral("bench-convert-preset-export"));
    preset_export_->setToolTip(QStringLiteral("Export the selected encoder preset as JSON"));
    connect(preset_export_, &QPushButton::clicked, this, &ConvertDialog::exportSelectedPreset);
    preset_row->addWidget(preset_export_);
    preset_delete_ = new QPushButton(QStringLiteral("Delete"), this);
    preset_delete_->setObjectName(QStringLiteral("bench-convert-preset-delete"));
    preset_delete_->setVisible(false);
    connect(preset_delete_, &QPushButton::clicked, job_, &ConvertJob::deletePreset);
    preset_row->addWidget(preset_delete_);
    form->addRow(QStringLiteral("Preset:"), preset_row);

    auto* destination_row = new QHBoxLayout;
    destination_choice_ = new QComboBox(this);
    destination_choice_->setObjectName(QStringLiteral("bench-convert-destination-choice"));
    destination_choice_->setVisible(false);
    destination_row->addWidget(destination_choice_);
    destination_ = new QLineEdit(this);
    destination_->setObjectName(QStringLiteral("bench-convert-destination"));
    destination_->setPlaceholderText(QStringLiteral("Destination folder"));
    auto* browse = new QPushButton(QStringLiteral("Browse…"), this);
    browse->setObjectName(QStringLiteral("bench-convert-browse"));
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto chosen = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose the conversion destination"), destination_->text());
        if (!chosen.isEmpty()) {
            job_->setDestination(chosen, true);
        }
    });
    destination_row->addWidget(destination_, 1);
    destination_row->addWidget(browse);
    form->addRow(QStringLiteral("Into:"), destination_row);

    layout_choice_ = new QComboBox(this);
    layout_choice_->setObjectName(QStringLiteral("bench-convert-layout"));
    form->addRow(QStringLiteral("Layout:"), layout_choice_);
    form->setRowVisible(layout_choice_, false);

    mirror_structure_ = new QCheckBox(QStringLiteral("Mirror source folders"), this);
    mirror_structure_->setObjectName(QStringLiteral("bench-convert-mirror"));
    mirror_structure_->setToolTip(
        QStringLiteral("Recreates each source's complete folder path beneath the destination, "
                       "keeping source file names instead of the expressions"));
    form->addRow(QString{}, mirror_structure_);

    directory_expression_ = new QLineEdit(this);
    directory_expression_->setObjectName(QStringLiteral("bench-convert-directory-expression"));
    form->addRow(QStringLiteral("Folders:"), directory_expression_);

    basename_expression_ = new QLineEdit(this);
    basename_expression_->setObjectName(QStringLiteral("bench-convert-basename-expression"));
    form->addRow(QStringLiteral("Names:"), basename_expression_);

    resample_ = new QComboBox(this);
    resample_->setObjectName(QStringLiteral("bench-convert-resample"));
    fill(resample_, ConvertJob::resampleChoices());
    resample_->setToolTip(QStringLiteral(
        "Encoders that only speak certain rates still constrain the result — Opus maps every "
        "choice into its 48 kHz family"));
    form->addRow(QStringLiteral("Resample:"), resample_);

    bit_depth_ = new QComboBox(this);
    bit_depth_->setObjectName(QStringLiteral("bench-convert-bit-depth"));
    fill(bit_depth_, ConvertJob::bitDepthChoices());
    bit_depth_->setToolTip(
        QStringLiteral("Stored bit depth for lossless output; Opus and other float-based "
                       "encoders have no stored depth and ignore this"));
    form->addRow(QStringLiteral("Bit depth:"), bit_depth_);

    channels_ = new QComboBox(this);
    channels_->setObjectName(QStringLiteral("bench-convert-channels"));
    fill(channels_, ConvertJob::channelChoices());
    form->addRow(QStringLiteral("Channels:"), channels_);

    gain_ = new QComboBox(this);
    gain_->setObjectName(QStringLiteral("bench-convert-gain"));
    fill(gain_, ConvertJob::gainChoices());
    gain_->setToolTip(QStringLiteral(
        "Permanently changes PCM before encoding; stale ReplayGain tags are removed"));
    form->addRow(QStringLiteral("Permanent volume adjustment:"), gain_);
    gain_warning_ = new QLabel(this);
    gain_warning_->setObjectName(QStringLiteral("bench-convert-gain-warning"));
    gain_warning_->setWordWrap(true);
    gain_warning_->setTextFormat(Qt::RichText);
    form->addRow(QString{}, gain_warning_);

    embed_artwork_ = new QCheckBox(QStringLiteral("Embed cover art"), this);
    embed_artwork_->setObjectName(QStringLiteral("bench-convert-artwork"));
    embed_artwork_->setToolTip(
        QStringLiteral("Carries each source's cover image (embedded pictures first, then "
                       "cover/folder/front siblings) into the converted file"));
    form->addRow(QString{}, embed_artwork_);

    parallelism_ = new QSpinBox(this);
    parallelism_->setObjectName(QStringLiteral("bench-convert-parallelism"));
    parallelism_->setRange(1, ConvertJob::maximumParallelism());
    form->addRow(QStringLiteral("Parallel files:"), parallelism_);
    layout->addLayout(form);

    preview_ = new QListWidget(this);
    preview_->setObjectName(QStringLiteral("bench-convert-preview"));
    preview_->setSelectionMode(QAbstractItemView::NoSelection);
    preview_->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(preview_, 1);

    progress_ = new QProgressBar(this);
    progress_->setObjectName(QStringLiteral("bench-convert-progress"));
    progress_->setVisible(false);
    layout->addWidget(progress_);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("bench-convert-status"));
    status_->setWordWrap(true);
    layout->addWidget(status_);
    // Per-file problem reports scroll inside a bounded pane.
    problems_ = new QPlainTextEdit(this);
    problems_->setObjectName(QStringLiteral("bench-convert-problems"));
    problems_->setReadOnly(true);
    problems_->setMaximumHeight(140);
    problems_->hide();
    layout->addWidget(problems_);

    auto* buttons = new QHBoxLayout;
    run_ = new QPushButton(QStringLiteral("Convert"), this);
    run_->setObjectName(QStringLiteral("bench-convert-run"));
    run_->setDefault(true);
    connect(run_, &QPushButton::clicked, this, &ConvertDialog::startConversion);
    stop_ = new QPushButton(QStringLiteral("Stop"), this);
    stop_->setObjectName(QStringLiteral("bench-convert-stop"));
    stop_->setVisible(false);
    connect(stop_, &QPushButton::clicked, job_, &ConvertJob::stop);
    close_ = new QPushButton(QStringLiteral("Close"), this);
    close_->setObjectName(QStringLiteral("bench-convert-close"));
    connect(close_, &QPushButton::clicked, this, &QDialog::close);
    buttons->addStretch(1);
    buttons->addWidget(run_);
    buttons->addWidget(stop_);
    buttons->addWidget(close_);
    layout->addLayout(buttons);

    connect(preset_, &QComboBox::currentIndexChanged, this,
            [this] { job_->selectPreset(preset_->currentData().toString()); });
    connect(destination_choice_, &QComboBox::activated, job_, &ConvertJob::selectDestination);
    connect(layout_choice_, &QComboBox::activated, job_, &ConvertJob::selectLayout);
    connect(destination_, &QLineEdit::textChanged, this,
            [this](const QString& text) { job_->setDestination(text, false); });
    connect(destination_, &QLineEdit::textEdited, this,
            [this](const QString& text) { job_->setDestination(text, true); });
    connect(directory_expression_, &QLineEdit::textChanged, this,
            [this](const QString& text) { job_->setDirectoryExpression(text, false); });
    connect(directory_expression_, &QLineEdit::textEdited, this,
            [this](const QString& text) { job_->setDirectoryExpression(text, true); });
    connect(basename_expression_, &QLineEdit::textChanged, this,
            [this](const QString& text) { job_->setBasenameExpression(text, false); });
    connect(basename_expression_, &QLineEdit::textEdited, this,
            [this](const QString& text) { job_->setBasenameExpression(text, true); });
    connect(mirror_structure_, &QCheckBox::toggled, job_, &ConvertJob::setMirror);
    connect(resample_, &QComboBox::currentIndexChanged, this,
            [this] { job_->setResample(resample_->currentData()); });
    connect(bit_depth_, &QComboBox::currentIndexChanged, this,
            [this] { job_->setBitDepth(bit_depth_->currentData()); });
    connect(channels_, &QComboBox::currentIndexChanged, this,
            [this] { job_->setChannels(channels_->currentData()); });
    connect(gain_, &QComboBox::currentIndexChanged, this,
            [this] { job_->setGain(gain_->currentData().toInt()); });
    connect(embed_artwork_, &QCheckBox::toggled, job_, &ConvertJob::setEmbedArtwork);
    connect(parallelism_, &QSpinBox::valueChanged, job_, &ConvertJob::setParallelism);

    connect(job_, &ConvertJob::changed, this, &ConvertDialog::sync);
    connect(job_, &ConvertJob::presetsChanged, this, &ConvertDialog::rebuildPresets);
    connect(job_, &ConvertJob::profilesChanged, this, &ConvertDialog::rebuildProfiles);
    connect(job_, &ConvertJob::filesConverted, this, &ConvertDialog::filesConverted);
    rebuildPresets();
    rebuildProfiles();
    sync();
}

// The built-ins first, then the user's saved presets; unavailable encoders
// stay visible but disabled with the probe detail as tooltip.
void ConvertDialog::rebuildPresets() {
    const QSignalBlocker blocker{preset_};
    preset_->clear();
    for (const auto& preset : job_->presets()) {
        if (preset.separator) {
            preset_->insertSeparator(preset_->count());
            continue;
        }
        preset_->addItem(preset.name, preset.id);
        if (!preset.available) {
            auto* model = qobject_cast<QStandardItemModel*>(preset_->model());
            if (auto* item = model != nullptr ? model->item(preset_->count() - 1) : nullptr) {
                item->setEnabled(false);
                item->setToolTip(preset.detail);
            }
        }
    }
    preset_->setCurrentIndex(job_->presetIndex());
}

void ConvertDialog::rebuildProfiles() {
    {
        const QSignalBlocker layouts{layout_choice_};
        const QSignalBlocker destinations{destination_choice_};
        layout_choice_->clear();
        layout_choice_->addItems(job_->layouts());
        destination_choice_->clear();
        destination_choice_->addItems(job_->destinations());
    }
    form_->setRowVisible(layout_choice_, job_->layouts().size() > 1);
    destination_choice_->setVisible(job_->destinations().size() > 1);
    sync();
}

void ConvertDialog::sync() {
    const auto& job = *job_;
    if (preset_->currentIndex() != job.presetIndex()) {
        const QSignalBlocker blocker{preset_};
        preset_->setCurrentIndex(job.presetIndex());
    }
    preset_delete_->setVisible(job.canDeletePreset());
    {
        const QSignalBlocker layouts{layout_choice_};
        const QSignalBlocker destinations{destination_choice_};
        layout_choice_->setCurrentIndex(job.layoutChoice());
        destination_choice_->setCurrentIndex(job.destinationChoice());
    }
    setText(destination_, job.destination());
    setText(directory_expression_, job.directoryExpression());
    setText(basename_expression_, job.basenameExpression());
    {
        const QSignalBlocker mirror{mirror_structure_};
        const QSignalBlocker artwork{embed_artwork_};
        const QSignalBlocker parallel{parallelism_};
        mirror_structure_->setChecked(job.mirror());
        embed_artwork_->setChecked(job.embedArtwork());
        parallelism_->setValue(job.parallelism());
    }
    const auto mirrored = job.mirror();
    directory_expression_->setEnabled(!mirrored);
    basename_expression_->setEnabled(!mirrored);
    layout_choice_->setEnabled(!mirrored);
    choose(resample_, job.resample());
    choose(bit_depth_, job.bitDepth());
    choose(channels_, job.channels());
    choose(gain_, job.gain());
    gain_warning_->setText(job.gainWarning());
    if (preview_->count() != job.preview().size() ||
        (preview_->count() > 0 && preview_->item(0)->text() != job.preview().front())) {
        preview_->clear();
        preview_->addItems(job.preview());
    }
    status_->setText(job.status());
    problems_->setVisible(!job.problems().isEmpty());
    problems_->setPlainText(job.problems().join(QStringLiteral("\n")));
    const auto running = job.running();
    run_->setVisible(!running);
    run_->setEnabled(job.canRun());
    stop_->setVisible(running);
    progress_->setVisible(running);
    progress_->setRange(0, job.progressMaximum());
    progress_->setValue(job.progressValue());
}

void ConvertDialog::startConversion() {
    if (!job_->canRun()) {
        return;
    }
    if (job_->gain() != 0) {
        QMessageBox confirmation{
            QMessageBox::Warning, QStringLiteral("Permanently change audio volume?"),
            job_->gainConfirmation(), QMessageBox::Yes | QMessageBox::Cancel, this};
        confirmation.setObjectName(QStringLiteral("bench-convert-gain-confirmation"));
        confirmation.button(QMessageBox::Yes)->setText(QStringLiteral("Convert and change volume"));
        confirmation.setDefaultButton(QMessageBox::Cancel);
        confirmation.setEscapeButton(QMessageBox::Cancel);
        if (confirmation.exec() != QMessageBox::Yes) {
            return;
        }
    }
    job_->run();
}

void ConvertDialog::openPresetEditor() {
    auto* editor = new EncoderPresetEditor(job_->presetDraft(), this);
    editor->setAttribute(Qt::WA_DeleteOnClose);
    connect(editor, &QDialog::accepted, this,
            [this, editor] { job_->savePreset(editor->result()); });
    editor->open();
}

void ConvertDialog::exportSelectedPreset() {
    const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("Export encoder preset"),
                                                   job_->suggestedPresetExportName(),
                                                   QStringLiteral("JSON (*.json)"));
    if (!path.isEmpty()) {
        job_->exportPreset(path);
    }
}

void ConvertDialog::closeEvent(QCloseEvent* event) {
    if (!job_->requestClose()) {
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

} // namespace trackknife::bench
