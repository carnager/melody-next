// SPDX-License-Identifier: GPL-3.0-only

#include "bench/replaygain_dialog.hpp"

#include "bench/metadata_dialog_helpers.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/write_plan.hpp"

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
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <map>
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
    resize(640, 500);

    const QSettings settings;
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
    grouping_->addItem(QStringLiteral("Albums by release tags"));
    grouping_->addItem(QStringLiteral("Albums — combine disc editions"));
    grouping_->addItem(QStringLiteral("Selection as one album"));
    grouping_->addItem(QStringLiteral("Track gain only"));
    grouping_->addItem(QStringLiteral("Custom album grouping…"));
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
    groups_->addItem(QStringLiteral("Preview groups before scanning to check album boundaries."));
    layout->addWidget(groups_, 1);
    connect(preview_, &QPushButton::clicked, this, [this] {
        if (running_)
            return;
        preview_only_ = true;
        startCapture();
    });
    const auto invalidate_groups = [this] {
        groups_->clear();
        groups_->addItem(
            QStringLiteral("Grouping changed — preview again to check album boundaries."));
    };
    connect(grouping_, &QComboBox::currentIndexChanged, this, invalidate_groups);
    connect(expression_, &QLineEdit::textChanged, this, invalidate_groups);

    auto* storage = new QGroupBox(QStringLiteral("Storage and peak measurement"), this);
    auto* storage_layout = new QVBoxLayout(storage);

    // The same persisted policies Properties exposes (ADR-0146/0148).
    sidecar_only_ =
        new QCheckBox(QStringLiteral("Keep audio files untouched — store in sidecar files"), this);
    sidecar_only_->setObjectName(QStringLiteral("bench-replaygain-dialog-sidecar-only"));
    sidecar_only_->setChecked(
        settings.value(QStringLiteral("replaygain/sidecar-only"), false).toBool());
    sidecar_only_->setToolTip(
        QStringLiteral("Otherwise write tags where safely supported, with sidecar fallback. CUE "
                       "tracks store gains in their CUE sheet."));
    storage_layout->addWidget(sidecar_only_);
    true_peak_ = new QCheckBox(QStringLiteral("Use true peak for clipping protection"), this);
    true_peak_->setToolTip(QStringLiteral("Uses inter-sample peak estimates instead of sample "
                                          "peaks for the saved ReplayGain peak values."));
    true_peak_->setObjectName(QStringLiteral("bench-replaygain-dialog-true-peak"));
    true_peak_->setChecked(settings.value(QStringLiteral("replaygain/true-peak"), false).toBool());
    storage_layout->addWidget(true_peak_);
    layout->addWidget(storage);

    status_ =
        new QLabel(QStringLiteral("%1 tracks selected. Scan writes tags when measurement finishes.")
                       .arg(item_count_),
                   this);
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
    connect(run_, &QPushButton::clicked, this, &ReplayGainDialog::startRun);
    buttons->addWidget(run_);
    stop_ = new QPushButton(QStringLiteral("Stop"), this);
    stop_->setObjectName(QStringLiteral("bench-replaygain-dialog-stop"));
    stop_->hide();
    connect(stop_, &QPushButton::clicked, this, [this] {
        cancellation_.request_cancellation();
        stop_->setEnabled(false);
        progress_timer_->stop();
        status_->setText(QStringLiteral("Stopping… Any completed tag writes are retained."));
    });
    buttons->addWidget(stop_);
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
            progress_->setValue(static_cast<int>(completed_->load()));
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
    preview_->setEnabled(!running);
    stop_->setVisible(running);
    stop_->setEnabled(running);
    progress_->setVisible(running);
    grouping_->setEnabled(!running);
    expression_->setEnabled(!running);
    sidecar_only_->setEnabled(!running);
    true_peak_->setEnabled(!running);
    if (!running) {
        progress_timer_->stop();
    }
}

void ReplayGainDialog::startRun() {
    if (running_)
        return;
    preview_only_ = false;
    startCapture();
}

