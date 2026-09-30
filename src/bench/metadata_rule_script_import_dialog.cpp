// SPDX-License-Identifier: GPL-3.0-only

#include "bench/metadata_rule_script_import_dialog.hpp"
#include "workspace/script_session.hpp"

#include "bench/metadata_dialog_helpers.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

#include <utility>

namespace trackknife::bench {

MetadataRuleScriptImportDialog::MetadataRuleScriptImportDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("bench-metadata-rule-script-import"));
    setWindowTitle(QStringLiteral("Generate rules from script"));
    setWindowModality(Qt::WindowModal);
    resize(760, 560);

    auto* layout = new QVBoxLayout(this);
    auto* explanation = new QLabel(
        QStringLiteral("Paste a Picard-style cleanup script. Trackknife translates the "
                       "supported $unset/$delete, $set, $if, $and/$or, $eq/$ne, $not, and "
                       "$left subset into editable typed rules; it does not store or execute "
                       "the pasted script. Here, $unset generates an actual Remove field "
                       "rule. Imported removals match the exact native field name, ignoring "
                       "ASCII case but preserving separators."),
        this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);

    source_ = new QPlainTextEdit(this);
    source_->setObjectName(QStringLiteral("bench-metadata-rule-script-source"));
    source_->setPlaceholderText(QStringLiteral("$unset(comment)\n$set(date,$left(%date%,4))"));
    layout->addWidget(source_, 2);

    diagnostics_ = new QPlainTextEdit(this);
    diagnostics_->setObjectName(QStringLiteral("bench-metadata-rule-script-diagnostics"));
    diagnostics_->setReadOnly(true);
    diagnostics_->setMaximumBlockCount(128);
    diagnostics_->setMaximumHeight(150);
    layout->addWidget(diagnostics_);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    append_ =
        buttons_->addButton(QStringLiteral("Append generated rules"), QDialogButtonBox::ActionRole);
    append_->setObjectName(QStringLiteral("bench-metadata-rule-script-append"));
    replace_ = buttons_->addButton(QStringLiteral("Replace rules"), QDialogButtonBox::AcceptRole);
    replace_->setObjectName(QStringLiteral("bench-metadata-rule-script-replace"));
    layout->addWidget(buttons_);

    connect(source_, &QPlainTextEdit::textChanged, this, [this] { updateTranslation(); });
    connect(append_, &QPushButton::clicked, this, [this] {
        mode_ = ImportMode::append;
        accept();
    });
    connect(replace_, &QPushButton::clicked, this, [this] {
        mode_ = ImportMode::replace;
        accept();
    });
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    updateTranslation();
}

MetadataRuleScriptImportDialog::ImportMode
MetadataRuleScriptImportDialog::importMode() const noexcept {
    return mode_;
}

QString MetadataRuleScriptImportDialog::sourceText() const { return source_->toPlainText(); }

void MetadataRuleScriptImportDialog::updateTranslation() {
    const auto translated = ScriptSession::translateRuleScript(source_->toPlainText());
    diagnostics_->setPlainText(translated.diagnostics);
    append_->setEnabled(translated.ready);
    replace_->setEnabled(translated.ready);
}

} // namespace trackknife::bench
