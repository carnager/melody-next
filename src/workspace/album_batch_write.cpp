// SPDX-License-Identifier: GPL-3.0-only
#include "workspace/album_batch_write.hpp"

#include "bench/metadata_dialog_helpers.hpp"
#include "workspace/preparation_planning.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace trackknife::bench {

namespace {

QString issueText(const std::string& message) { return display_utf8(message); }

} // namespace

AlbumBatchWrite::AlbumBatchWrite(TaggerSession& tagger, std::vector<Album> albums, Options options,
                                 QObject* parent)
    : QObject(parent), tagger_(&tagger), albums_(std::move(albums)), options_(std::move(options)),
      progress_(std::make_shared<Progress>()) {
    operations_ = operations::PreparationOperationSelection{
        .save_tags = true,
        .rename_files = options_.rename.has_value(),
        .move_files = options_.move.has_value(),
        .replaygain = false,
    };
    outcomes_.reserve(albums_.size());
    for (const auto& album : albums_) {
        outcomes_.push_back(AlbumOutcome{.album = album.album,
                                         .outcome = Outcome::pending,
                                         .written = 0U,
                                         .files = album.items.size(),
                                         .note = {}});
        files_total_ += album.items.size();
        for (const auto item : album.items) {
            album_of_[item] = album.album;
        }
    }
}

AlbumBatchWrite::~AlbumBatchWrite() { cancellation_.request_cancellation(); }

std::size_t AlbumBatchWrite::filesDone() const {
    return std::min(files_total_, files_before_ + progress_->done.load());
}

AlbumBatchWrite::AlbumOutcome& AlbumBatchWrite::outcomeOf(const std::size_t album) {
    return *std::ranges::find(outcomes_, album, &AlbumOutcome::album);
}

std::vector<std::size_t> AlbumBatchWrite::itemsAt(const std::string& raw_path) const {
    const auto found = items_at_.find(raw_path);
    return found == items_at_.end() ? std::vector<std::size_t>{} : found->second;
}

void AlbumBatchWrite::start() {
    if (running_ || tagger_ == nullptr) {
        return;
    }
    const auto* draft = tagger_->draft();
    const auto selection = tagger_->sharedSelection();
    if (draft == nullptr || selection == nullptr || !tagger_->beginWriteElsewhere()) {
        for (auto& outcome : outcomes_) {
            outcome.outcome = Outcome::failed;
            outcome.note = QStringLiteral("The tagger is busy");
        }
        QMetaObject::invokeMethod(this, &AlbumBatchWrite::finished, Qt::QueuedConnection);
        return;
    }
    running_ = true;
    for (std::size_t item = 0U; item < selection->item_count(); ++item) {
        items_at_[selection->source(item).raw_path].push_back(item);
    }
    // Only tags: an album whose draft is no change has nothing to write.
    std::unordered_set<std::size_t> changed;
    for (const auto& patch : draft->patches()) {
        changed.insert(patch.item_index);
    }
    const auto path_operation = operations_.rename_files || operations_.move_files;
    const auto roots = operations_.move_files && tagger_->services().library_roots
                           ? tagger_->services().library_roots()
                           : std::vector<std::string>{};
    std::vector<std::pair<std::optional<std::string>, std::size_t>> placed;
    for (const auto& album : albums_) {
        auto& outcome = outcomeOf(album.album);
        if (!path_operation &&
            std::ranges::none_of(album.items, [&](auto item) { return changed.contains(item); })) {
            outcome.outcome = Outcome::written;
            outcome.written = album.items.size();
            outcome.note = QStringLiteral("Nothing to change");
            continue;
        }
        std::optional<std::string> root;
        if (operations_.move_files) {
            root = album.items.empty() ? std::nullopt
                                       : libraryFolderOf(selection->source(album.items.front()).raw_path,
                                                        roots);
            if (!root) {
                outcome.outcome = Outcome::left_out;
                outcome.note = QStringLiteral("Not in a library folder, so not moved");
                continue;
            }
        }
        placed.emplace_back(std::move(root), album.album);
    }
    for (auto& [root, album] : placed) {
        auto group = std::ranges::find_if(groups_, [&root](const Group& candidate) {
            return candidate.destination.has_value() == root.has_value() &&
                   (!root || candidate.destination->root_raw_path == *root);
        });
        if (group == groups_.end()) {
            groups_.push_back(Group{
                .destination = root ? std::optional{folderDestination(*root)} : std::nullopt,
                .albums = {}});
            group = std::prev(groups_.end());
        }
        group->albums.push_back(album);
    }
    files_before_ = 0U;
    for (const auto& outcome : outcomes_) {
        if (outcome.outcome != Outcome::pending) {
            files_before_ += outcome.files;
        }
    }
    group_ = 0U;
    planGroup();
}