void ReplayGainDialog::startCapture() {
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
    progress_->setRange(0, 0);
    capture_watcher_.setFuture(QtConcurrent::run([reader = source_reader_, count = item_count_,
                                                  grouping = settings_.grouping,
                                                  token = cancellation_.token()] {
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
        auto prepared = metadata::capture_uncached_metadata_sources(std::move(sources), token);
        if (!prepared) {
            capture->selection = std::unexpected(prepared.error());
            return capture;
        }
        capture->selection = metadata::StagedMetadataSelection::create(std::move(*prepared), {});
        if (!capture->selection)
            return capture;
        std::vector<const metadata::MetadataDocument*> documents;
        for (std::size_t i = 0; i < capture->selection->item_count(); ++i)
            documents.push_back(&capture->selection->source(i).baseline);
        const auto keys = loudness::assign_loudness_groups(grouping, documents, token);
        if (!keys) {
            capture->selection = std::unexpected(keys.error());
            return capture;
        }
        std::map<std::string, std::pair<QString, int>> groups;
        int track_only = 0;
        for (std::size_t i = 0; i < keys->size(); ++i) {
            if (!(*keys)[i]) {
                ++track_only;
                continue;
            }
            auto& group = groups[*(*keys)[i]];
            if (group.second++ == 0) {
                const auto& doc = *documents[i];
                group.first =
                    grouping.mode == loudness::LoudnessGroupingMode::selection_album
                        ? QStringLiteral("Selection as one album")
                        : display_utf8(doc.first_effective_value("albumartist")
                                           .value_or(doc.first_effective_value("artist").value_or(
                                               "Unknown artist"))) +
                              QStringLiteral(" — ") +
                              display_utf8(
                                  doc.first_effective_value("album").value_or("Untitled album"));
            }
        }
        for (const auto& [key, group] : groups) {
            (void)key;
            if (capture->groups.size() >= 200)
                break;
            capture->groups << QStringLiteral("%1 · %2 tracks").arg(group.first).arg(group.second);
        }
        if (groups.size() > 200)
            capture->groups << QStringLiteral("… %1 more album groups").arg(groups.size() - 200);
        if (track_only)
            capture->groups << QStringLiteral("%1 tracks without album grouping — track gain only")
                                   .arg(track_only);
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
    groups_->clear();
    groups_->addItems(capture->groups);
    if (item_count_ == 0U) {
        setRunning(false);
        status_->setText(QStringLiteral("Nothing to scan."));
        return;
    }
    if (preview_only_ || cancellation_.is_cancellation_requested()) {
        setRunning(false);
        status_->setText(cancellation_.is_cancellation_requested()
                             ? QStringLiteral("Stopped. No tags written.")
                             : QStringLiteral("%1 tracks ready. Scan and write tags to measure "
                                              "loudness and save the results.")
                                   .arg(item_count_));
        return;
    }
    progress_->setRange(0, static_cast<int>(item_count_));
    progress_->setValue(0);
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
    if (cancellation_.is_cancellation_requested()) {
        setRunning(false);
        status_->setText(QStringLiteral("Stopped. No tags written."));
        return;
    }
    if (!outcome || !outcome->proposals) {
        setRunning(false);
        status_->setText(outcome ? display_utf8(outcome->proposals.error().message)
                                 : QStringLiteral("The scan returned no result"));
        return;
    }
    for (const auto& problem : outcome->problems) {
        scan_problems_ << QStringLiteral("%1: %2").arg(problem.file, problem.detail);
    }
    status_->setText(QStringLiteral("Writing ReplayGain tags… Audio samples are not changed."));
    progress_->setRange(0, 0);
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
                            display_utf8(core::display_raw_path(source.raw_path)),
                            display_utf8(issue.error.message));
                    }
                }
                for (const auto& sidecar : plan->sidecars) {
                    for (const auto& issue : sidecar.issues) {
                        apply->problems << QStringLiteral("%1: %2").arg(
                            display_utf8(core::display_raw_path(sidecar.raw_audio_path)),
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
    auto text = QStringLiteral("Saved ReplayGain tags to %1 target%2. Audio samples unchanged.")
                    .arg(committed)
                    .arg(committed == 1U ? QString{} : QStringLiteral("s"));
    if (failed > 0U) {
        text += QStringLiteral(" %1 failed.").arg(failed);
        for (const auto& source : result.sources) {
            if (!source.commit && source.issue) {
                problems << QStringLiteral("%1: %2").arg(
                    display_utf8(core::display_raw_path(source.raw_path)),
                    display_utf8(source.issue->message));
            }
        }
    }
    status_->setText(text);
    showProblems(problems);
}

} // namespace trackknife::bench
