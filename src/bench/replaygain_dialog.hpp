// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "workspace/replaygain_job.hpp"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QListWidget;
class QProgressBar;

namespace trackknife::bench {

// ADR-0156: the compact scan-and-write ReplayGain surface for the track
// context menu, a view over a ReplayGainJob.
class ReplayGainDialog final : public QDialog {
    Q_OBJECT

  public:
    ReplayGainDialog(std::size_t item_count, MetadataPropertiesSourceReader source_reader,
                     MetadataWritePlanApplierFactory plan_applier_factory,
                     MetadataApplyObserver apply_observer, QWidget* parent = nullptr,
                     FileWorkTools tools = {});

  private:
    void sync();

    ReplayGainJob* job_{nullptr};
    QComboBox* grouping_{nullptr};
    QLineEdit* expression_{nullptr};
    QCheckBox* sidecar_only_{nullptr};
    QCheckBox* true_peak_{nullptr};
    QLabel* status_{nullptr};
    QPlainTextEdit* problems_{nullptr};
    QPushButton* run_{nullptr};
    QPushButton* preview_{nullptr};
    QPushButton* stop_{nullptr};
    QListWidget* groups_{nullptr};
    QProgressBar* progress_{nullptr};
};

} // namespace trackknife::bench
