// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/replaygain_job.hpp"

#include "bench/metadata_dialog_helpers.hpp"
#include "trackknife/loudness/replaygain.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/write_plan.hpp"

#include <QSettings>
#include <QtConcurrent/QtConcurrentRun>

#include <map>
#include <utility>

namespace trackknife::bench {
namespace {

// "1 track", "2 tracks".
[[nodiscard]] QString tracks(const long long count) {
    return count == 1 ? QStringLiteral("1 track") : QStringLiteral("%1 tracks").arg(count);
}

} // namespace

ReplayGainJob::ReplayGainJob(const std::size_t item_count,
                             MetadataPropertiesSourceReader source_reader,
                             MetadataWritePlanApplierFactory plan_applier_factory,
                             MetadataApplyObserver apply_observer, FileWorkTools tools,
                             QObject* parent)
    : QObject(parent), item_count_(item_count), source_reader_(std::move(source_reader)),
      plan_applier_factory_(std::move(plan_applier_factory)),
      apply_observer_(std::move(apply_observer)), tools_(std::move(tools)),
      groups_{QStringLiteral("Preview groups before scanning to check album boundaries.")} {
    // The same persisted policies the tag editor exposes (ADR-0146/0148).
    const QSettings settings;
    grouping_ = settings.value(QStringLiteral("replaygain/grouping"), 0).toInt();
    expression_ = settings.value(QStringLiteral("replaygain/grouping-expression")).toString();
    sidecar_only_ = settings.value(QStringLiteral("replaygain/sidecar-only"), false).toBool();
    true_peak_ = settings.value(QStringLiteral("replaygain/true-peak"), false).toBool();
    status_ = QStringLiteral("%1 selected. Scan writes tags when measurement finishes.")
                  .arg(tracks(static_cast<long long>(item_count_)));
    progress_timer_.setInterval(100);
    connect(&progress_timer_, &QTimer::timeout, this, [this] {
        if (completed_) {
            progress_value_ = static_cast<int>(completed_->load());
            status_ = QStringLiteral("Measuring loudness · %1 of %2 files…")
                          .arg(completed_->load())
                          .arg(item_count_);
            emit changed();
        }
    });
    connect(&capture_watcher_, &QFutureWatcherBase::finished, this, &ReplayGainJob::finishCapture);
    connect(&scan_watcher_, &QFutureWatcherBase::finished, this, &ReplayGainJob::finishScan);
    connect(&apply_watcher_, &QFutureWatcherBase::finished, this, &ReplayGainJob::finishApply);
}

ReplayGainJob::~ReplayGainJob() {
    cancellation_.request_cancellation();
    capture_watcher_.waitForFinished();
    scan_watcher_.waitForFinished();
    apply_watcher_.waitForFinished();
}

QStringList ReplayGainJob::groupings() {
    return {QStringLiteral("Albums by release tags"),
            QStringLiteral("Albums — combine disc editions"),
            QStringLiteral("Selection as one album"), QStringLiteral("Track gain only"),
            QStringLiteral("Custom album grouping…")};
}

QString ReplayGainJob::title() const {
    return QStringLiteral("ReplayGain %1 track%2")
        .arg(item_count_)
        .arg(item_count_ == 1U ? QString{} : QStringLiteral("s"));
}

void ReplayGainJob::groupsChanged() {
    groups_ =
        QStringList{QStringLiteral("Grouping changed — preview again to check album boundaries.")};
    emit changed();
}

void ReplayGainJob::setGrouping(const int index) {
    if (grouping_ != index) {
        grouping_ = index;
        groupsChanged();
    }
}

void ReplayGainJob::setExpression(const QString& expression) {
    if (expression_ != expression) {
        expression_ = expression;
        groupsChanged();
    }
}

void ReplayGainJob::setSidecarOnly(const bool on) {
    sidecar_only_ = on;
    emit changed();
}

void ReplayGainJob::setTruePeak(const bool on) {
    true_peak_ = on;
    emit changed();
}

void ReplayGainJob::setStatusText(const QString& text) {
    status_ = text;
    emit changed();
}

void ReplayGainJob::setRunning(const bool running) {
    running_ = running;
    stopping_ = false;
    if (!running) {
        progress_timer_.stop();
    }
    emit changed();
}

void ReplayGainJob::preview() {
    if (running_) {
        return;
    }
    preview_only_ = true;
    startCapture();
}

void ReplayGainJob::run() {
    if (running_) {
        return;
    }
    preview_only_ = false;
    startCapture();
}

void ReplayGainJob::stop() {
    if (!running_) {
        return;
    }
    cancellation_.request_cancellation();
    stopping_ = true;
    progress_timer_.stop();
    setStatusText(QStringLiteral("Stopping… Any completed tag writes are retained."));
}

void ReplayGainJob::startCapture() {
    if (running_ || item_count_ == 0U || !source_reader_ || !plan_applier_factory_) {
        return;
    }
    settings_ = {};
    switch (grouping_) {
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
        settings_.grouping.expression = expression_.trimmed().toStdString();
        if (settings_.grouping.expression.empty()) {
            status_ = QStringLiteral("Enter a tkfmt-1 grouping expression, e.g. %album%");
            emit changed();
            return;
        }
        break;
    }
    settings_.sidecar_only = sidecar_only_;
    settings_.true_peak = true_peak_;
    QSettings settings;
    settings.setValue(QStringLiteral("replaygain/grouping"), grouping_);
    settings.setValue(QStringLiteral("replaygain/grouping-expression"), expression_);
    settings.setValue(QStringLiteral("replaygain/sidecar-only"), settings_.sidecar_only);
    settings.setValue(QStringLiteral("replaygain/true-peak"), settings_.true_peak);

    problems_.clear();
    scan_problems_.clear();
    cancellation_ = core::CancellationSource{};
    status_ = QStringLiteral("Reading the selection…");
    progress_maximum_ = 0;
    progress_value_ = 0;
    setRunning(true);
    capture_watcher_.setFuture(QtConcurrent::run([reader = source_reader_, count = item_count_,
                                                  grouping = settings_.grouping,
                                                  access = tools_.access,
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
        auto prepared =
            metadata::capture_uncached_metadata_sources(std::move(sources), access, token);
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
            capture->groups << QStringLiteral("%1 · %2").arg(group.first, tracks(group.second));
        }
        if (groups.size() > 200)
            capture->groups << QStringLiteral("… %1 more album groups").arg(groups.size() - 200);
        if (track_only)
            capture->groups << QStringLiteral("%1 without album grouping — track gain only")
                                   .arg(tracks(static_cast<long long>(track_only)));
        return capture;
    }));
}

