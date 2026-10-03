// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/replaygain_scan.hpp"
#include "workspace/tagger_session.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace trackknife::bench {

// ADR-0156: scanning the selected tracks' loudness and writing ReplayGain
// tags without the tag editor's review grid -- grouped into albums as
// chosen, previewed first if wanted, measured, planned and written through
// the same journaled pipeline. The options are the tag editor's settings.
// The window's ReplayGain dialog draws it.
class ReplayGainJob final : public QObject {
    Q_OBJECT

  public:
    ReplayGainJob(std::size_t item_count, MetadataPropertiesSourceReader source_reader,
                  MetadataWritePlanApplierFactory plan_applier_factory,
                  MetadataApplyObserver apply_observer, FileWorkTools tools = {},
                  QObject* parent = nullptr);
    ~ReplayGainJob() override;

    [[nodiscard]] static QStringList groupings();
    [[nodiscard]] QString title() const;
    [[nodiscard]] int grouping() const { return grouping_; }
    [[nodiscard]] QString expression() const { return expression_; }
    [[nodiscard]] bool sidecarOnly() const { return sidecar_only_; }
    [[nodiscard]] bool truePeak() const { return true_peak_; }
    [[nodiscard]] QStringList groups() const { return groups_; }
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] QStringList problems() const { return problems_; }
    [[nodiscard]] bool running() const { return running_; }
    [[nodiscard]] bool canStop() const { return running_ && !stopping_; }
    [[nodiscard]] int progressValue() const { return progress_value_; }
    // 0: under way, without a count.
    [[nodiscard]] int progressMaximum() const { return progress_maximum_; }

    void setGrouping(int index);
    void setExpression(const QString& expression);
    void setSidecarOnly(bool on);
    void setTruePeak(bool on);
    // Off: the choices made here are not kept as the ReplayGain dialog's.
    void setRememberChoices(bool on) { remember_choices_ = on; }
    // The album groups the selection falls into, without measuring.
    void preview();
    // Measured and written.
    void run();
    void stop();

  signals:
    void changed();
    // A preview or a scan is over, however it ended.
    void finished();

  private:
    struct Capture {
        core::Result<metadata::StagedMetadataSelection> selection{
            metadata::StagedMetadataSelection{}};
        std::vector<MetadataPropertiesAudioSource> audio;
        QStringList groups;
        // Files whose tags could not be read: not scanned, not written.
        std::vector<std::pair<std::string, core::Error>> unreadable;
    };
    // How far reading the selection's tags has got, told by the worker.
    struct Reading {
        std::atomic_size_t read{0U};
        std::atomic_size_t total{0U};
    };
    struct ApplyOutcome {
        core::Result<operations::MetadataApplyResult> result{operations::MetadataApplyResult{}};
        QStringList problems;
        std::size_t staged_fields{0U};
    };

    void groupsChanged();
    void setStatusText(const QString& text);
    void startCapture();
    // The rows, collected on this thread a few milliseconds at a time: they
    // are the window's list, which only its thread may read.
    void collectSlice();
    void startReading();
    void finishCapture();
    void finishScan();
    void finishApply();
    void setRunning(bool running);

    std::size_t item_count_{0U};
    MetadataPropertiesSourceReader source_reader_;
    MetadataWritePlanApplierFactory plan_applier_factory_;
    MetadataApplyObserver apply_observer_;
    FileWorkTools tools_;

    int grouping_{0};
    QString expression_;
    bool sidecar_only_{false};
    bool true_peak_{false};
    QStringList groups_;
    QString status_;
    QStringList problems_;
    bool preview_only_{false};
    bool remember_choices_{true};
    bool running_{false};
    bool stopping_{false};
    int progress_value_{0};
    int progress_maximum_{0};

    QFutureWatcher<std::shared_ptr<Capture>> capture_watcher_;
    QFutureWatcher<std::shared_ptr<ReplayGainScanOutcome>> scan_watcher_;
    QFutureWatcher<std::shared_ptr<ApplyOutcome>> apply_watcher_;
    core::CancellationSource cancellation_;
    std::shared_ptr<std::atomic_size_t> completed_;
    std::shared_ptr<Reading> reading_;
    std::size_t collect_next_{0U};
    std::vector<metadata::StagedMetadataSource> collected_sources_;
    std::vector<MetadataPropertiesAudioSource> collected_audio_;
    QTimer progress_timer_;

    std::shared_ptr<const metadata::StagedMetadataSelection> selection_;
    std::shared_ptr<const std::vector<MetadataPropertiesAudioSource>> audio_sources_;
    ReplayGainScanSettings settings_;
    QStringList scan_problems_;
};

} // namespace trackknife::bench
