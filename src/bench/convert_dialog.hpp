// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "workspace/convert_job.hpp"

#include <QDialog>

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

// Converts the current selection below a destination root (ADR-0107): a
// view over a ConvertJob.
class ConvertDialog final : public QDialog {
    Q_OBJECT

  public:
    explicit ConvertDialog(std::vector<ConvertDialogItem> items,
                           ConvertProfilesLoader profiles = {},
                           ConvertPresetStore preset_store = {}, QWidget* parent = nullptr);

  signals:
    void filesConverted();

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    void sync();
    void rebuildPresets();
    void rebuildProfiles();
    void startConversion();
    void openPresetEditor();
    void exportSelectedPreset();

    ConvertJob* job_{nullptr};
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
    QLabel* gain_warning_{nullptr};
    QCheckBox* embed_artwork_{nullptr};
    QCheckBox* mirror_structure_{nullptr};
    QSpinBox* parallelism_{nullptr};
    QListWidget* preview_{nullptr};
    QLabel* status_{nullptr};
    QPlainTextEdit* problems_{nullptr};
    QProgressBar* progress_{nullptr};
    QPushButton* run_{nullptr};
    QPushButton* stop_{nullptr};
    QPushButton* close_{nullptr};
};

} // namespace trackknife::bench
