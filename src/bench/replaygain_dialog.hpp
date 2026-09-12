// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/metadata_properties_dialog.hpp"
#include "bench/replaygain_scan.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/operations/metadata_apply.hpp"

#include <QDialog>
#include <QFutureWatcher>

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTimer;

namespace trackknife::bench {

// ADR-0156: the compact scan-and-write ReplayGain surface for the track
// context menu. Options share the Properties QSettings keys; capture,
// measurement, proposals, and the journaled write pipeline are the same
// code paths Properties uses — without the review grid.
class ReplayGainDialog final : public QDialog {
    Q_OBJECT

  public:
    ReplayGainDialog(std::size_t item_count, MetadataPropertiesSourceReader source_reader,
                     MetadataWritePlanApplierFactory plan_applier_factory,
                     MetadataApplyObserver apply_observer, QWidget* parent = nullptr);
    ~ReplayGainDialog() override;

  private:
    struct Capture {
        core::Result<metadata::StagedMetadataSelection> selection{
            metadata::StagedMetadataSelection{}};
        std::vector<MetadataPropertiesAudioSource> audio;
    };
    struct ApplyOutcome {
        core::Result<operations::MetadataApplyResult> result{operations::MetadataApplyResult{}};
        QStringList problems;
        std::size_t staged_fields{0U};
    };

    void startRun();
    void finishCapture();
    void finishScan();
    void finishApply();
    void showProblems(const QStringList& problems);
    void setRunning(bool running);

    std::size_t item_count_{0U};
    MetadataPropertiesSourceReader source_reader_;
    MetadataWritePlanApplierFactory plan_applier_factory_;
    MetadataApplyObserver apply_observer_;

    QComboBox* grouping_{nullptr};
    QLineEdit* expression_{nullptr};
    QCheckBox* sidecar_only_{nullptr};
    QCheckBox* true_peak_{nullptr};
    QLabel* status_{nullptr};
    QPlainTextEdit* problems_{nullptr};
    QPushButton* run_{nullptr};

    QFutureWatcher<std::shared_ptr<Capture>> capture_watcher_;
    QFutureWatcher<std::shared_ptr<ReplayGainScanOutcome>> scan_watcher_;
    QFutureWatcher<std::shared_ptr<ApplyOutcome>> apply_watcher_;
    core::CancellationSource cancellation_;
    std::shared_ptr<std::atomic_size_t> completed_;
    QTimer* progress_timer_{nullptr};
    bool running_{false};

    std::shared_ptr<const metadata::StagedMetadataSelection> selection_;
    std::shared_ptr<const std::vector<MetadataPropertiesAudioSource>> audio_sources_;
    ReplayGainScanSettings settings_;
    QStringList scan_problems_;
};

} // namespace trackknife::bench
