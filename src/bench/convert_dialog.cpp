// SPDX-License-Identifier: GPL-3.0-only

#include "convert_dialog.hpp"

#include "bench_main_window_helpers.hpp"
#include "trackknife/convert/preset.hpp"

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
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <filesystem>
#include <system_error>
#include <utility>

namespace trackknife::bench {
namespace {

constexpr int preview_limit = 200;

[[nodiscard]] QString settingsText(const QSettings& settings, const char* key,
                                   const QString& fallback) {
    const auto value = settings.value(QLatin1String(key)).toString();
    return value.isEmpty() ? fallback : value;
}

// One fixed encode format per row keeps user presets inside the qualified
// encoder set; the rate controls adapt to the chosen format.
struct EditorFormat {
    const char* label;
    const char* codec;
    const char* container;
    const char* extension;
    bool lossless;
    const char* sample_format_hint;
};

constexpr std::array<EditorFormat, 4> editor_formats{{
    {"FLAC (lossless)", "flac", "flac", "flac", true, "s32"},
    {"Opus", "libopus", "opus", "opus", false, ""},
    {"MP3", "libmp3lame", "mp3", "mp3", false, ""},
    {"Ogg Vorbis", "libvorbis", "ogg", "ogg", false, ""},
}};

} // namespace

// Creates a new saved preset, prefilled from whichever preset was selected;
// built-ins stay immutable, so "editing" always saves a new profile.
class EncoderPresetEditor final : public QDialog {
  public:
    explicit EncoderPresetEditor(const convert::EncoderPreset& base, QWidget* parent)
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
        for (const auto& entry : editor_formats) {
            format_->addItem(QLatin1String(entry.label));
        }
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
        bitrate_->setValue(192);
        form->addRow(QStringLiteral("Bit rate:"), bitrate_);

        quality_ = new QSpinBox(this);
        quality_->setObjectName(QStringLiteral("bench-preset-editor-quality"));
        quality_->setRange(-2, 12);
        quality_->setValue(4);
        form->addRow(QStringLiteral("Quality:"), quality_);

        auto* buttons =
            new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        buttons->setObjectName(QStringLiteral("bench-preset-editor-buttons"));
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        form->addRow(buttons);
        save_button_ = buttons->button(QDialogButtonBox::Save);

        const auto refresh_controls = [this] {
            const auto& entry = editor_formats[static_cast<std::size_t>(format_->currentIndex())];
            rate_mode_->setEnabled(!entry.lossless);
            const auto quality_mode = rate_mode_->currentIndex() == 1;
            bitrate_->setEnabled(!entry.lossless && !quality_mode);
            quality_->setEnabled(!entry.lossless && quality_mode);
            save_button_->setEnabled(!name_->text().trimmed().isEmpty());
        };
        connect(format_, &QComboBox::currentIndexChanged, this, refresh_controls);
        connect(rate_mode_, &QComboBox::currentIndexChanged, this, refresh_controls);
        connect(name_, &QLineEdit::textChanged, this, refresh_controls);