void ReplayGainJob::finishCapture() {
    auto capture = capture_watcher_.result();
    if (!capture || !capture->selection) {
        setRunning(false);
        setStatusText(capture ? display_utf8(capture->selection.error().message)
                              : QStringLiteral("Capture returned no result"));
        return;
    }
    selection_ =
        std::make_shared<const metadata::StagedMetadataSelection>(std::move(*capture->selection));
    audio_sources_ = std::make_shared<const std::vector<MetadataPropertiesAudioSource>>(
        std::move(capture->audio));
    item_count_ = selection_->item_count();
    groups_ = capture->groups;
    if (item_count_ == 0U) {
        setRunning(false);
        setStatusText(QStringLiteral("Nothing to scan."));
        return;
    }
    if (preview_only_ || cancellation_.is_cancellation_requested()) {
        setRunning(false);
        setStatusText(cancellation_.is_cancellation_requested()
                          ? QStringLiteral("Stopped. No tags written.")
                          : QStringLiteral("%1 ready. Scan and write tags to measure "
                                           "loudness and save the results.")
                                .arg(tracks(static_cast<long long>(item_count_))));
        return;
    }
    progress_maximum_ = static_cast<int>(item_count_);
    progress_value_ = 0;
    completed_ = std::make_shared<std::atomic_size_t>(0U);
    progress_timer_.start();
    emit changed();
    std::vector<std::size_t> items;
    items.reserve(item_count_);
    for (std::size_t index = 0U; index < item_count_; ++index) {
        items.push_back(index);
    }
    scan_watcher_.setFuture(
        QtConcurrent::run([selection = selection_, items = std::move(items), audio = audio_sources_,
                           settings = settings_, completed = completed_, scanner = tools_.scanner,
                           token = cancellation_.token()] {
            return run_replaygain_scan(selection, metadata::StagedMetadataPatchSet{}, items, audio,
                                       settings, completed, token, scanner);
        }));
}

void ReplayGainJob::finishScan() {
    progress_timer_.stop();
    auto outcome = scan_watcher_.result();
    if (cancellation_.is_cancellation_requested()) {
        setRunning(false);
        setStatusText(QStringLiteral("Stopped. No tags written."));
        return;
    }
    if (!outcome || !outcome->proposals) {
        setRunning(false);
        setStatusText(outcome ? display_utf8(outcome->proposals.error().message)
                              : QStringLiteral("The scan returned no result"));
        return;
    }
    for (const auto& problem : outcome->problems) {
        scan_problems_ << QStringLiteral("%1: %2").arg(problem.file, problem.detail);
    }
    progress_maximum_ = 0;
    setStatusText(QStringLiteral("Writing ReplayGain tags… Audio samples are not changed."));
    apply_watcher_.setFuture(
        QtConcurrent::run([selection = selection_, proposals = std::move(*outcome->proposals),
                           settings = settings_, applier = plan_applier_factory_(),
                           access = tools_.access, token = cancellation_.token()]() {
            auto apply = std::make_shared<ApplyOutcome>();
            const auto fail = [&apply](core::Error error) {
                apply->result = std::unexpected(std::move(error));
                return apply;
            };
            // The measured values become ordinary staged patches on a copy
            // of the selection whose vocabulary grows the loudness fields.
            auto staged = loudness::stage_replaygain(*selection, proposals, token);
            if (!staged) {
                return fail(std::move(staged.error()));
            }
            auto& staged_selection = staged->selection;
            auto& patches = staged->patches;
            apply->staged_fields = staged->staged_fields;
            if (apply->staged_fields == 0U) {
                return apply;
            }
            auto plan = metadata::build_metadata_write_plan(
                staged_selection, patches, access, token,
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

void ReplayGainJob::finishApply() {
    setRunning(false);
    auto outcome = apply_watcher_.result();
    auto problems = scan_problems_;
    if (outcome) {
        problems << outcome->problems;
    }
    if (!outcome) {
        setStatusText(QStringLiteral("Apply returned no result"));
        problems_ = problems;
        emit changed();
        return;
    }
    if (!outcome->result) {
        setStatusText(display_utf8(outcome->result.error().message));
        problems_ = problems;
        emit changed();
        return;
    }
    if (outcome->staged_fields == 0U) {
        setStatusText(QStringLiteral("Nothing measurable to write."));
        problems_ = problems;
        emit changed();
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
    setStatusText(text);
    problems_ = problems;
    emit changed();
}

} // namespace trackknife::bench
