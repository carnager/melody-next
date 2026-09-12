// SPDX-License-Identifier: GPL-3.0-only

#include "bench/replaygain_dialog.hpp"

#include "bench/metadata_dialog_helpers.hpp"
#include "trackknife/metadata/write_plan.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <utility>

namespace trackknife::bench {

ReplayGainDialog::ReplayGainDialog(const std::size_t item_count,
                                   MetadataPropertiesSourceReader source_reader,
                                   MetadataWritePlanApplierFactory plan_applier_factory,
                                   MetadataApplyObserver apply_observer, QWidget* parent)
    : QDialog(parent), item_count_(item_count), source_reader_(std::move(source_reader)),
      plan_applier_factory_(std::move(plan_applier_factory)),
      apply_observer_(std::move(apply_observer)) {
    setWindowTitle(QStringLiteral("ReplayGain %1 track%2")
                       .arg(item_count_)
                       .arg(item_count_ == 1U ? QString{} : QStringLiteral("s")));
    setObjectName(QStringLiteral("bench-replaygain-dialog"));
    setAttribute(Qt::WA_DeleteOnClose);
    setModal(false);

    const QSettings settings;
    auto* layout = new QVBoxLayout(this);
    auto* grouping_row = new QHBoxLayout;
    grouping_row->addWidget(new QLabel(QStringLiteral("Group as:"), this));
    grouping_ = new QComboBox(this);
    grouping_->setObjectName(QStringLiteral("bench-replaygain-dialog-grouping"));
    grouping_->addItem(QStringLiteral("Album by release"));
    grouping_->addItem(QStringLiteral("Album merging discs"));
    grouping_->addItem(QStringLiteral("Selection as one album"));
    grouping_->addItem(QStringLiteral("Tracks only"));
    grouping_->addItem(QStringLiteral("Custom expression"));
    grouping_->setCurrentIndex(settings.value(QStringLiteral("replaygain/grouping"), 0).toInt());
    grouping_row->addWidget(grouping_, 1);
    layout->addLayout(grouping_row);
    expression_ = new QLineEdit(this);
    expression_->setObjectName(QStringLiteral("bench-replaygain-dialog-expression"));
    expression_->setPlaceholderText(QStringLiteral("tkfmt-1, e.g. %album%"));
    expression_->setText(
        settings.value(QStringLiteral("replaygain/grouping-expression")).toString());
    expression_->setVisible(grouping_->currentIndex() == 4);
    layout->addWidget(expression_);
    connect(grouping_, &QComboBox::currentIndexChanged, this,
            [this](const int index) { expression_->setVisible(index == 4); });

    // The same persisted policies Properties exposes (ADR-0146/0148).
    sidecar_only_ = new QCheckBox(QStringLiteral("Store in sidecar only"), this);
    sidecar_only_->setObjectName(QStringLiteral("bench-replaygain-dialog-sidecar-only"));
    sidecar_only_->setChecked(
        settings.value(QStringLiteral("replaygain/sidecar-only"), false).toBool());
    layout->addWidget(sidecar_only_);
    true_peak_ = new QCheckBox(QStringLiteral("True peak as ReplayGain peak"), this);
    true_peak_->setObjectName(QStringLiteral("bench-replaygain-dialog-true-peak"));
    true_peak_->setChecked(settings.value(QStringLiteral("replaygain/true-peak"), false).toBool());
    layout->addWidget(true_peak_);

    status_ = new QLabel(QStringLiteral("Ready. Review-first workflows live in Properties."), this);
    status_->setObjectName(QStringLiteral("bench-replaygain-dialog-status"));
    status_->setWordWrap(true);
    layout->addWidget(status_);
    problems_ = new QPlainTextEdit(this);
    problems_->setObjectName(QStringLiteral("bench-replaygain-dialog-problems"));
    problems_->setReadOnly(true);
    problems_->setMaximumHeight(140);
    problems_->hide();
    layout->addWidget(problems_);

    auto* buttons = new QHBoxLayout;
    run_ = new QPushButton(QStringLiteral("Scan && apply"), this);
    run_->setObjectName(QStringLiteral("bench-replaygain-dialog-run"));
    run_->setDefault(true);
    connect(run_, &QPushButton::clicked, this, &ReplayGainDialog::startRun);
    buttons->addWidget(run_);
    buttons->addStretch();
    auto* close = new QPushButton(QStringLiteral("Close"), this);
    close->setObjectName(QStringLiteral("bench-replaygain-dialog-close"));
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    progress_timer_ = new QTimer(this);
    progress_timer_->setInterval(100);
    connect(progress_timer_, &QTimer::timeout, this, [this] {
        if (completed_) {
            status_->setText(QStringLiteral("Measuring loudness · %1 of %2 files…")
                                 .arg(completed_->load())
                                 .arg(item_count_));
        }
    });
    connect(&capture_watcher_, &QFutureWatcherBase::finished, this,
            &ReplayGainDialog::finishCapture);
    connect(&scan_watcher_, &QFutureWatcherBase::finished, this, &ReplayGainDialog::finishScan);
    connect(&apply_watcher_, &QFutureWatcherBase::finished, this, &ReplayGainDialog::finishApply);
}

ReplayGainDialog::~ReplayGainDialog() {
    cancellation_.request_cancellation();
    capture_watcher_.waitForFinished();
    scan_watcher_.waitForFinished();
    apply_watcher_.waitForFinished();
}

void ReplayGainDialog::showProblems(const QStringList& problems) {
    if (problems.isEmpty()) {
        problems_->hide();
        problems_->clear();
        return;
    }
    problems_->setPlainText(problems.join(QStringLiteral("\n")));
    problems_->show();
}

void ReplayGainDialog::setRunning(const bool running) {
    running_ = running;
    run_->setEnabled(!running);
    grouping_->setEnabled(!running);
    expression_->setEnabled(!running);
    sidecar_only_->setEnabled(!running);
    true_peak_->setEnabled(!running);
    if (!running) {
        progress_timer_->stop();
    }
}

void ReplayGainDialog::startRun() {
    if (running_ || item_count_ == 0U || !source_reader_ || !plan_applier_factory_) {
        return;
    }
    settings_ = {};
    switch (grouping_->currentIndex()) {
    case 0:
        settings_.grouping.mode = loudness::LoudnessGroupingMode::release;
        break;
    case 1:
        settings_.grouping.mode = loudness::LoudnessGroupingMode::release_merged_discs;
        break;
    case 2:
        settings_.grouping.mode = loudness::LoudnessGroupingMode::selection_album;
        break;
    case 3:
        settings_.grouping.mode = loudness::LoudnessGroupingMode::track;
        break;
    default:
        settings_.grouping.mode = loudness::LoudnessGroupingMode::format_expression;
        settings_.grouping.expression = expression_->text().trimmed().toStdString();
        if (settings_.grouping.expression.empty()) {
            status_->setText(QStringLiteral("Enter a tkfmt-1 grouping expression, e.g. %album%"));
            return;
        }
        break;
    }
    settings_.sidecar_only = sidecar_only_->isChecked();
    settings_.true_peak = true_peak_->isChecked();
    QSettings settings;
    settings.setValue(QStringLiteral("replaygain/grouping"), grouping_->currentIndex());
    settings.setValue(QStringLiteral("replaygain/grouping-expression"), expression_->text());
    settings.setValue(QStringLiteral("replaygain/sidecar-only"), settings_.sidecar_only);
    settings.setValue(QStringLiteral("replaygain/true-peak"), settings_.true_peak);

    setRunning(true);
    showProblems({});
    scan_problems_.clear();
    cancellation_ = core::CancellationSource{};
    status_->setText(QStringLiteral("Reading the selection…"));
    capture_watcher_.setFuture(QtConcurrent::run(
        [reader = source_reader_, count = item_count_, token = cancellation_.token()] {
            auto capture = std::make_shared<Capture>();
            std::vector<metadata::StagedMetadataSource> sources;
            sources.reserve(count);
            capture->audio.reserve(count);
            for (std::size_t index = 0U; index < count; ++index) {
                if (token.is_cancellation_requested()) {
                    capture->selection =
                        std::unexpected(core::Error{.code = core::ErrorCode::cancelled,
                                                    .message = "ReplayGain capture cancelled",
                                                    .context = {}});
                    return capture;
                }
                auto source = reader(index);
                if (!source) {
                    continue;
                }
                sources.push_back(std::move(source->source));
                capture->audio.push_back(source->audio);
            }
            capture->selection = metadata::StagedMetadataSelection::create(std::move(sources), {});
            return capture;
        }));
}

void ReplayGainDialog::finishCapture() {
    auto capture = capture_watcher_.result();
    if (!capture || !capture->selection) {
        setRunning(false);
        status_->setText(capture ? display_utf8(capture->selection.error().message)
                                 : QStringLiteral("Capture returned no result"));
        return;
    }
    selection_ =
        std::make_shared<const metadata::StagedMetadataSelection>(std::move(*capture->selection));
    audio_sources_ = std::make_shared<const std::vector<MetadataPropertiesAudioSource>>(
        std::move(capture->audio));
    item_count_ = selection_->item_count();
    if (item_count_ == 0U) {
        setRunning(false);
        status_->setText(QStringLiteral("Nothing to scan."));
        return;
    }
    completed_ = std::make_shared<std::atomic_size_t>(0U);
    progress_timer_->start();
    std::vector<std::size_t> items;
    items.reserve(item_count_);
    for (std::size_t index = 0U; index < item_count_; ++index) {
        items.push_back(index);
    }
    scan_watcher_.setFuture(QtConcurrent::run(
        [selection = selection_, items = std::move(items), audio = audio_sources_,
         settings = settings_, completed = completed_, token = cancellation_.token()] {
            return run_replaygain_scan(selection, metadata::StagedMetadataPatchSet{}, items, audio,
                                       settings, completed, token);
        }));
}

void ReplayGainDialog::finishScan() {
    progress_timer_->stop();
    auto outcome = scan_watcher_.result();
    if (!outcome || !outcome->proposals) {
        setRunning(false);
        status_->setText(outcome ? display_utf8(outcome->proposals.error().message)
                                 : QStringLiteral("The scan returned no result"));
        return;
    }
    for (const auto& problem : outcome->problems) {
        scan_problems_ << QStringLiteral("%1: %2").arg(problem.file, problem.detail);
    }
    status_->setText(QStringLiteral("Writing gains…"));
    apply_watcher_.setFuture(QtConcurrent::run(
        [selection = selection_, proposals = std::move(*outcome->proposals), settings = settings_,
         applier = plan_applier_factory_(), token = cancellation_.token()]() {
            auto apply = std::make_shared<ApplyOutcome>();
            const auto fail = [&apply](core::Error error) {
                apply->result = std::unexpected(std::move(error));
                return apply;
            };
            auto preview = metadata::metadata_proposal_preview(
                *selection, metadata::StagedMetadataPatchSet{}, proposals, 0.0, token);
            if (!preview) {
                return fail(std::move(preview.error()));
            }
            // The measured values become ordinary staged patches on a copy
            // of the selection whose vocabulary grows the loudness fields.
            auto staged_selection = *selection;
            metadata::StagedMetadataPatchSet patches;
            for (const auto& cell : preview->cells) {
                auto field_index = staged_selection.field_index(cell.canonical_field);
                if (!field_index) {
                    auto ensured = staged_selection.ensure_missing_field(cell.canonical_field,
                                                                         cell.display_field);
                    if (!ensured) {
                        return fail(std::move(ensured.error()));
                    }
                    field_index = *ensured;
                }
                const auto staged =
                    cell.after
                        ? patches.replace_values(staged_selection, cell.item_index, *field_index,
                                                 *cell.after)
                        : patches.remove_field(staged_selection, cell.item_index, *field_index);
                if (!staged) {
                    return fail(core::Error{staged.error()});
                }
                ++apply->staged_fields;
            }
            if (apply->staged_fields == 0U) {
                return apply;
            }
            auto plan = metadata::revalidate_metadata_write_plan(
                staged_selection, patches, token,
                metadata::MetadataWritePlanOptions{.sidecar_loudness = settings.sidecar_only,
                                                   .true_peak_loudness = settings.true_peak});
            if (!plan) {
                return fail(std::move(plan.error()));
            }
            if (!plan->ready()) {
                for (const auto& source : plan->sources) {
                    for (const auto& issue : source.issues) {
                        apply->problems << QStringLiteral("%1: %2").arg(
                            display_utf8(core::escape_raw_path(source.raw_path)),
                            display_utf8(issue.error.message));
                    }
                }
                for (const auto& sidecar : plan->sidecars) {
                    for (const auto& issue : sidecar.issues) {
                        apply->problems << QStringLiteral("%1: %2").arg(
                            display_utf8(core::escape_raw_path(sidecar.raw_audio_path)),
                            display_utf8(issue.error.message));
                    }
                }
                apply->result =
                    std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                                .message = "the write plan is blocked",
                                                .context = {}});
                return apply;
            }
            apply->result = applier(*plan, {}, token);
            return apply;
        }));
}

