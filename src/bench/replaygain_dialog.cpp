// SPDX-License-Identifier: GPL-3.0-only

#include "bench/replaygain_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <utility>

namespace trackknife::bench {

ReplayGainDialog::ReplayGainDialog(const std::size_t item_count,
                                   MetadataPropertiesSourceReader source_reader,
                                   MetadataWritePlanApplierFactory plan_applier_factory,
                                   MetadataApplyObserver apply_observer, QWidget* parent,
                                   FileWorkTools tools)
    : QDialog(parent),
      job_(new ReplayGainJob(item_count, std::move(source_reader), std::move(plan_applier_factory),
                             std::move(apply_observer), std::move(tools), this)) {
    setWindowTitle(job_->title());
    setObjectName(QStringLiteral("bench-replaygain-dialog"));
    setAttribute(Qt::WA_DeleteOnClose);
    setModal(false);
    resize(640, 500);

    auto* layout = new QVBoxLayout(this);
    auto* explanation =
        new QLabel(QStringLiteral("<b>Calculate ReplayGain tags</b><br>"
                                  "Measure loudness and save track and album volume adjustments. "
                                  "Audio samples are not changed."),
                   this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* grouping_row = new QHBoxLayout;
    grouping_row->addWidget(new QLabel(QStringLiteral("Scan mode:"), this));
    grouping_ = new QComboBox(this);
    grouping_->setObjectName(QStringLiteral("bench-replaygain-dialog-grouping"));
    grouping_->addItems(ReplayGainJob::groupings());
    grouping_row->addWidget(grouping_, 1);
    layout->addLayout(grouping_row);
    expression_ = new QLineEdit(job_->expression(), this);
    expression_->setObjectName(QStringLiteral("bench-replaygain-dialog-expression"));
    expression_->setPlaceholderText(QStringLiteral("tkfmt-1, e.g. %album%"));
    layout->addWidget(expression_);

    auto* group_header = new QHBoxLayout;
    group_header->addWidget(new QLabel(QStringLiteral("Album groups in the selected files"), this));
    group_header->addStretch();
    preview_ = new QPushButton(QStringLiteral("Preview groups"), this);
    preview_->setObjectName(QStringLiteral("bench-replaygain-dialog-preview"));
    preview_->setAutoDefault(false);
    group_header->addWidget(preview_);
    layout->addLayout(group_header);
    groups_ = new QListWidget(this);
    groups_->setObjectName(QStringLiteral("bench-replaygain-dialog-groups"));
    groups_->setUniformItemSizes(true);
    groups_->setSelectionMode(QAbstractItemView::NoSelection);
    layout->addWidget(groups_, 1);

    auto* storage = new QGroupBox(QStringLiteral("Storage and peak measurement"), this);
    auto* storage_layout = new QVBoxLayout(storage);
    sidecar_only_ =
        new QCheckBox(QStringLiteral("Keep audio files untouched — store in sidecar files"), this);
    sidecar_only_->setObjectName(QStringLiteral("bench-replaygain-dialog-sidecar-only"));
    sidecar_only_->setToolTip(
        QStringLiteral("Otherwise write tags where safely supported, with sidecar fallback. CUE "
                       "tracks store gains in their CUE sheet."));
    storage_layout->addWidget(sidecar_only_);
    true_peak_ = new QCheckBox(QStringLiteral("Use true peak for clipping protection"), this);
    true_peak_->setToolTip(QStringLiteral("Uses inter-sample peak estimates instead of sample "
                                          "peaks for the saved ReplayGain peak values."));
    true_peak_->setObjectName(QStringLiteral("bench-replaygain-dialog-true-peak"));
    storage_layout->addWidget(true_peak_);
    layout->addWidget(storage);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("bench-replaygain-dialog-status"));
    status_->setWordWrap(true);
    layout->addWidget(status_);
    progress_ = new QProgressBar(this);
    progress_->setObjectName(QStringLiteral("bench-replaygain-dialog-progress"));
    progress_->hide();
    layout->addWidget(progress_);
    problems_ = new QPlainTextEdit(this);
    problems_->setObjectName(QStringLiteral("bench-replaygain-dialog-problems"));
    problems_->setReadOnly(true);
    problems_->setMaximumHeight(140);
    problems_->hide();
    layout->addWidget(problems_);

    auto* buttons = new QHBoxLayout;
    run_ = new QPushButton(QStringLiteral("Scan and write tags"), this);
    run_->setObjectName(QStringLiteral("bench-replaygain-dialog-run"));
    run_->setDefault(true);
    buttons->addWidget(run_);
    stop_ = new QPushButton(QStringLiteral("Stop"), this);
    stop_->setObjectName(QStringLiteral("bench-replaygain-dialog-stop"));
    stop_->hide();
    buttons->addWidget(stop_);
    buttons->addStretch();
    auto* close = new QPushButton(QStringLiteral("Close"), this);
    close->setObjectName(QStringLiteral("bench-replaygain-dialog-close"));
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(grouping_, &QComboBox::currentIndexChanged, job_, &ReplayGainJob::setGrouping);
    connect(expression_, &QLineEdit::textChanged, job_, &ReplayGainJob::setExpression);
    connect(sidecar_only_, &QCheckBox::toggled, job_, &ReplayGainJob::setSidecarOnly);
    connect(true_peak_, &QCheckBox::toggled, job_, &ReplayGainJob::setTruePeak);
    connect(preview_, &QPushButton::clicked, job_, &ReplayGainJob::preview);
    connect(run_, &QPushButton::clicked, job_, &ReplayGainJob::run);
    connect(stop_, &QPushButton::clicked, job_, &ReplayGainJob::stop);
    connect(job_, &ReplayGainJob::changed, this, &ReplayGainDialog::sync);
    sync();
}

void ReplayGainDialog::sync() {
    const auto& job = *job_;
    {
        const QSignalBlocker grouping{grouping_};
        const QSignalBlocker sidecar{sidecar_only_};
        const QSignalBlocker peak{true_peak_};
        grouping_->setCurrentIndex(job.grouping());
        sidecar_only_->setChecked(job.sidecarOnly());
        true_peak_->setChecked(job.truePeak());
    }
    expression_->setVisible(job.grouping() == 4);
    if (groups_->count() != job.groups().size() ||
        (groups_->count() > 0 && groups_->item(0)->text() != job.groups().front())) {
        groups_->clear();
        groups_->addItems(job.groups());
    }
    status_->setText(job.status());
    const auto running = job.running();
    run_->setEnabled(!running);
    preview_->setEnabled(!running);
    stop_->setVisible(running);
    stop_->setEnabled(job.canStop());
    progress_->setVisible(running);
    progress_->setRange(0, job.progressMaximum());
    progress_->setValue(job.progressValue());
    grouping_->setEnabled(!running);
    expression_->setEnabled(!running);
    sidecar_only_->setEnabled(!running);
    true_peak_->setEnabled(!running);
    problems_->setVisible(!job.problems().isEmpty());
    problems_->setPlainText(job.problems().join(QStringLiteral("\n")));
}

} // namespace trackknife::bench
