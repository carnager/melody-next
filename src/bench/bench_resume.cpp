// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"
#include "bench/settings_dialog.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "uicommon/list_persistence_service.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStatusBar>

namespace trackknife::bench {
namespace {
persistence::ListItem resumeSource(const LocalTrackRow& row) {
    persistence::ListItem source;
    source.source = persistence::ListSource::local;
    source.source_reference = row.raw_path;
    source.source_revision = row.source_revision;
    source.source_selection = persistence::ListItemSourceSelection{row.selection.stream_index,
                                                                   row.selection.subsong_index};
    if (row.segment)
        source.segment =
            persistence::ListItemSegment{row.segment->start_sample, row.segment->end_sample};
    return source;
}
bool resumeEnabled() {
    return QSettings{}.value(QLatin1String(SettingsDialog::restore_playback_key), false).toBool();
}
} // namespace

void BenchMainWindow::checkpointLocalResume(const audio::LocalAuditionSnapshot& snapshot,
                                            bool force) {
    if (!persistence_ || !lists_restored_ || resume_restore_pending_)
        return;
    if (!force && !resumeEnabled())
        return;
    if (!force && (resume_save_pending_ ||
                   (resume_save_clock_.isValid() && resume_save_clock_.elapsed() < 5000)))
        return;
    if (snapshot.state == audio::LocalAuditionState::loading)
        return;
    resume_save_clock_.start();
    // Keep an active request's offset in the same payload as its exact source
    // and pending FIFO, never in the normal-list checkpoint.
    if (local_requests_.active())
        persistUpNext();
    std::optional<ui::LocalResumeCheckpoint> checkpoint;
    const bool resumable = snapshot.state == audio::LocalAuditionState::paused ||
                           snapshot.state == audio::LocalAuditionState::playing ||
                           snapshot.state == audio::LocalAuditionState::buffering ||
                           snapshot.state == audio::LocalAuditionState::draining;
    if (resumeEnabled() && resumable && snapshot.source_revision && snapshot.format &&
        snapshot.format->sample_rate > 0 && snapshot.position_sample >= 0 &&
        (!snapshot.end_sample || snapshot.position_sample < *snapshot.end_sample) &&
        playback_index_.isValid() && !local_requests_.active()) {
        auto* tab = tabForDocument(playback_document_id_);
        if (tab && playback_index_.model() == tab->model) {
            const auto row = playback_index_.row();
            auto source = resumeSource(tab->model->rows().at(static_cast<std::size_t>(row)));
            if (source.source_reference == snapshot.raw_path &&
                source.source_revision == snapshot.source_revision &&
                tab->model->source(row).selection == snapshot.selection &&
                tab->model->source(row).segment == snapshot.segment) {
                const auto rate = snapshot.format->sample_rate;
                const auto position_ms = snapshot.position_sample / rate * 1000 +
                                         snapshot.position_sample % rate * 1000 / rate;
                checkpoint = ui::LocalResumeCheckpoint{playback_document_id_, row,
                                                       std::move(source), position_ms};
            }
        }
    }
    resume_save_pending_ = true;
    persistence_->saveLocalResume(std::move(checkpoint), [this](QString error) {
        resume_save_pending_ = false;
        if (!error.isEmpty())
            statusBar()->showMessage(tr("Playback resume save failed: %1").arg(error), 7000);
    });
}

void BenchMainWindow::restoreLocalResume() {
    if (!persistence_ || !player_ || !resumeEnabled() ||
        player_->snapshot().state != audio::LocalAuditionState::empty) {
        resume_restore_pending_ = false;
        return;
    }
    resume_restore_pending_ = true;
    const auto generation = resume_intent_generation_;
    persistence_->loadUiState(QStringLiteral("playback/local-resume-v1"), [this, generation](
                                                                              QByteArray bytes,
                                                                              QString error) {
        const auto finish = [this](QString problem = {}) {
            resume_restore_pending_ = false;
            if (!problem.isEmpty())
                statusBar()->showMessage(tr("Playback was not restored: %1").arg(problem), 7000);
        };
        if (!player_ || generation != resume_intent_generation_ || !resumeEnabled()) {
            finish();
            return;
        }
        if (!error.isEmpty() || bytes.isEmpty()) {
            finish(error);
            return;
        }
        if (bytes.size() > 4096) {
            finish(tr("invalid saved state"));
            return;
        }
        const auto record = QJsonDocument::fromJson(bytes).object();
        bool valid_position = false;
        const auto position =
            record.value(QStringLiteral("position_ms")).toString().toLongLong(&valid_position);
        const auto row = record.value(QStringLiteral("row")).toInt(-1);
        const auto id = record.value(QStringLiteral("list")).toString();
        const auto key = record.value(QStringLiteral("source")).toString();
        auto* tab = tabForDocument(id);
        if (record.value(QStringLiteral("version")).toInt() != 1 || !valid_position ||
            position < 0 || position > 365LL * 24 * 60 * 60 * 1000 || row < 0 || !tab ||
            row >= tab->model->rowCount() || key.size() != 64) {
            finish(tr("saved track or position is no longer available"));
            return;
        }
        const auto source = resumeSource(tab->model->rows().at(static_cast<std::size_t>(row)));
        const QPersistentModelIndex index = tab->model->index(row, 0);
        persistence_->verifyLocalResumeSource(
            source, key, [this, generation, id, index, source, position, finish](QString problem) {
                auto* restored_tab = tabForDocument(id);
                if (!player_ || generation != resume_intent_generation_ || !resumeEnabled() ||
                    !restored_tab || !index.isValid() ||
                    player_->snapshot().state != audio::LocalAuditionState::empty) {
                    finish();
                    return;
                }
                if (!problem.isEmpty()) {
                    finish(problem);
                    return;
                }
                if (resumeSource(restored_tab->model->rows().at(
                        static_cast<std::size_t>(index.row()))) != source) {
                    finish(tr("the source changed during restore"));
                    return;
                }
                playRow(*restored_tab, index.row(), position);
                finish();
            });
    });
}
} // namespace trackknife::bench