void AlbumBatchWrite::stop() {
    if (!running_ || stopping_) {
        return;
    }
    stopping_ = true;
    cancellation_.request_cancellation();
}

std::vector<std::size_t> AlbumBatchWrite::itemsOf(const Group& group) const {
    std::vector<std::size_t> items;
    for (const auto album : group.albums) {
        const auto& outcome = *std::ranges::find(outcomes_, album, &AlbumOutcome::album);
        if (outcome.outcome != Outcome::pending) {
            continue;
        }
        const auto& source = *std::ranges::find(albums_, album, &Album::album);
        items.insert(items.end(), source.items.begin(), source.items.end());
    }
    return items;
}

void AlbumBatchWrite::leaveOut(const std::size_t item, const QString& why) {
    const auto album = album_of_.find(item);
    if (album == album_of_.end()) {
        return;
    }
    auto& outcome = outcomeOf(album->second);
    if (outcome.outcome == Outcome::pending) {
        outcome.outcome = Outcome::left_out;
        outcome.note = why;
        files_before_ += outcome.files;
    }
}

void AlbumBatchWrite::failGroup(const QString& why) {
    for (const auto album : groups_[group_].albums) {
        auto& outcome = outcomeOf(album);
        if (outcome.outcome == Outcome::pending) {
            outcome.outcome = Outcome::failed;
            outcome.note = why;
            files_before_ += outcome.files;
        }
    }
    nextGroup();
}

void AlbumBatchWrite::planGroup() {
    if (group_ >= groups_.size() || tagger_ == nullptr) {
        finish();
        return;
    }
    if (stopping_) {
        nextGroup();
        return;
    }
    const auto& group = groups_[group_];
    auto items = itemsOf(group);
    if (items.empty()) {
        nextGroup();
        return;
    }
    const auto selection = tagger_->sharedSelection();
    auto draft = draftOf(*selection, *tagger_->draft(), items);
    if (!draft) {
        failGroup(issueText(draft.error().message));
        return;
    }
    std::optional<operations::OutputLayoutProfile> layout;
    if (options_.move) {
        layout = options_.move;
        if (options_.rename) {
            layout->basename_expression = options_.rename->basename_expression;
        }
    } else if (options_.rename) {
        layout = options_.rename;
    }
    PreparationRequest request{
        .selection = selection,
        .draft = std::move(*draft),
        .items = std::move(items),
        .operations = operations_,
        .layout = std::move(layout),
        .destination = group.destination,
        .options = writePlanOptions(),
        .artwork = {},
        .cover_policy = {},
        .tools = tagger_->services().tools,
    };
    progress_->done = 0U;
    const QPointer self{this};
    (void)QtConcurrent::run([request = std::move(request), token = cancellation_.token(),
                             self]() mutable {
        auto result = std::make_shared<core::Result<operations::PreparationPlan>>(
            planPreparation(std::move(request), token));
        QMetaObject::invokeMethod(
            self, [self, result] { if (self) { self->planned(result); } }, Qt::QueuedConnection);
    });
}

