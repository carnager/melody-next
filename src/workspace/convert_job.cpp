// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/convert_job.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "trackknife/core/stable_id.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
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

// A saved choice is kept only when it is one of the choices.
[[nodiscard]] QVariant chosen(const std::vector<ConvertJob::Choice>& choices, const QVariant& value,
                              const QVariant& fallback) {
    return std::ranges::any_of(choices,
                               [&value](const auto& choice) { return choice.value == value; })
               ? value
               : fallback;
}

} // namespace

ConvertJob::ConvertJob(std::vector<ConvertDialogItem> items, ConvertProfilesLoader profiles,
                       ConvertPresetStore preset_store, QObject* parent)
    : QObject(parent), items_(std::move(items)), preset_store_(std::move(preset_store)) {
    const QSettings settings;
    destination_ = settings.value(QStringLiteral("convert/destination-root")).toString();
    mirror_ = settings.value(QStringLiteral("convert/mirror-structure"), false).toBool();
    directory_expression_ = settingsText(settings, "convert/directory-expression",
                                         QStringLiteral("%albumartist%/%album%"));
    basename_expression_ = settingsText(settings, "convert/basename-expression",
                                        QStringLiteral("%tracknumber% - %title%"));
    resample_ = chosen(resampleChoices(),
                       settings.value(QStringLiteral("convert/resample-rate"), 0).toInt(), 0);
    bit_depth_ = chosen(bitDepthChoices(),
                        settings.value(QStringLiteral("convert/bit-depth"), 0).toInt(), 0);
    channels_ =
        chosen(channelChoices(), settings.value(QStringLiteral("convert/channels"), 0).toInt(), 0);
    // Never restore a destructive signal-processing choice from last use or
    // a preset.
    gain_ = 0;
    embed_artwork_ = settings.value(QStringLiteral("convert/embed-artwork"), true).toBool();
    parallelism_ = std::clamp(settings.value(QStringLiteral("convert/parallelism"), 4).toInt(), 1,
                              maximumParallelism());
    rebuildPresets(settings.value(QStringLiteral("convert/preset")).toString());

    debounce_.setSingleShot(true);
    // Planning is pure and fast; a short debounce only coalesces bursts of
    // keystrokes.
    debounce_.setInterval(150);
    connect(&debounce_, &QTimer::timeout, this, &ConvertJob::refreshPreview);
    poll_.setInterval(100);
    connect(&poll_, &QTimer::timeout, this, [this] {
        const auto done = completed_ ? completed_->load() : 0U;
        progress_value_ = static_cast<int>(done);
        status_ = QStringLiteral("Converting · %1 of %2 files").arg(done).arg(running_total_);
        emit changed();
    });
    connect(&watcher_, &QFutureWatcherBase::finished, this, &ConvertJob::finishConversion);

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
            emit self->profilesChanged();
            emit self->changed();
        });
    }
    reloadPresets(preset_id_);
    refreshPreview();
}

ConvertJob::~ConvertJob() {
    cancellation_.request_cancellation();
    watcher_.waitForFinished();
}

QString ConvertJob::title() const {
    return QStringLiteral("Convert %1 file%2")
        .arg(items_.size())
        .arg(items_.size() == 1U ? QString{} : QStringLiteral("s"));
}

std::vector<ConvertJob::Choice> ConvertJob::resampleChoices() {
    std::vector<Choice> choices{{QStringLiteral("Keep source rate"), 0}};
    for (const auto rate : {44'100, 48'000, 88'200, 96'000, 176'400, 192'000}) {
        choices.push_back({QStringLiteral("%1 kHz").arg(rate / 1000.0), rate});
    }
    // Downsample-only caps (ADR-0134) are stored as negated rates.
    for (const auto cap : {44'100, 48'000}) {
        choices.push_back(
            {QStringLiteral("Downsample to %1 kHz if higher").arg(cap / 1000.0), -cap});
    }
    return choices;
}

std::vector<ConvertJob::Choice> ConvertJob::bitDepthChoices() {
    return {{QStringLiteral("Preset default"), 0},
            {QStringLiteral("16-bit (dithered)"), 16},
            {QStringLiteral("24-bit"), 24},
            {QStringLiteral("Keep source depth"), -1}};
}