void ReplayGainDialog::finishApply() {
    setRunning(false);
    auto outcome = apply_watcher_.result();
    auto problems = scan_problems_;
    if (outcome) {
        problems << outcome->problems;
    }
    if (!outcome) {
        status_->setText(QStringLiteral("Apply returned no result"));
        showProblems(problems);
        return;
    }
    if (!outcome->result) {
        status_->setText(display_utf8(outcome->result.error().message));
        showProblems(problems);
        return;
    }
    if (outcome->staged_fields == 0U) {
        status_->setText(QStringLiteral("Nothing measurable to write."));
        showProblems(problems);
        return;
    }
    if (apply_observer_) {
        apply_observer_(*outcome->result);
    }
    std::size_t committed = 0U;
    std::size_t failed = 0U;
    const auto& result = *outcome->result;
    for (const auto& source : result.sources) {
        source.commit ? ++committed : ++failed;
    }
    for (const auto& sheet : result.cue_sheets) {
        sheet.commit ? ++committed : ++failed;
    }
    for (const auto& sidecar : result.sidecars) {
        sidecar.commit ? ++committed : ++failed;
    }
    auto text = QStringLiteral("Applied gains to %1 target%2.")
                    .arg(committed)
                    .arg(committed == 1U ? QString{} : QStringLiteral("s"));
    if (failed > 0U) {
        text += QStringLiteral(" %1 failed.").arg(failed);
        for (const auto& source : result.sources) {
            if (!source.commit && source.issue) {
                problems << QStringLiteral("%1: %2").arg(
                    display_utf8(core::escape_raw_path(source.raw_path)),
                    display_utf8(source.issue->message));
            }
        }
    }
    status_->setText(text);
    showProblems(problems);
}

} // namespace trackknife::bench