void AlbumBatchWrite::planned(std::shared_ptr<core::Result<operations::PreparationPlan>> result) {
    if (!*result) {
        failGroup(stopping_ ? QStringLiteral("Stopped before it was written")
                            : issueText(result->error().message));
        return;
    }
    auto plan = std::make_shared<const operations::PreparationPlan>(std::move(**result));
    if (!plan->ready()) {
        // Each file that cannot be written as planned leaves its album out;
        // what is the whole write's fails it.
        std::size_t attributed = 0U;
        std::optional<QString> whole;
        const auto per_item = [&](const std::vector<std::size_t>& items, const QString& why) {
            for (const auto item : items) {
                leaveOut(item, why);
                ++attributed;
            }
        };
        for (const auto& issue : plan->issues) {
            if (!issue.blocking) {
                continue;
            }
            if (issue.kind != operations::PreparationPlanIssueKind::combined_source_mismatch ||
                !plan->metadata || !plan->output_paths) {
                whole = whole.value_or(issueText(issue.message));
                continue;
            }
            // The files whose tags and path were planned from other revisions
            // -- one changed between the two looks.
            for (const auto& source : plan->metadata->sources) {
                const auto path = std::ranges::find(plan->output_paths->sources, source.raw_path,
                                                    &operations::PlannedOutputPathSource::source_raw_path);
                if (path == plan->output_paths->sources.end() || !source.expected_revision ||
                    !source.observed_revision ||
                    *source.expected_revision != *source.observed_revision ||
                    path->source_revision != *source.observed_revision) {
                    per_item(source.occurrence_indexes,
                             QStringLiteral("A file changed since it was read"));
                }
            }
        }
        if (plan->metadata) {
            for (const auto& source : plan->metadata->sources) {
                for (const auto& issue : source.issues) {
                    if (!issue.blocking) {
                        continue;
                    }
                    per_item(source.occurrence_indexes,
                             issue.kind == metadata::MetadataWritePlanIssueKind::source_changed
                                 ? QStringLiteral("A file changed since it was read")
                                 : issueText(issue.error.message));
                }
            }
            for (const auto& sheet : plan->metadata->cue_sheets) {
                for (const auto& issue : sheet.issues) {
                    if (issue.blocking) {
                        per_item(issue.item_indexes, issueText(issue.error.message));
                    }
                }
            }
            for (const auto& sidecar : plan->metadata->sidecars) {
                for (const auto& issue : sidecar.issues) {
                    if (issue.blocking) {
                        per_item(itemsAt(sidecar.raw_audio_path), issueText(issue.error.message));
                    }
                }
            }
        }
        if (plan->output_paths) {
            for (const auto& issue : plan->output_paths->issues) {
                if (issue.blocking) {
                    per_item(issue.item_indexes.empty() && issue.source_raw_path
                                 ? itemsAt(*issue.source_raw_path)
                                 : issue.item_indexes,
                             issueText(issue.message));
                }
            }
        }
        if (plan->path_preflight) {
            for (const auto& issue : plan->path_preflight->issues) {
                if (issue.blocking) {
                    per_item(issue.item_indexes.empty() ? itemsAt(issue.source_raw_path)
                                                        : issue.item_indexes,
                             issue.kind == operations::OutputPathPreflightIssueKind::source_changed
                                 ? QStringLiteral("A file changed since it was read")
                                 : issueText(issue.message));
                }
            }
        }
        // Once more without them; a plan still refused fails what is left.
        if (attributed > 0U && !whole && !replanned_) {
            replanned_ = true;
            planGroup();
            return;
        }
        failGroup(whole.value_or(QStringLiteral("The write could not be planned")));
        return;
    }
    if (tagger_ == nullptr) {
        finish();
        return;
    }
    const auto& services = tagger_->services();
    const auto progress = progress_;
    const auto token = cancellation_.token();
    const QPointer self{this};
    if (plan->has_path_operation()) {
        auto applier = services.file_plan_applier_factory ? services.file_plan_applier_factory()
                                                          : FilePublicationPlanApplier{};
        if (!applier) {
            failGroup(QStringLiteral("Moving and renaming are unavailable"));
            return;
        }
        (void)QtConcurrent::run([plan, applier = std::move(applier), progress, token, self] {
            auto outcome = std::make_shared<core::Result<operations::FilePublicationApplyResult>>(
                applier(*plan,
                        [progress](const operations::FilePublicationApplyProgress& update) {
                            progress->done = update.completed_sources;
                        },
                        token));
            QMetaObject::invokeMethod(
                self,
                [self, plan, outcome] {
                    if (self) {
                        self->applied(plan, nullptr, outcome);
                    }
                },
                Qt::QueuedConnection);
        });
    } else {
        auto applier =
            services.plan_applier_factory ? services.plan_applier_factory() : MetadataWritePlanApplier{};
        if (!applier || !plan->metadata) {
            failGroup(QStringLiteral("Saving tags is unavailable"));
            return;
        }
        (void)QtConcurrent::run([plan, applier = std::move(applier), progress, token, self] {
            auto outcome = std::make_shared<core::Result<operations::MetadataApplyResult>>(
                applier(*plan->metadata,
                        [progress](const operations::MetadataApplyProgress& update) {
                            progress->done = update.completed_sources;
                        },
                        token));
            QMetaObject::invokeMethod(
                self,
                [self, plan, outcome] {
                    if (self) {
                        self->applied(plan, outcome, nullptr);
                    }
                },
                Qt::QueuedConnection);
        });
    }
    emit progressed();
}

