// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/metadata/rule_script_import.hpp"

#include <QDialog>

#include <cstdint>
#include <string>
#include <vector>

class QDialogButtonBox;
class QPlainTextEdit;
class QPushButton;
class QWidget;

namespace trackknife::bench {

class MetadataRuleScriptImportDialog final : public QDialog {
  public:
    enum class ImportMode : std::uint8_t {
        append,
        replace,
    };

    explicit MetadataRuleScriptImportDialog(QWidget* parent);

    [[nodiscard]] ImportMode importMode() const noexcept;
    [[nodiscard]] QString sourceText() const;

  private:
    void updateTranslation();

    ImportMode mode_{ImportMode::replace};
    QPlainTextEdit* source_{nullptr};
    QPlainTextEdit* diagnostics_{nullptr};
    QDialogButtonBox* buttons_{nullptr};
    QPushButton* append_{nullptr};
    QPushButton* replace_{nullptr};
};

} // namespace trackknife::bench
