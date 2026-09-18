// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/convert/scan.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/operations/output_path_plan.hpp"
#include "trackknife/persistence/list_repository.hpp"

#include <QDialog>
#include <QFutureWatcher>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QPlainTextEdit;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSpinBox;

namespace trackknife::bench {

struct ConvertDialogItem {
    std::string raw_path;
    formats::AudioSourceSelection selection;
    std::optional<formats::SampleRange> segment;
    std::optional<core::LocalSourceRevision> source_revision;
    metadata::MetadataDocument metadata;
    QString label;
};

// Loads the saved naming layouts and destination roots the rest of the app
// already manages so the converter offers them as one-click choices.
using ConvertProfilesLoader = std::function<void(
    std::function<void(std::vector<persistence::SavedOutputLayoutProfile>,
                       std::vector<persistence::SavedDestinationProfile>, QString)>)>;

// Saved encoder presets beside the built-ins: load populates the preset
// combo, save persists a new profile (editing always saves a new one —
// built-ins are immutable), remove deletes a saved profile.
struct ConvertPresetStore {
    using LoadCompletion =
        std::function<void(std::vector<persistence::SavedEncoderPreset>, QString)>;
    using Completion = std::function<void(QString)>;

    std::function<void(LoadCompletion)> load;
    std::function<void(persistence::SavedEncoderPreset, Completion)> save;
    std::function<void(core::StableId, Completion)> remove;
};

// Converts the current selection below a destination root (ADR-0107):
// preset choice with probed availability, a tkfmt-1 naming layout with a
// live target preview, and a direct bounded parallel conversion with
// problems-only feedback — no review step between preview and output.
class ConvertDialog final : public QDialog {
    Q_OBJECT

  public:
    explicit ConvertDialog(std::vector<ConvertDialogItem> items,
                           ConvertProfilesLoader profiles = {},
                           ConvertPresetStore preset_store = {}, QWidget* parent = nullptr);
    ~ConvertDialog() override;

  signals:
    void filesConverted();

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    void refreshPreview();
    void startConversion();
    void finishConversion();
    [[nodiscard]] std::optional<convert::EncoderPreset> selectedPreset() const;

    void applySavedLayout(int combo_index);
    void applySavedDestination(int combo_index);
    void reloadPresets(const QString& select_data);
    void rebuildPresetCombo(const QString& select_data);
    void openPresetEditor();
    void exportSelectedPreset();
    void deleteSelectedPreset();
    void saveJobSettings(const QString& preset_id) const;
    void applyJobSettings(const QString& preset_id);

    std::vector<ConvertDialogItem> items_;
    std::vector<persistence::SavedOutputLayoutProfile> layout_catalog_;
    std::vector<persistence::SavedDestinationProfile> destination_catalog_;
    ConvertPresetStore preset_store_;
    std::vector<persistence::SavedEncoderPreset> saved_presets_;
    QFormLayout* form_{nullptr};
    QComboBox* preset_{nullptr};
    QPushButton* preset_new_{nullptr};
    QPushButton* preset_export_{nullptr};
    QPushButton* preset_delete_{nullptr};
    QComboBox* layout_choice_{nullptr};
    QComboBox* destination_choice_{nullptr};
    QLineEdit* destination_{nullptr};
    QLineEdit* directory_expression_{nullptr};
    QLineEdit* basename_expression_{nullptr};
    QComboBox* resample_{nullptr};
    QComboBox* bit_depth_{nullptr};
    QComboBox* channels_{nullptr};
    QComboBox* gain_{nullptr};
    QCheckBox* embed_artwork_{nullptr};
    QCheckBox* mirror_structure_{nullptr};
    QSpinBox* parallelism_{nullptr};
    QListWidget* preview_{nullptr};
    QLabel* status_{nullptr};
    QPlainTextEdit* problems_{nullptr};
    void showProblems(const QStringList& problems);
    QProgressBar* progress_{nullptr};
    QPushButton* run_{nullptr};
    QPushButton* stop_{nullptr};
    QPushButton* close_{nullptr};

    std::optional<operations::OutputPathPlan> plan_;
    bool running_{false};
    core::CancellationSource cancellation_;
    std::shared_ptr<std::atomic_size_t> completed_;
    std::size_t running_total_{0U};
    QFutureWatcher<std::shared_ptr<core::Result<convert::ConversionScanResult>>> watcher_;
};

} // namespace trackknife::bench