void AlbumBatchWrite::applied(
    std::shared_ptr<const operations::PreparationPlan> plan,
    std::shared_ptr<core::Result<operations::MetadataApplyResult>> tags,
    std::shared_ptr<core::Result<operations::FilePublicationApplyResult>> files) {
    (void)plan;
    const auto group_error = tags != nullptr && !*tags     ? std::optional{tags->error().message}
                             : files != nullptr && !*files ? std::optional{files->error().message}
                                                           : std::nullopt;
    if (group_error) {
        failGroup(issueText(*group_error));
        return;
    }
    // What each row came to.
    std::unordered_map<std::size_t, QString> failed;
    std::unordered_set<std::size_t> written;
    if (tags != nullptr) {
        if (tagger_ != nullptr && tagger_->services().apply_observer) {
            tagger_->services().apply_observer(**tags);
        }
        for (const auto& source : (**tags).sources) {
            for (const auto item : itemsAt(source.raw_path)) {
                if (source.state == operations::MetadataApplySourceState::committed) {
                    written.insert(item);
                    rewritten_.push_back({.item = item, .raw_path = source.raw_path});
                } else {
                    failed.emplace(item, source.issue ? issueText(source.issue->message)
                                                      : QStringLiteral("Not written"));
                }
            }
        }
    } else {
        if (tagger_ != nullptr && tagger_->services().file_apply_observer) {
            tagger_->services().file_apply_observer(**files);
        }
        for (const auto& source : (**files).sources) {
            const auto done =
                source.state == operations::FilePublicationApplySourceState::committed ||
                source.state == operations::FilePublicationApplySourceState::unchanged;
            for (const auto item : itemsAt(source.source_raw_path)) {
                if (done) {
                    written.insert(item);
                    rewritten_.push_back(
                        {.item = item,
                         .raw_path = source.commit ? source.commit->target_raw_path
                                                   : source.source_raw_path});
                } else {
                    failed.emplace(item, source.issue ? issueText(source.issue->message)
                                                      : QStringLiteral("Not written"));
                }
            }
        }
    }
    for (const auto album : groups_[group_].albums) {
        auto& outcome = outcomeOf(album);
        if (outcome.outcome != Outcome::pending) {
            continue;
        }
        const auto& source = *std::ranges::find(albums_, album, &Album::album);
        outcome.written = static_cast<std::size_t>(std::ranges::count_if(
            source.items, [&written](auto item) { return written.contains(item); }));
        for (const auto item : source.items) {
            if (const auto why = failed.find(item); why != failed.end()) {
                outcome.note = why->second;
                break;
            }
        }
        // A row the plan had nothing to do for is as staged already.
        const auto untouched = std::ranges::count_if(source.items, [&](auto item) {
            return !written.contains(item) && !failed.contains(item);
        });
        outcome.written += static_cast<std::size_t>(untouched);
        outcome.outcome = outcome.written == outcome.files ? Outcome::written
                          : outcome.written == 0U
                              ? (stopping_ ? Outcome::stopped : Outcome::failed)
                              : Outcome::partly_written;
        if (outcome.outcome == Outcome::stopped && outcome.note.isEmpty()) {
            outcome.note = QStringLiteral("Stopped before it was written");
        }
        files_before_ += outcome.files;
    }
    nextGroup();
}

void AlbumBatchWrite::nextGroup() {
    progress_->done = 0U;
    replanned_ = false;
    ++group_;
    emit progressed();
    planGroup();
}

void AlbumBatchWrite::finish() {
    for (auto& outcome : outcomes_) {
        if (outcome.outcome == Outcome::pending) {
            outcome.outcome = Outcome::stopped;
        }
        if (outcome.outcome == Outcome::stopped && outcome.note.isEmpty()) {
            outcome.note = QStringLiteral("Stopped before it was written");
        }
    }
    if (tagger_ == nullptr) {
        running_ = false;
        emit finished();
        return;
    }
    connect(tagger_, &TaggerSession::writtenElsewhere, this,
            [this] {
                running_ = false;
                emit finished();
            },
            Qt::SingleShotConnection);
    tagger_->finishWriteElsewhere(std::move(rewritten_));
}

} // namespace trackknife::bench
