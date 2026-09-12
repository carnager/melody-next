// SPDX-License-Identifier: GPL-3.0-only

#include "bench/metadata_field_review_bar.hpp"

#include "bench/metadata_grid_model.hpp"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

namespace trackknife::bench {

MetadataFieldReviewBar::MetadataFieldReviewBar(QTableView* fields, MetadataAggregateModel* model,
                                               QTableView* files, QWidget* parent)
    : QWidget(parent), fields_(fields), model_(model) {
    setObjectName(QStringLiteral("bench-metadata-field-review"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto* controls = new QHBoxLayout;
    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("bench-metadata-field-filter"));
    search_->setPlaceholderText(tr("Filter fields…"));
    search_->setAccessibleName(tr("Filter metadata field names"));
    search_->setClearButtonEnabled(true);
    search_->setToolTip(tr("Match display or canonical field names; values are not searched."));
    controls->addWidget(search_, 1);
    changed_only_ = new QCheckBox(tr("Changed fields only"), this);
    changed_only_->setObjectName(QStringLiteral("bench-metadata-changed-only"));
    changed_only_->setToolTip(tr("Show fields with staged edits in the selected files."));
    controls->addWidget(changed_only_);
    auto* show_files = new QCheckBox(tr("Show files"), this);
    show_files->setObjectName(QStringLiteral("bench-metadata-show-files"));
    show_files->setChecked(true);
    show_files->setToolTip(tr("Hide the file list to make more room for fields. "
                              "The selected files stay selected."));
    connect(show_files, &QCheckBox::toggled, files, &QWidget::setVisible);
    controls->addWidget(show_files);
    layout->addLayout(controls);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("bench-metadata-field-filter-status"));
    status_->setWordWrap(true);
    layout->addWidget(status_);
    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(40);
    connect(debounce_, &QTimer::timeout, this, &MetadataFieldReviewBar::refresh);
    const auto schedule = [this] { debounce_->start(); };
    connect(search_, &QLineEdit::textChanged, this, schedule);
    connect(changed_only_, &QCheckBox::toggled, this, schedule);
    connect(model_, &QAbstractItemModel::dataChanged, this, schedule);
    connect(model_, &QAbstractItemModel::rowsInserted, this, schedule);
    connect(model_, &QAbstractItemModel::modelReset, this, schedule);
    connect(model_, &MetadataAggregateModel::selectionProjectionChanged, this, schedule);
    connect(model_, &MetadataAggregateModel::draftProjectionChanged, this, schedule);
    refresh();
}

void MetadataFieldReviewBar::revealField(const int row) {
    if (row < 0 || row >= model_->rowCount()) {
        return;
    }
    search_->clear();
    changed_only_->setChecked(false);
    refresh();
}

void MetadataFieldReviewBar::refresh() {
    // Wait for the existing bounded worker projection instead of traversing tracks.
    const auto ready = model_->summaryReady() && model_->draftPreviewReady();
    if (changed_only_->isChecked() && !ready) {
        status_->setText(tr("Updating changed fields… · Apply includes hidden edits."));
        return;
    }
    const auto query = search_->text().trimmed();
    int visible = 0;
    int changed = 0;
    QItemSelection hidden_selection;
    for (int row = 0; row < model_->rowCount(); ++row) {
        const auto field = model_->index(row, 0);
        const auto staged = model_->index(row, 2).data(metadata_cell_staged_role).toBool();
        changed += staged ? 1 : 0;
        const auto matches = field.data().toString().contains(query, Qt::CaseInsensitive) ||
                             field.data(metadata_field_canonical_name_role)
                                 .toString()
                                 .contains(query, Qt::CaseInsensitive);
        const auto hidden = !matches || (changed_only_->isChecked() && !staged);
        if (fields_->isRowHidden(row) != hidden) {
            fields_->setRowHidden(row, hidden);
        }
        if (hidden) {
            hidden_selection.select(field, model_->index(row, model_->columnCount() - 1));
        } else {
            ++visible;
        }
    }
    // A field that disappears must not remain an invisible Remove/Revert target.
    fields_->selectionModel()->select(hidden_selection, QItemSelectionModel::Deselect);
    const auto current = fields_->currentIndex();
    if (current.isValid() && fields_->isRowHidden(current.row())) {
        fields_->selectionModel()->setCurrentIndex({}, QItemSelectionModel::NoUpdate);
    }
    const auto counts = tr("%1 of %2 fields shown").arg(visible).arg(model_->rowCount());
    status_->setText(ready
                         ? tr("%1 · %2 changed in selected files · Apply includes hidden edits.")
                               .arg(counts)
                               .arg(changed)
                         : tr("%1 · Updating changes… · Apply includes hidden edits.").arg(counts));
}

} // namespace trackknife::bench
