// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/convert/preset.hpp"
#include "trackknife/convert/scan.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/operations/output_path_plan.hpp"
#include "trackknife/persistence/list_repository.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariant>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::bench {

struct ConvertDialogItem {
    std::string raw_path;
    formats::AudioSourceSelection selection;
    std::optional<formats::SampleRange> segment;
    std::optional<core::LocalSourceRevision> source_revision;
    metadata::MetadataDocument metadata;
    QString label;
    // ADR-0237 stage 6: a file of an engine elsewhere that this computer
    // cannot reach -- fetched from that engine into the given path, and
    // converted from there. Empty: read at raw_path.
    std::function<core::Result<void>(const std::filesystem::path&, const core::CancellationToken&)>
        fetch;
};

// Loads the saved naming layouts and destination roots the rest of the app
// already manages so the converter offers them as one-click choices.
using ConvertProfilesLoader = std::function<void(
    std::function<void(std::vector<persistence::SavedOutputLayoutProfile>,
                       std::vector<persistence::SavedDestinationProfile>, QString)>)>;

// Saved encoder presets beside the built-ins: load populates the preset
// choice, save persists a new profile (editing always saves a new one --
// built-ins are immutable), remove deletes a saved profile.
struct ConvertPresetStore {
    using LoadCompletion =
        std::function<void(std::vector<persistence::SavedEncoderPreset>, QString)>;
    using Completion = std::function<void(QString)>;

    std::function<void(LoadCompletion)> load;
    std::function<void(persistence::SavedEncoderPreset, Completion)> save;
    std::function<void(core::StableId, Completion)> remove;
};

// Converting the selection below a destination root (ADR-0107): a preset
// with probed availability, a tkfmt-1 naming layout (or the source folders
// mirrored) with a live target preview, resampling, depth, channels, an
// optional permanent volume change, cover art, and a bounded parallel
// conversion with problems-only feedback. Choices are remembered, and per
// saved preset. Both windows' Convert dialogs draw it.
class ConvertJob final : public QObject {
    Q_OBJECT

  public:
    struct Choice {
        QString label;
        QVariant value;
    };
    // A preset row: a built-in or saved preset, or the line between them.
    struct Preset {
        QString id;
        QString name;
        bool available{true};
        QString detail;
        bool saved{false};
        bool separator{false};
    };
    // The new-preset editor: one fixed encode format per row keeps user
    // presets inside the qualified encoder set.
    struct PresetDraft {
        QString name;
        int format{0};
        bool quality_mode{false};
        int bitrate_kbps{192};
        int quality{4};
    };

    ConvertJob(std::vector<ConvertDialogItem> items, ConvertProfilesLoader profiles = {},
               ConvertPresetStore preset_store = {}, QObject* parent = nullptr);
    ~ConvertJob() override;

    [[nodiscard]] QString title() const;
    [[nodiscard]] static std::vector<Choice> resampleChoices();
    [[nodiscard]] static std::vector<Choice> bitDepthChoices();
    [[nodiscard]] static std::vector<Choice> channelChoices();
    [[nodiscard]] static std::vector<Choice> gainChoices();
    [[nodiscard]] static QStringList editorFormats();
    [[nodiscard]] static bool editorFormatLossless(int format);
    [[nodiscard]] static int maximumParallelism();