std::vector<ConvertJob::Choice> ConvertJob::channelChoices() {
    return {{QStringLiteral("Keep source channels"), 0},
            {QStringLiteral("Mono"), 1},
            {QStringLiteral("Stereo"), 2}};
}

std::vector<ConvertJob::Choice> ConvertJob::gainChoices() {
    return {{QStringLiteral("Off — do not change volume"), 0},
            {QStringLiteral("Permanently change volume using track ReplayGain"), 1},
            {QStringLiteral("Permanently change volume using album ReplayGain"), 2}};
}

QStringList ConvertJob::editorFormats() {
    QStringList labels;
    for (const auto& entry : editor_formats) {
        labels.push_back(QLatin1String(entry.label));
    }
    return labels;
}

bool ConvertJob::editorFormatLossless(const int format) {
    return format >= 0 && static_cast<std::size_t>(format) < editor_formats.size() &&
           editor_formats[static_cast<std::size_t>(format)].lossless;
}

int ConvertJob::maximumParallelism() {
    return static_cast<int>(convert::maximum_conversion_parallelism);
}

// Presets.

int ConvertJob::presetIndex() const {
    const auto found = std::ranges::find_if(presets_, [this](const Preset& preset) {
        return !preset.separator && preset.id == preset_id_;
    });
    return found == presets_.end() ? -1 : static_cast<int>(std::distance(presets_.begin(), found));
}

bool ConvertJob::canDeletePreset() const {
    const auto chosen = preset_id_.toStdString();
    return preset_store_.remove != nullptr &&
           std::ranges::any_of(saved_presets_,
                               [&chosen](const auto& entry) { return entry.preset.id == chosen; });
}

// The built-ins first, then the user's saved presets; unavailable encoders
// stay listed but cannot be chosen, with the probe's detail.
void ConvertJob::rebuildPresets(const QString& select) {
    gain_ = 0;
    presets_.clear();
    const auto add_preset = [this](const convert::EncoderPreset& preset, const bool saved) {
        const auto availability = convert::probe_encoder_preset(preset);
        presets_.push_back(Preset{.id = displayText(preset.id),
                                  .name = displayText(preset.display_name),
                                  .available = availability.available,
                                  .detail = displayText(availability.detail),
                                  .saved = saved,
                                  .separator = false});
    };
    for (const auto& preset : convert::builtin_encoder_presets()) {
        add_preset(preset, false);
    }
    if (!saved_presets_.empty()) {
        presets_.push_back(Preset{.id = {},
                                  .name = {},
                                  .available = false,
                                  .detail = {},
                                  .saved = false,
                                  .separator = true});
        for (const auto& saved : saved_presets_) {
            add_preset(saved.preset, true);
        }
    }
    const auto found = std::ranges::find_if(presets_, [&select](const Preset& preset) {
        return !preset.separator && preset.id == select;
    });
    preset_id_ =
        found != presets_.end() ? select : (presets_.empty() ? QString{} : presets_.front().id);
    emit presetsChanged();
    emit changed();
}

void ConvertJob::reloadPresets(const QString& select) {
    if (!preset_store_.load) {
        return;
    }
    const QPointer self{this};
    preset_store_.load(
        [self, select](std::vector<persistence::SavedEncoderPreset> presets, const QString& error) {
            if (self == nullptr) {
                return;
            }
            if (!error.isEmpty()) {
                self->status_ = error;
                emit self->changed();
                return;
            }
            self->saved_presets_ = std::move(presets);
            self->rebuildPresets(select);
            self->refreshPreview();
        });
}

void ConvertJob::selectPreset(const QString& id) {
    if (id == preset_id_) {
        return;
    }
    preset_id_ = id;
    presetChosen();
}

void ConvertJob::presetChosen() {
    gain_ = 0;
    if (canDeletePreset()) {
        applyJobSettings(preset_id_);
    }
    schedulePreview();
    emit changed();
}

std::optional<convert::EncoderPreset> ConvertJob::selectedPreset() const {
    const auto chosen = preset_id_.toStdString();
    if (auto builtin = convert::find_encoder_preset(chosen)) {
        return builtin;
    }
    const auto found = std::ranges::find_if(
        saved_presets_, [&chosen](const auto& entry) { return entry.preset.id == chosen; });
    return found != saved_presets_.end() ? std::optional{found->preset} : std::nullopt;
}