        // Prefill from the selected preset.
        for (std::size_t index = 0U; index < editor_formats.size(); ++index) {
            if (editor_formats[index].codec == base.codec_name) {
                format_->setCurrentIndex(static_cast<int>(index));
                break;
            }
        }
        if (base.vbr_quality) {
            rate_mode_->setCurrentIndex(1);
            quality_->setValue(*base.vbr_quality);
        } else if (base.bit_rate) {
            rate_mode_->setCurrentIndex(0);
            bitrate_->setValue(static_cast<int>(*base.bit_rate / 1'000));
        }
        refresh_controls();
    }

    [[nodiscard]] persistence::SavedEncoderPreset result() const {
        const auto id = core::StableId::random();
        const auto& entry = editor_formats[static_cast<std::size_t>(format_->currentIndex())];
        persistence::SavedEncoderPreset saved;
        saved.id = id;
        saved.preset = convert::EncoderPreset{
            .id = id.to_string(),
            .version = 1,
            .display_name = name_->text().trimmed().toStdString(),
            .codec_name = entry.codec,
            .container_name = entry.container,
            .file_extension = entry.extension,
            .lossless = entry.lossless,
            .bit_rate = std::nullopt,
            .vbr_quality = std::nullopt,
            .sample_format_hint = entry.sample_format_hint,
        };
        if (!entry.lossless) {
            if (rate_mode_->currentIndex() == 1) {
                saved.preset.vbr_quality = quality_->value();
            } else {
                saved.preset.bit_rate = static_cast<std::int64_t>(bitrate_->value()) * 1'000;
            }
        }
        return saved;
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
    : QDialog(parent), items_(std::move(items)), preset_store_(std::move(preset_store)) {
    setWindowTitle(QStringLiteral("Convert %1 file%2")
                       .arg(items_.size())
                       .arg(items_.size() == 1U ? QString{} : QStringLiteral("s")));
    setObjectName(QStringLiteral("bench-convert-dialog"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(720, 520);

    const QSettings settings;
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
    preset_delete_ = new QPushButton(QStringLiteral("Delete"), this);
    preset_delete_->setObjectName(QStringLiteral("bench-convert-preset-delete"));
    preset_delete_->setVisible(false);
    connect(preset_delete_, &QPushButton::clicked, this, &ConvertDialog::deleteSelectedPreset);
    preset_row->addWidget(preset_delete_);
    form->addRow(QStringLiteral("Preset:"), preset_row);
    rebuildPresetCombo(settings.value(QStringLiteral("convert/preset")).toString());
    if (!preset_store_.load) {
        preset_new_->setVisible(false);
    }

    auto* destination_row = new QHBoxLayout;
    destination_choice_ = new QComboBox(this);
    destination_choice_->setObjectName(QStringLiteral("bench-convert-destination-choice"));
    destination_choice_->addItem(QStringLiteral("Custom"));
    destination_choice_->setVisible(false);
    connect(destination_choice_, &QComboBox::activated, this,
            &ConvertDialog::applySavedDestination);
    destination_row->addWidget(destination_choice_);
    destination_ = new QLineEdit(this);
    destination_->setObjectName(QStringLiteral("bench-convert-destination"));
    destination_->setText(settings.value(QStringLiteral("convert/destination-root")).toString());
    destination_->setPlaceholderText(QStringLiteral("Destination folder"));
    auto* browse = new QPushButton(QStringLiteral("Browse…"), this);
    browse->setObjectName(QStringLiteral("bench-convert-browse"));
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto chosen = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose the conversion destination"), destination_->text());
        if (!chosen.isEmpty()) {
            destination_->setText(chosen);
        }
    });
    destination_row->addWidget(destination_, 1);
    destination_row->addWidget(browse);
    form->addRow(QStringLiteral("Into:"), destination_row);

    layout_choice_ = new QComboBox(this);
    layout_choice_->setObjectName(QStringLiteral("bench-convert-layout"));
    layout_choice_->addItem(QStringLiteral("Custom"));
    connect(layout_choice_, &QComboBox::activated, this, &ConvertDialog::applySavedLayout);
    form->addRow(QStringLiteral("Layout:"), layout_choice_);
    form->setRowVisible(layout_choice_, false);

    mirror_structure_ = new QCheckBox(QStringLiteral("Mirror source folders"), this);
    mirror_structure_->setObjectName(QStringLiteral("bench-convert-mirror"));
    mirror_structure_->setChecked(
        settings.value(QStringLiteral("convert/mirror-structure"), false).toBool());
    mirror_structure_->setToolTip(
        QStringLiteral("Reproduces the folder structure below the sources' deepest common "
                       "directory beneath the destination, keeping source file names instead of "
                       "the expressions"));
    form->addRow(QString{}, mirror_structure_);

    // ADR-0154: an explicit, persisted mirror root keeps the mirrored
    // layout independent of how broad the selection is; empty falls back
    // to the inferred deepest common directory.
    auto* mirror_root_row = new QHBoxLayout;
    mirror_root_ = new QLineEdit(this);
    mirror_root_->setObjectName(QStringLiteral("bench-convert-mirror-root"));
    mirror_root_->setPlaceholderText(QStringLiteral("auto: deepest common folder"));
    mirror_root_->setText(settings.value(QStringLiteral("convert/mirror-root")).toString());
    mirror_root_->setToolTip(
        QStringLiteral("Mirror each source's path below this folder; sources outside it block "
                       "the plan. Leave empty to use the selection's deepest common folder."));
    mirror_root_row->addWidget(mirror_root_, 1);
    auto* mirror_browse = new QPushButton(QStringLiteral("Browse…"), this);
    mirror_browse->setObjectName(QStringLiteral("bench-convert-mirror-root-browse"));
    connect(mirror_browse, &QPushButton::clicked, this, [this] {
        const auto chosen = QFileDialog::getExistingDirectory(this, QStringLiteral("Mirror below"),
                                                              mirror_root_->text());
        if (!chosen.isEmpty()) {
            mirror_root_->setText(chosen);
        }
    });
    mirror_root_row->addWidget(mirror_browse);
    form->addRow(QStringLiteral("Mirror below:"), mirror_root_row);

    directory_expression_ = new QLineEdit(this);
    directory_expression_->setObjectName(QStringLiteral("bench-convert-directory-expression"));
    directory_expression_->setText(settingsText(settings, "convert/directory-expression",
                                                QStringLiteral("%albumartist%/%album%")));
    form->addRow(QStringLiteral("Folders:"), directory_expression_);

    basename_expression_ = new QLineEdit(this);
    basename_expression_->setObjectName(QStringLiteral("bench-convert-basename-expression"));
    basename_expression_->setText(settingsText(settings, "convert/basename-expression",
                                               QStringLiteral("%tracknumber% - %title%")));
    form->addRow(QStringLiteral("Names:"), basename_expression_);

    resample_ = new QComboBox(this);
    resample_->setObjectName(QStringLiteral("bench-convert-resample"));
    resample_->addItem(QStringLiteral("Keep source rate"), 0);
    for (const auto rate : {44'100, 48'000, 88'200, 96'000, 176'400, 192'000}) {
        resample_->addItem(QStringLiteral("%1 kHz").arg(rate / 1000.0), rate);
    }
    // Downsample-only caps (ADR-0134) are stored as negated rates.
    for (const auto cap : {44'100, 48'000}) {
        resample_->addItem(QStringLiteral("Downsample to %1 kHz if higher").arg(cap / 1000.0),
                           -cap);
    }
    const auto saved_rate = settings.value(QStringLiteral("convert/resample-rate"), 0).toInt();
    if (const auto position = resample_->findData(saved_rate); position >= 0) {
        resample_->setCurrentIndex(position);
    }
    resample_->setToolTip(QStringLiteral(
        "Encoders that only speak certain rates still constrain the result — Opus maps every "
        "choice into its 48 kHz family"));
    form->addRow(QStringLiteral("Resample:"), resample_);

    bit_depth_ = new QComboBox(this);
    bit_depth_->setObjectName(QStringLiteral("bench-convert-bit-depth"));
    bit_depth_->addItem(QStringLiteral("Preset default"), 0);
    bit_depth_->addItem(QStringLiteral("16-bit (dithered)"), 16);
    bit_depth_->addItem(QStringLiteral("24-bit"), 24);
    bit_depth_->addItem(QStringLiteral("Keep source depth"), -1);
    const auto saved_depth = settings.value(QStringLiteral("convert/bit-depth"), 0).toInt();
    if (const auto depth_position = bit_depth_->findData(saved_depth); depth_position >= 0) {
        bit_depth_->setCurrentIndex(depth_position);
    }
    bit_depth_->setToolTip(
        QStringLiteral("Stored bit depth for lossless output; Opus and other float-based "
                       "encoders have no stored depth and ignore this"));
    form->addRow(QStringLiteral("Bit depth:"), bit_depth_);

    embed_artwork_ = new QCheckBox(QStringLiteral("Embed cover art"), this);
    embed_artwork_->setObjectName(QStringLiteral("bench-convert-artwork"));
    embed_artwork_->setChecked(
        settings.value(QStringLiteral("convert/embed-artwork"), true).toBool());
    embed_artwork_->setToolTip(
        QStringLiteral("Carries each source's cover image (embedded pictures first, then "
                       "cover/folder/front siblings) into the converted file"));
    form->addRow(QString{}, embed_artwork_);

    parallelism_ = new QSpinBox(this);
    parallelism_->setObjectName(QStringLiteral("bench-convert-parallelism"));
    parallelism_->setRange(1, static_cast<int>(convert::maximum_conversion_parallelism));
    parallelism_->setValue(settings.value(QStringLiteral("convert/parallelism"), 4).toInt());
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
    // Per-file problem reports scroll inside a bounded pane; unbounded
    // multi-line status text used to stretch the dialog past the screen.
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
    connect(stop_, &QPushButton::clicked, this, [this] { cancellation_.request_cancellation(); });
    close_ = new QPushButton(QStringLiteral("Close"), this);
    close_->setObjectName(QStringLiteral("bench-convert-close"));
    connect(close_, &QPushButton::clicked, this, &QDialog::close);
    buttons->addStretch(1);
    buttons->addWidget(run_);
    buttons->addWidget(stop_);
    buttons->addWidget(close_);
    layout->addLayout(buttons);

    connect(&watcher_, &QFutureWatcherBase::finished, this, &ConvertDialog::finishConversion);
    // The preview recomputes on every edit; planning is pure and fast, so a
    // short debounce only coalesces bursts of keystrokes.
    auto* debounce = new QTimer(this);
    debounce->setSingleShot(true);
    debounce->setInterval(150);
    connect(debounce, &QTimer::timeout, this, &ConvertDialog::refreshPreview);
    const auto schedule = [debounce] { debounce->start(); };
    connect(destination_, &QLineEdit::textChanged, this, schedule);
    connect(directory_expression_, &QLineEdit::textChanged, this, schedule);
    connect(basename_expression_, &QLineEdit::textChanged, this, schedule);
    const auto apply_mirror_mode = [this, schedule] {
        const auto mirrored = mirror_structure_->isChecked();
        directory_expression_->setEnabled(!mirrored);
        basename_expression_->setEnabled(!mirrored);
        layout_choice_->setEnabled(!mirrored);
        mirror_root_->setEnabled(mirrored);
        schedule();
    };
    connect(mirror_structure_, &QCheckBox::toggled, this, apply_mirror_mode);
    connect(mirror_root_, &QLineEdit::textChanged, this, [schedule] { schedule(); });
    apply_mirror_mode();
    connect(preset_, &QComboBox::currentIndexChanged, this, schedule);
    connect(preset_, &QComboBox::currentIndexChanged, this, [this] {
        const auto chosen = preset_->currentData().toString().toStdString();
        const auto saved = std::ranges::any_of(
            saved_presets_, [&chosen](const auto& entry) { return entry.preset.id == chosen; });
        preset_delete_->setVisible(saved && preset_store_.remove != nullptr);
    });
    // Hand-editing an expression or the root leaves the saved choice.
    connect(directory_expression_, &QLineEdit::textEdited, this,
            [this] { layout_choice_->setCurrentIndex(0); });
    connect(basename_expression_, &QLineEdit::textEdited, this,
            [this] { layout_choice_->setCurrentIndex(0); });
    connect(destination_, &QLineEdit::textEdited, this,
            [this] { destination_choice_->setCurrentIndex(0); });

    if (profiles) {
        const QPointer self{this};
        profiles([self](std::vector<persistence::SavedOutputLayoutProfile> layouts,
                        std::vector<persistence::SavedDestinationProfile> destinations,
                        const QString& error) {
            if (self == nullptr || !error.isEmpty()) {
                return;
            }
            self->layout_catalog_ = std::move(layouts);
            self->destination_catalog_ = std::move(destinations);
            for (const auto& saved : self->layout_catalog_) {
                self->layout_choice_->addItem(displayText(saved.profile.name));
            }
            self->form_->setRowVisible(self->layout_choice_, !self->layout_catalog_.empty());
            for (const auto& destination : self->destination_catalog_) {
                self->destination_choice_->addItem(displayText(destination.profile.name));
            }
            self->destination_choice_->setVisible(!self->destination_catalog_.empty());
        });
    }

    reloadPresets(preset_->currentData().toString());

    refreshPreview();
}

// The combo lists the immutable built-ins first, then the user's saved
// presets; unavailable encoders stay visible but disabled with the probe
// detail as tooltip.
void ConvertDialog::rebuildPresetCombo(const QString& select_data) {
    const QSignalBlocker blocker{preset_};
    preset_->clear();
    const auto add_preset = [this](const convert::EncoderPreset& preset) {
        preset_->addItem(displayText(preset.display_name), displayText(preset.id));
        const auto availability = convert::probe_encoder_preset(preset);
        if (!availability.available) {
            const auto row = preset_->count() - 1;
            auto* model = qobject_cast<QStandardItemModel*>(preset_->model());
            if (model != nullptr && model->item(row) != nullptr) {
                model->item(row)->setEnabled(false);
                model->item(row)->setToolTip(displayText(availability.detail));
            }
        }
    };
    for (const auto& preset : convert::builtin_encoder_presets()) {
        add_preset(preset);
    }
    if (!saved_presets_.empty()) {
        preset_->insertSeparator(preset_->count());
        for (const auto& saved : saved_presets_) {
            add_preset(saved.preset);
        }
    }
    const auto position = preset_->findData(select_data);
    preset_->setCurrentIndex(position >= 0 ? position : 0);
    const auto chosen = preset_->currentData().toString().toStdString();
    const auto saved_selected = std::ranges::any_of(
        saved_presets_, [&chosen](const auto& entry) { return entry.preset.id == chosen; });
    preset_delete_->setVisible(saved_selected && preset_store_.remove != nullptr);
}

void ConvertDialog::reloadPresets(const QString& select_data) {
    if (!preset_store_.load) {
        return;
    }
    const QPointer self{this};
    preset_store_.load([self, select_data](std::vector<persistence::SavedEncoderPreset> presets,
                                           const QString& error) {
        if (self == nullptr) {
            return;
        }
        if (!error.isEmpty()) {
            self->status_->setText(error);
            return;
        }
        self->saved_presets_ = std::move(presets);
        self->rebuildPresetCombo(select_data);
        self->refreshPreview();
    });
}

void ConvertDialog::openPresetEditor() {
    if (!preset_store_.save) {
        return;
    }
    const auto base = selectedPreset();
    auto* editor = new EncoderPresetEditor(base ? *base : convert::EncoderPreset{}, this);
    editor->setAttribute(Qt::WA_DeleteOnClose);
    connect(editor, &QDialog::accepted, this, [this, editor] {
        if (!preset_store_.save) {
            return;
        }
        auto saved = editor->result();
        const auto select = displayText(saved.preset.id);
        const QPointer self{this};
        preset_store_.save(std::move(saved), [self, select](const QString& error) {
            if (self == nullptr) {
                return;
            }
            if (!error.isEmpty()) {
                self->status_->setText(error);
                return;
            }
            self->reloadPresets(select);
        });
    });
    editor->open();
}

void ConvertDialog::deleteSelectedPreset() {
    const auto chosen = preset_->currentData().toString().toStdString();
    const auto found = std::ranges::find_if(
        saved_presets_, [&chosen](const auto& entry) { return entry.preset.id == chosen; });
    if (found == saved_presets_.end() || !preset_store_.remove) {
        return;
    }
    const QPointer self{this};
    preset_store_.remove(found->id, [self](const QString& error) {
        if (self == nullptr) {
            return;
        }
        if (!error.isEmpty()) {
            self->status_->setText(error);
            return;
        }
        self->reloadPresets({});
    });
}

// Selecting a saved naming layout fills the expressions; they stay editable
// and drift back to Custom on the first keystroke.
void ConvertDialog::applySavedLayout(const int combo_index) {
    const auto position = static_cast<std::size_t>(combo_index) - 1U;
    if (combo_index <= 0 || position >= layout_catalog_.size()) {
        return;
    }
    const auto& profile = layout_catalog_[position].profile;
    directory_expression_->setText(displayText(profile.relative_directory_expression));
    basename_expression_->setText(displayText(profile.basename_expression));
}

void ConvertDialog::applySavedDestination(const int combo_index) {
    const auto position = static_cast<std::size_t>(combo_index) - 1U;
    if (combo_index <= 0 || position >= destination_catalog_.size()) {
        return;
    }
    destination_->setText(displayText(destination_catalog_[position].profile.root_raw_path));
}

ConvertDialog::~ConvertDialog() {
    cancellation_.request_cancellation();
    watcher_.waitForFinished();
}

void ConvertDialog::closeEvent(QCloseEvent* event) {
    if (running_) {
        cancellation_.request_cancellation();
        status_->setText(QStringLiteral("Stopping after the files already in flight…"));
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

std::optional<convert::EncoderPreset> ConvertDialog::selectedPreset() const {
    const auto chosen = preset_->currentData().toString().toStdString();
    if (auto builtin = convert::find_encoder_preset(chosen)) {
        return builtin;
    }
    const auto found = std::ranges::find_if(
        saved_presets_, [&chosen](const auto& entry) { return entry.preset.id == chosen; });
    return found != saved_presets_.end() ? std::optional{found->preset} : std::nullopt;
}

void ConvertDialog::refreshPreview() {
    if (running_) {
        return;
    }
    plan_.reset();
    preview_->clear();
    run_->setEnabled(false);

    const auto preset = selectedPreset();
    if (!preset) {
        status_->setText(QStringLiteral("Choose a preset."));
        return;
    }
    if (const auto availability = convert::probe_encoder_preset(*preset); !availability.available) {
        status_->setText(displayText(availability.detail));
        return;
    }
    const auto root = destination_->text().trimmed().toStdString();
    if (root.empty()) {
        status_->setText(QStringLiteral("Choose a destination folder."));
        return;
    }
    std::error_code root_error;
    if (!std::filesystem::is_directory(std::filesystem::path{root}, root_error)) {
        status_->setText(QStringLiteral("The destination folder does not exist."));
        return;
    }

    std::vector<operations::OutputPathPlanningItem> planning_items;
    planning_items.reserve(items_.size());
    QStringList problems;
    for (std::size_t index = 0U; index < items_.size(); ++index) {
        auto& item = items_[index];
        if (!item.source_revision) {
            auto observed = core::observe_local_source_revision(item.raw_path);
            if (!observed) {
                problems.push_back(QStringLiteral("%1: %2").arg(
                    item.label, displayText(observed.error().message)));
                continue;
            }
            item.source_revision = *observed;
        }
        planning_items.push_back(operations::OutputPathPlanningItem{
            .item_index = index,
            .source_raw_path = item.raw_path,
            .source_revision = *item.source_revision,
            .final_metadata = item.metadata,
        });
    }
    if (planning_items.empty()) {
        status_->setText(
            problems.isEmpty()
                ? QStringLiteral("Nothing to convert.")
                : QStringLiteral("No convertible files (%1 with problems).").arg(problems.size()));
        showProblems(problems);
        return;
    }

    operations::OutputLayoutProfile layout;
    layout.name = "convert";
    layout.relative_directory_expression = directory_expression_->text().trimmed().toStdString();
    layout.basename_expression = basename_expression_->text().trimmed().toStdString();
    operations::DestinationProfile destination;
    destination.name = "convert";
    destination.root_raw_path = std::filesystem::path{root}.lexically_normal().native();
    operations::ConvertedPublicationPolicy converted{.target_extension = preset->file_extension,
                                                     .mirror_source_root_raw_path = {}};
    if (mirror_structure_->isChecked()) {
        std::vector<std::string> source_paths;
        source_paths.reserve(planning_items.size());
        for (const auto& planning_item : planning_items) {
            source_paths.push_back(planning_item.source_raw_path);
        }
        auto mirror_root = mirror_root_ != nullptr && !mirror_root_->text().trimmed().isEmpty()
                               ? std::filesystem::path{mirror_root_->text().trimmed().toStdString()}
                                     .lexically_normal()
                                     .native()
                               : operations::common_source_directory_raw_path(source_paths);
        if (mirror_root.empty()) {
            status_->setText(QStringLiteral("No common source folder to mirror."));
            return;
        }
        preview_->addItem(QStringLiteral("Mirroring below %1").arg(displayText(mirror_root)));
        converted.mirror_source_root_raw_path = std::move(mirror_root);
    }
    auto planned = operations::plan_output_paths(
        planning_items, {.rename_files = true, .move_files = true}, std::move(layout),
        std::move(destination), {}, {}, {}, converted);
    if (!planned) {
        status_->setText(displayText(planned.error().message));
        return;
    }

    for (const auto& issue : planned->issues) {
        problems.push_back(displayText(issue.message));
    }
    const auto shown =
        std::min<std::size_t>(planned->sources.size(), static_cast<std::size_t>(preview_limit));
    for (std::size_t index = 0U; index < shown; ++index) {
        const auto& source = planned->sources[index];
        auto relative = source.sanitized_relative_directory;
        if (!relative.empty()) {
            relative += '/';
        }
        relative += source.sanitized_basename + "." + preset->file_extension;
        preview_->addItem(displayText(relative));
    }
    if (planned->sources.size() > shown) {
        preview_->addItem(QStringLiteral("… and %1 more").arg(planned->sources.size() - shown));
    }

    const auto ready = planned->ready();
    if (!problems.isEmpty()) {
        status_->setText(QStringLiteral("%1 problem%2 block the plan.")
                             .arg(problems.size())
                             .arg(problems.size() == 1 ? QString{} : QStringLiteral("s")));
    } else {
        status_->setText(QStringLiteral("%1 file%2 ready.")
                             .arg(planned->sources.size())
                             .arg(planned->sources.size() == 1U ? QString{} : QStringLiteral("s")));
    }
    showProblems(problems);
    plan_ = std::move(*planned);
    run_->setEnabled(ready);
}

void ConvertDialog::startConversion() {
    if (running_ || !plan_ || !plan_->ready()) {
        return;
    }
    const auto preset = selectedPreset();
    if (!preset) {
        return;
    }

    QSettings settings;
    settings.setValue(QStringLiteral("convert/preset"), preset_->currentData().toString());
    settings.setValue(QStringLiteral("convert/destination-root"), destination_->text());
    settings.setValue(QStringLiteral("convert/directory-expression"),
                      directory_expression_->text());
    settings.setValue(QStringLiteral("convert/basename-expression"), basename_expression_->text());
    settings.setValue(QStringLiteral("convert/parallelism"), parallelism_->value());
    settings.setValue(QStringLiteral("convert/resample-rate"), resample_->currentData().toInt());
    settings.setValue(QStringLiteral("convert/bit-depth"), bit_depth_->currentData().toInt());
    settings.setValue(QStringLiteral("convert/embed-artwork"), embed_artwork_->isChecked());
    settings.setValue(QStringLiteral("convert/mirror-root"), mirror_root_->text());
    settings.setValue(QStringLiteral("convert/mirror-structure"), mirror_structure_->isChecked());

    std::vector<convert::ConversionScanItem> scan_items;
    scan_items.reserve(plan_->sources.size());
    for (const auto& source : plan_->sources) {
        for (const auto item_index : source.item_indexes) {
            const auto& item = items_[item_index];
            scan_items.push_back(convert::ConversionScanItem{
                .item_index = item_index,
                .source_raw_path = item.raw_path,
                .selection = item.selection,
                .range = item.segment,
                .destination_raw_path = source.target_raw_path,
                .metadata = item.metadata,
            });
        }
    }

    running_ = true;
    running_total_ = scan_items.size();
    cancellation_ = core::CancellationSource{};
    completed_ = std::make_shared<std::atomic_size_t>(0U);
    run_->setVisible(false);
    stop_->setVisible(true);
    progress_->setVisible(true);
    progress_->setRange(0, static_cast<int>(running_total_));
    progress_->setValue(0);
    status_->setText(QStringLiteral("Converting · 0 of %1 files").arg(running_total_));

    auto* poll = new QTimer(this);
    poll->setInterval(100);
    connect(poll, &QTimer::timeout, this, [this] {
        const auto done = completed_ ? completed_->load() : 0U;
        progress_->setValue(static_cast<int>(done));
        status_->setText(
            QStringLiteral("Converting · %1 of %2 files").arg(done).arg(running_total_));
    });
    connect(&watcher_, &QFutureWatcherBase::finished, poll, &QObject::deleteLater);
    poll->start();

    const auto cancellation = cancellation_.token();
    const auto parallelism = static_cast<std::size_t>(parallelism_->value());
    const auto resample_rate = resample_->currentData().toInt();
    const auto target_sample_rate = resample_rate > 0 ? std::optional{resample_rate} : std::nullopt;
    const auto sample_rate_cap = resample_rate < 0 ? std::optional{-resample_rate} : std::nullopt;
    const auto depth_choice = bit_depth_->currentData().toInt();
    const auto target_bit_depth = depth_choice > 0 ? std::optional{depth_choice} : std::nullopt;
    const auto keep_source_depth = depth_choice < 0;
    const auto carry_artwork = embed_artwork_->isChecked();
    watcher_.setFuture(
        QtConcurrent::run([scan_items = std::move(scan_items), preset = *preset, parallelism,
                           target_sample_rate, sample_rate_cap, target_bit_depth, keep_source_depth,
                           carry_artwork, completed = completed_, cancellation] {
            // The conversion core requires existing target directories; create
            // them up front so parallel workers never race directory creation.
            for (const auto& item : scan_items) {
                std::error_code create_error;
                std::filesystem::create_directories(
                    std::filesystem::path{item.destination_raw_path}.parent_path(), create_error);
            }
            auto scan = convert::scan_conversion(
                scan_items,
                {.preset = preset,
                 .maximum_parallelism = parallelism,
                 .target_sample_rate = target_sample_rate,
                 .sample_rate_cap = sample_rate_cap,
                 .target_bit_depth = target_bit_depth,
                 .keep_source_bit_depth = keep_source_depth,
                 .carry_artwork = carry_artwork},
                [completed](const convert::ConversionScanProgress& update) {
                    completed->store(update.completed_items);
                },
                cancellation);
            return std::make_shared<core::Result<convert::ConversionScanResult>>(std::move(scan));
        }));
}

void ConvertDialog::finishConversion() {
    running_ = false;
    stop_->setVisible(false);
    run_->setVisible(true);
    progress_->setVisible(false);
    const auto outcome = watcher_.result();
    if (!outcome || !*outcome) {
        status_->setText(outcome ? displayText((*outcome).error().message)
                                 : QStringLiteral("Conversion failed."));
        refreshPreview();
        return;
    }
    const auto& result = **outcome;
    if (result.converted_count() > 0U) {
        emit filesConverted();
    }
    QStringList problems;
    for (const auto& item : result.items) {
        if (item.state == convert::ConversionScanState::failed && item.issue) {
            problems.push_back(QStringLiteral("%1: %2").arg(items_[item.item_index].label,
                                                            displayText(item.issue->message)));
        }
    }
    auto summary = QStringLiteral("Converted %1 of %2 files.")
                       .arg(result.converted_count())
                       .arg(result.items.size());
    if (result.cancellation_requested) {
        summary += QStringLiteral(" Stopped early.");
    }
    if (!problems.isEmpty()) {
        summary += QStringLiteral(" %1 failed.").arg(problems.size());
    }
    status_->setText(summary);
    showProblems(problems);
}

void ConvertDialog::showProblems(const QStringList& problems) {
    if (problems.isEmpty()) {
        problems_->hide();
        problems_->clear();
        return;
    }
    problems_->setPlainText(problems.join(QStringLiteral("\n")));
    problems_->show();
}

} // namespace trackknife::bench