    // What is shown.
    [[nodiscard]] const std::vector<Preset>& presets() const { return presets_; }
    [[nodiscard]] int presetIndex() const;
    [[nodiscard]] bool canAddPreset() const { return static_cast<bool>(preset_store_.load); }
    [[nodiscard]] bool canDeletePreset() const;
    [[nodiscard]] QStringList layouts() const;
    [[nodiscard]] QStringList destinations() const;
    [[nodiscard]] int layoutChoice() const { return layout_choice_; }
    [[nodiscard]] int destinationChoice() const { return destination_choice_; }
    [[nodiscard]] QString destination() const { return destination_; }
    [[nodiscard]] bool mirror() const { return mirror_; }
    [[nodiscard]] QString directoryExpression() const { return directory_expression_; }
    [[nodiscard]] QString basenameExpression() const { return basename_expression_; }
    [[nodiscard]] QVariant resample() const { return resample_; }
    [[nodiscard]] QVariant bitDepth() const { return bit_depth_; }
    [[nodiscard]] QVariant channels() const { return channels_; }
    [[nodiscard]] int gain() const { return gain_; }
    [[nodiscard]] QString gainWarning() const;
    [[nodiscard]] bool gainWarningRich() const { return gain_ != 0; }
    [[nodiscard]] bool embedArtwork() const { return embed_artwork_; }
    [[nodiscard]] int parallelism() const { return parallelism_; }
    [[nodiscard]] QStringList preview() const { return preview_; }
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] QStringList problems() const { return problems_; }
    [[nodiscard]] bool running() const { return running_; }
    [[nodiscard]] bool canRun() const { return !running_ && plan_ && plan_->ready(); }
    [[nodiscard]] int progressValue() const { return progress_value_; }
    [[nodiscard]] int progressMaximum() const { return progress_maximum_; }
    // A permanent volume change is confirmed first: this is what is asked.
    [[nodiscard]] QString gainConfirmation() const;
    // The editor, prefilled from the selected preset.
    [[nodiscard]] PresetDraft presetDraft() const;
    [[nodiscard]] QString suggestedPresetExportName() const;

    // What the user does. A typed expression or root leaves the saved
    // layout or destination it came from.
    void selectPreset(const QString& id);
    void setDestination(const QString& root, bool typed);
    void selectLayout(int choice);
    void selectDestination(int choice);
    void setMirror(bool on);
    void setDirectoryExpression(const QString& expression, bool typed);
    void setBasenameExpression(const QString& expression, bool typed);
    void setResample(const QVariant& value);
    void setBitDepth(const QVariant& value);
    void setChannels(const QVariant& value);
    void setGain(int value);
    void setEmbedArtwork(bool on);
    void setParallelism(int value);
    // Converting; with a volume change, only once confirmed.
    void run();
    void stop();
    void savePreset(const PresetDraft& draft);
    void exportPreset(const QString& path);
    void deletePreset();
    // Closing while converting stops it first.
    [[nodiscard]] bool requestClose();

  signals:
    void changed();
    void presetsChanged();
    void profilesChanged();
    void filesConverted();

  private:
    void schedulePreview();
    void refreshPreview();
    void reloadPresets(const QString& select);
    void rebuildPresets(const QString& select);
    void presetChosen();
    void saveJobSettings(const QString& preset_id) const;
    void applyJobSettings(const QString& preset_id);
    void finishConversion();
    [[nodiscard]] std::optional<convert::EncoderPreset> selectedPreset() const;

    std::vector<ConvertDialogItem> items_;
    ConvertPresetStore preset_store_;
    std::vector<persistence::SavedOutputLayoutProfile> layout_catalog_;
    std::vector<persistence::SavedDestinationProfile> destination_catalog_;
    std::vector<persistence::SavedEncoderPreset> saved_presets_;
    std::vector<Preset> presets_;
    QString preset_id_;
    int layout_choice_{0};
    int destination_choice_{0};
    QString destination_;
    bool mirror_{false};
    QString directory_expression_;
    QString basename_expression_;
    QVariant resample_{0};
    QVariant bit_depth_{0};
    QVariant channels_{0};
    int gain_{0};
    bool embed_artwork_{true};
    int parallelism_{4};
    QStringList preview_;
    QString status_;
    QStringList problems_;
    int progress_value_{0};
    int progress_maximum_{0};
    QTimer debounce_;
    QTimer poll_;

    std::optional<operations::OutputPathPlan> plan_;
    bool running_{false};
    core::CancellationSource cancellation_;
    std::shared_ptr<std::atomic_size_t> completed_;
    std::size_t running_total_{0U};
    QFutureWatcher<std::shared_ptr<core::Result<convert::ConversionScanResult>>> watcher_;
};

} // namespace trackknife::bench