ConvertJob::PresetDraft ConvertJob::presetDraft() const {
    PresetDraft draft;
    const auto base = selectedPreset().value_or(convert::EncoderPreset{});
    for (std::size_t index = 0U; index < editor_formats.size(); ++index) {
        if (editor_formats[index].codec == base.codec_name) {
            draft.format = static_cast<int>(index);
            break;
        }
    }
    if (base.vbr_quality) {
        draft.quality_mode = true;
        draft.quality = *base.vbr_quality;
    } else if (base.bit_rate) {
        draft.bitrate_kbps = static_cast<int>(*base.bit_rate / 1'000);
    }
    return draft;
}

void ConvertJob::savePreset(const PresetDraft& draft) {
    if (!preset_store_.save || draft.name.trimmed().isEmpty() || draft.format < 0 ||
        static_cast<std::size_t>(draft.format) >= editor_formats.size()) {
        return;
    }
    const auto id = core::StableId::random();
    const auto& entry = editor_formats[static_cast<std::size_t>(draft.format)];
    persistence::SavedEncoderPreset saved;
    saved.id = id;
    saved.preset = convert::EncoderPreset{
        .id = id.to_string(),
        .version = 1,
        .display_name = draft.name.trimmed().toStdString(),
        .codec_name = entry.codec,
        .container_name = entry.container,
        .file_extension = entry.extension,
        .lossless = entry.lossless,
        .bit_rate = std::nullopt,
        .vbr_quality = std::nullopt,
        .sample_format_hint = entry.sample_format_hint,
    };
    if (!entry.lossless) {
        if (draft.quality_mode) {
            saved.preset.vbr_quality = draft.quality;
        } else {
            saved.preset.bit_rate = static_cast<std::int64_t>(draft.bitrate_kbps) * 1'000;
        }
    }
    const auto select = displayText(saved.preset.id);
    const QPointer self{this};
    preset_store_.save(std::move(saved), [self, select](const QString& error) {
        if (self == nullptr) {
            return;
        }
        if (!error.isEmpty()) {
            self->status_ = error;
            emit self->changed();
            return;
        }
        self->saveJobSettings(select);
        self->reloadPresets(select);
    });
}

QString ConvertJob::suggestedPresetExportName() const {
    return preset_id_ + QStringLiteral(".trackknife-preset.json");
}

void ConvertJob::exportPreset(const QString& path) {
    const auto selected = selectedPreset();
    if (!selected) {
        status_ = QStringLiteral("Select an encoder preset to export.");
        emit changed();
        return;
    }
    if (path.isEmpty()) {
        return;
    }
    QJsonObject preset{
        {QStringLiteral("id"), displayText(selected->id)},
        {QStringLiteral("version"), static_cast<int>(selected->version)},
        {QStringLiteral("display_name"), displayText(selected->display_name)},
        {QStringLiteral("codec"), displayText(selected->codec_name)},
        {QStringLiteral("container"), displayText(selected->container_name)},
        {QStringLiteral("extension"), displayText(selected->file_extension)},
        {QStringLiteral("lossless"), selected->lossless},
        {QStringLiteral("sample_format"), displayText(selected->sample_format_hint)}};
    if (selected->bit_rate) {
        preset.insert(QStringLiteral("bit_rate"), static_cast<qint64>(*selected->bit_rate));
    }
    if (selected->vbr_quality) {
        preset.insert(QStringLiteral("vbr_quality"), *selected->vbr_quality);
    }
    const QJsonObject document{
        {QStringLiteral("format"), QStringLiteral("trackknife-encoder-preset-1")},
        {QStringLiteral("preset"), preset}};
    QSaveFile output{path};
    if (!output.open(QIODevice::WriteOnly) ||
        output.write(QJsonDocument{document}.toJson(QJsonDocument::Indented)) < 0 ||
        !output.commit()) {
        status_ = QStringLiteral("Could not export encoder preset: %1").arg(output.errorString());
        emit changed();
        return;
    }
    status_ = QStringLiteral("Exported encoder preset to %1").arg(path);
    emit changed();
}

void ConvertJob::deletePreset() {
    const auto chosen = preset_id_.toStdString();
    const auto found = std::ranges::find_if(
        saved_presets_, [&chosen](const auto& entry) { return entry.preset.id == chosen; });
    if (found == saved_presets_.end() || !preset_store_.remove) {
        return;
    }
    const QPointer self{this};
    const auto settings_id = displayText(found->preset.id);
    preset_store_.remove(found->id, [self, settings_id](const QString& error) {
        if (self == nullptr) {
            return;
        }
        if (!error.isEmpty()) {
            self->status_ = error;
            emit self->changed();
            return;
        }
        QSettings settings;
        settings.remove(QStringLiteral("convert/job-presets/") + settings_id);
        self->reloadPresets({});
    });
}

void ConvertJob::saveJobSettings(const QString& preset_id) const {
    QSettings settings;
    settings.beginGroup(QStringLiteral("convert/job-presets/") + preset_id);
    settings.setValue(QStringLiteral("schema"), 1);
    settings.setValue(QStringLiteral("backend-versions"),
                      displayText(convert::conversion_backend_versions()));
    settings.setValue(QStringLiteral("destination-root"), destination_);
    settings.setValue(QStringLiteral("directory-expression"), directory_expression_);
    settings.setValue(QStringLiteral("basename-expression"), basename_expression_);
    settings.setValue(QStringLiteral("mirror"), mirror_);
    settings.setValue(QStringLiteral("resample"), resample_);
    settings.setValue(QStringLiteral("bit-depth"), bit_depth_);
    settings.setValue(QStringLiteral("channels"), channels_);
    settings.remove(QStringLiteral("gain"));
    settings.setValue(QStringLiteral("artwork"), embed_artwork_);
    settings.setValue(QStringLiteral("parallelism"), parallelism_);
    settings.endGroup();
}

void ConvertJob::applyJobSettings(const QString& preset_id) {
    QSettings settings;
    settings.beginGroup(QStringLiteral("convert/job-presets/") + preset_id);
    if (settings.value(QStringLiteral("schema")).toInt() != 1) {
        settings.endGroup();
        return;
    }
    destination_ = settings.value(QStringLiteral("destination-root")).toString();
    directory_expression_ = settings.value(QStringLiteral("directory-expression")).toString();
    basename_expression_ = settings.value(QStringLiteral("basename-expression")).toString();
    mirror_ = settings.value(QStringLiteral("mirror")).toBool();
    // A saved value is kept only when it is one of the choices (as a
    // combo's findData would), compared as numbers.
    const auto select = [&settings](const std::vector<Choice>& choices, const char* key,
                                    QVariant& target) {
        const auto value = settings.value(QLatin1String(key));
        for (const auto& choice : choices) {
            if (choice.value.toInt() == value.toInt() && value.isValid()) {
                target = choice.value;
            }
        }
    };
    select(resampleChoices(), "resample", resample_);
    select(bitDepthChoices(), "bit-depth", bit_depth_);
    select(channelChoices(), "channels", channels_);
    gain_ = 0;
    embed_artwork_ = settings.value(QStringLiteral("artwork"), true).toBool();
    parallelism_ = std::clamp(settings.value(QStringLiteral("parallelism"), 2).toInt(), 1,
                              maximumParallelism());
    settings.endGroup();
}

// The form.

QStringList ConvertJob::layouts() const {
    QStringList names{QStringLiteral("Custom")};
    for (const auto& saved : layout_catalog_) {
        names.push_back(displayText(saved.profile.name));
    }
    return names;
}

QStringList ConvertJob::destinations() const {
    QStringList names{QStringLiteral("Custom")};
    for (const auto& destination : destination_catalog_) {
        names.push_back(displayText(destination.profile.name));
    }
    return names;
}

QString ConvertJob::gainWarning() const {
    return gain_ == 0
               ? QStringLiteral(
                     "No permanent volume adjustment. This does not calculate ReplayGain tags.")
               : QStringLiteral(
                     "<b>Warning: permanently changes the audio samples in the converted "
                     "files.</b> Uses existing ReplayGain values; does not calculate or write new "
                     "gain tags. Removing tags cannot undo this. Source files are not changed.");
}

QString ConvertJob::gainConfirmation() const {
    return QStringLiteral(
               "This will bake %1 ReplayGain into the audio samples of the converted files. It "
               "does not calculate or write ReplayGain tags. Removing tags cannot undo the "
               "change; recreate the outputs from the original sources instead.\n\n"
               "Source files will not be changed.")
        .arg(gain_ == 2 ? QStringLiteral("album (with track fallback)") : QStringLiteral("track"));
}

void ConvertJob::setDestination(const QString& root, const bool typed) {
    if (typed) {
        destination_choice_ = 0;
    }
    if (destination_ != root) {
        destination_ = root;
        schedulePreview();
    }
    emit changed();
}

// Selecting a saved naming layout fills the expressions; they stay editable
// and drift back to Custom on the first keystroke.
void ConvertJob::selectLayout(const int choice) {
    layout_choice_ = choice;
    const auto position = static_cast<std::size_t>(choice) - 1U;
    if (choice > 0 && position < layout_catalog_.size()) {
        const auto& profile = layout_catalog_[position].profile;
        directory_expression_ = displayText(profile.relative_directory_expression);
        basename_expression_ = displayText(profile.basename_expression);
        schedulePreview();
    }
    emit changed();
}

void ConvertJob::selectDestination(const int choice) {
    destination_choice_ = choice;
    const auto position = static_cast<std::size_t>(choice) - 1U;
    if (choice > 0 && position < destination_catalog_.size()) {
        destination_ = displayText(destination_catalog_[position].profile.root_raw_path);
        schedulePreview();
    }
    emit changed();
}

void ConvertJob::setMirror(const bool on) {
    mirror_ = on;
    schedulePreview();
    emit changed();
}

void ConvertJob::setDirectoryExpression(const QString& expression, const bool typed) {
    if (typed) {
        layout_choice_ = 0;
    }
    if (directory_expression_ != expression) {
        directory_expression_ = expression;
        schedulePreview();
    }
    emit changed();
}

void ConvertJob::setBasenameExpression(const QString& expression, const bool typed) {
    if (typed) {
        layout_choice_ = 0;
    }
    if (basename_expression_ != expression) {
        basename_expression_ = expression;
        schedulePreview();
    }
    emit changed();
}

void ConvertJob::setResample(const QVariant& value) {
    resample_ = value;
    emit changed();
}

void ConvertJob::setBitDepth(const QVariant& value) {
    bit_depth_ = value;
    emit changed();
}

void ConvertJob::setChannels(const QVariant& value) {
    channels_ = value;
    emit changed();
}

void ConvertJob::setGain(const int value) {
    gain_ = value;
    emit changed();
}

void ConvertJob::setEmbedArtwork(const bool on) {
    embed_artwork_ = on;
    emit changed();
}

void ConvertJob::setParallelism(const int value) {
    parallelism_ = std::clamp(value, 1, maximumParallelism());
    emit changed();
}

// The preview.

void ConvertJob::schedulePreview() { debounce_.start(); }

void ConvertJob::refreshPreview() {
    if (running_) {
        return;
    }
    plan_.reset();
    preview_.clear();
    const auto finish = [this](const QString& status, QStringList problems = {}) {
        status_ = status;
        problems_ = std::move(problems);
        emit changed();
    };

    const auto preset = selectedPreset();
    if (!preset) {
        finish(QStringLiteral("Choose a preset."));
        return;
    }
    if (const auto availability = convert::probe_encoder_preset(*preset); !availability.available) {
        finish(displayText(availability.detail));
        return;
    }
    const auto root = destination_.trimmed().toStdString();
    if (root.empty()) {
        finish(QStringLiteral("Choose a destination folder."));
        return;
    }
    std::error_code root_error;
    if (!std::filesystem::is_directory(std::filesystem::path{root}, root_error)) {
        finish(QStringLiteral("The destination folder does not exist."));
        return;
    }

    std::vector<operations::OutputPathPlanningItem> planning_items;
    planning_items.reserve(items_.size());
    QStringList problems;
    for (std::size_t index = 0U; index < items_.size(); ++index) {
        auto& item = items_[index];
        if (!item.source_revision && item.fetch) {
            // Not on this computer to look at: known by its path, which is
            // all the plan needs to tell two items apart.
            item.source_revision =
                core::LocalSourceRevision{.device = 0U,
                                          .inode = std::hash<std::string>{}(item.raw_path) | 1U,
                                          .size = 0U,
                                          .modification_time_seconds = 0,
                                          .modification_time_nanoseconds = 0};
        }
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
        finish(
            problems.isEmpty()
                ? QStringLiteral("Nothing to convert.")
                : QStringLiteral("No convertible files (%1 with problems).").arg(problems.size()),
            problems);
        return;
    }

    operations::OutputLayoutProfile layout;
    layout.name = "convert";
    layout.relative_directory_expression = directory_expression_.trimmed().toStdString();
    layout.basename_expression = basename_expression_.trimmed().toStdString();
    operations::DestinationProfile destination;
    destination.name = "convert";
    destination.root_raw_path = std::filesystem::path{root}.lexically_normal().native();
    operations::ConvertedPublicationPolicy converted{.target_extension = preset->file_extension,
                                                     .mirror_source_root_raw_path = {}};
    if (mirror_) {
        // ADR-0154: mirror recreates each source's complete folder path
        // beneath the destination -- no root inference, no extra knob.
        preview_.push_back(QStringLiteral("Recreating full source paths beneath the destination"));
        converted.mirror_source_root_raw_path = "/";
    }
    auto planned = operations::plan_output_paths(
        planning_items, {.rename_files = true, .move_files = true}, std::move(layout),
        std::move(destination), {}, {}, {}, converted);
    if (!planned) {
        finish(displayText(planned.error().message), problems_);
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
        preview_.push_back(displayText(relative));
    }
    if (planned->sources.size() > shown) {
        preview_.push_back(QStringLiteral("… and %1 more").arg(planned->sources.size() - shown));
    }
    const auto status =
        !problems.isEmpty()
            ? QStringLiteral("%1 problem%2 block the plan.")
                  .arg(problems.size())
                  .arg(problems.size() == 1 ? QString{} : QStringLiteral("s"))
            : QStringLiteral("%1 file%2 ready.")
                  .arg(planned->sources.size())
                  .arg(planned->sources.size() == 1U ? QString{} : QStringLiteral("s"));
    plan_ = std::move(*planned);
    finish(status, problems);
}

// Converting.

void ConvertJob::run() {
    if (!canRun()) {
        return;
    }
    const auto preset = selectedPreset();
    if (!preset) {
        return;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("convert/preset"), preset_id_);
    settings.setValue(QStringLiteral("convert/destination-root"), destination_);
    settings.setValue(QStringLiteral("convert/directory-expression"), directory_expression_);
    settings.setValue(QStringLiteral("convert/basename-expression"), basename_expression_);
    settings.setValue(QStringLiteral("convert/parallelism"), parallelism_);
    settings.setValue(QStringLiteral("convert/resample-rate"), resample_.toInt());
    settings.setValue(QStringLiteral("convert/bit-depth"), bit_depth_.toInt());
    settings.setValue(QStringLiteral("convert/channels"), channels_.toInt());
    settings.remove(QStringLiteral("convert/gain"));
    settings.setValue(QStringLiteral("convert/embed-artwork"), embed_artwork_);
    settings.setValue(QStringLiteral("convert/mirror-structure"), mirror_);

    std::vector<convert::ConversionScanItem> scan_items;
    scan_items.reserve(plan_->sources.size());
    std::map<std::size_t, decltype(ConvertDialogItem::fetch)> fetches;
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
            if (item.fetch) {
                fetches.emplace(item_index, item.fetch);
            }
        }
    }

    running_ = true;
    running_total_ = scan_items.size();
    cancellation_ = core::CancellationSource{};
    completed_ = std::make_shared<std::atomic_size_t>(0U);
    progress_maximum_ = static_cast<int>(running_total_);
    progress_value_ = 0;
    status_ = QStringLiteral("Converting · 0 of %1 files").arg(running_total_);
    poll_.start();
    emit changed();

    const auto cancellation = cancellation_.token();
    const auto parallelism = static_cast<std::size_t>(parallelism_);
    const auto resample_rate = resample_.toInt();
    const auto target_sample_rate = resample_rate > 0 ? std::optional{resample_rate} : std::nullopt;
    const auto sample_rate_cap = resample_rate < 0 ? std::optional{-resample_rate} : std::nullopt;
    const auto depth_choice = bit_depth_.toInt();
    const auto target_bit_depth = depth_choice > 0 ? std::optional{depth_choice} : std::nullopt;
    const auto keep_source_depth = depth_choice < 0;
    const auto channel_policy = channels_.toInt() == 1   ? convert::ConversionChannelPolicy::mono
                                : channels_.toInt() == 2 ? convert::ConversionChannelPolicy::stereo
                                                         : convert::ConversionChannelPolicy::keep;
    const auto gain_mode = gain_ == 1   ? convert::ConversionGainMode::track
                           : gain_ == 2 ? convert::ConversionGainMode::album
                                        : convert::ConversionGainMode::none;
    const auto carry_artwork = embed_artwork_;
    watcher_.setFuture(QtConcurrent::run(
        [scan_items = std::move(scan_items), fetches = std::move(fetches), preset = *preset,
         parallelism, target_sample_rate, sample_rate_cap, target_bit_depth, keep_source_depth,
         channel_policy, gain_mode, carry_artwork, completed = completed_, cancellation]() mutable {
            // ADR-0237 stage 6: files of an engine elsewhere, fetched first
            // into a folder of their own and converted from there.
            std::vector<convert::ConvertedItemScan> fetch_failures;
            std::optional<std::filesystem::path> fetched_folder;
            if (!fetches.empty()) {
                // On disk, not in /tmp: a batch of originals can be large.
                fetched_folder =
                    std::filesystem::path{QFile::encodeName(QStandardPaths::writableLocation(
                                                                QStandardPaths::CacheLocation))
                                              .toStdString()} /
                    ("convert-" + core::StableId::random().to_string());
                std::vector<convert::ConversionScanItem> reachable;
                for (auto& item : scan_items) {
                    const auto fetch = fetches.find(item.item_index);
                    if (fetch == fetches.end()) {
                        reachable.push_back(std::move(item));
                        continue;
                    }
                    const auto folder = *fetched_folder / std::to_string(item.item_index);
                    std::error_code made;
                    std::filesystem::create_directories(folder, made);
                    const auto copy =
                        folder / std::filesystem::path{item.source_raw_path}.filename();
                    auto fetched = made ? core::Result<void>{std::unexpected(
                                              core::Error{.code = core::ErrorCode::io,
                                                          .message = made.message(),
                                                          .context = {}})}
                                        : fetch->second(copy, cancellation);
                    if (!fetched) {
                        fetch_failures.push_back(convert::ConvertedItemScan{
                            .item_index = item.item_index,
                            .source_raw_path = item.source_raw_path,
                            .destination_raw_path = item.destination_raw_path,
                            .state = convert::ConversionScanState::failed,
                            .converted = std::nullopt,
                            .source_revision = std::nullopt,
                            .issue = std::move(fetched.error())});
                        continue;
                    }
                    item.source_raw_path = copy.native();
                    reachable.push_back(std::move(item));
                }
                scan_items = std::move(reachable);
            }
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
                 .channel_policy = channel_policy,
                 .gain_mode = gain_mode,
                 .carry_artwork = carry_artwork},
                [completed](const convert::ConversionScanProgress& update) {
                    completed->store(update.completed_items);
                },
                cancellation);
            if (fetched_folder) {
                std::error_code removed;
                std::filesystem::remove_all(*fetched_folder, removed);
            }
            if (scan) {
                scan->items.insert(scan->items.end(),
                                   std::make_move_iterator(fetch_failures.begin()),
                                   std::make_move_iterator(fetch_failures.end()));
            }
            return std::make_shared<core::Result<convert::ConversionScanResult>>(std::move(scan));
        }));
}

void ConvertJob::stop() { cancellation_.request_cancellation(); }

bool ConvertJob::requestClose() {
    if (!running_) {
        return true;
    }
    cancellation_.request_cancellation();
    status_ = QStringLiteral("Stopping after the files already in flight…");
    emit changed();
    return false;
}

void ConvertJob::finishConversion() {
    running_ = false;
    poll_.stop();
    const auto outcome = watcher_.result();
    if (!outcome || !*outcome) {
        status_ = outcome ? displayText((*outcome).error().message)
                          : QStringLiteral("Conversion failed.");
        emit changed();
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
    status_ = summary;
    problems_ = problems;
    emit changed();
}

} // namespace trackknife::bench
