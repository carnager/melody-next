// SPDX-License-Identifier: GPL-3.0-only

#include "quick/tag_editor.hpp"

#include "quick/engine_session.hpp"
#include "trackknife/protocol/message.hpp"

#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QVariantMap>

#include <array>
#include <atomic>
#include <string_view>

namespace trackknife::quick {

namespace {

// What the widgets tagger shows first, whether any file has it or not.
constexpr std::array<std::string_view, 12> preferred_fields{
    "Title",        "Artist",      "Album Artist", "Album", "Date",     "Track Number",
    "Total Tracks", "Disc Number", "Total Discs",  "Genre", "Composer", "Comment",
};

const QString separator = QStringLiteral("; ");

[[nodiscard]] QString joined(const std::vector<std::string>& values) {
    QStringList parts;
    for (const auto& value : values) {
        parts.push_back(QString::fromStdString(protocol::displayable_text(value)));
    }
    return parts.join(separator);
}

[[nodiscard]] std::vector<std::string> split(const QString& text) {
    std::vector<std::string> values;
    for (const auto& part : text.split(separator)) {
        values.push_back(part.toStdString());
    }
    return values;
}

[[nodiscard]] QString stateName(const metadata::MetadataSelectionFieldState state) {
    return QString::fromUtf8(metadata::metadata_selection_field_state_name(state));
}

[[nodiscard]] QString fileName(const std::string& raw_path) {
    return QString::fromStdString(protocol::displayable_text(std::filesystem::path{raw_path}.filename().native()));
}

} // namespace

TagEditor::TagEditor(QObject* parent) : QObject(parent) { pool_.setMaxThreadCount(1); }

TagEditor::~TagEditor() {
    cancellation_.request_cancellation();
    pool_.clear();
    pool_.waitForDone();
}

int TagEditor::files() const { return selection_ ? static_cast<int>(selection_->item_count()) : 0; }

void TagEditor::setStage(const QString& stage, const QString& status) {
    stage_ = stage;
    status_ = status;
    emit changed();
}

void TagEditor::open(EngineSession* session, const QStringList& paths) {
    close();
    if (session == nullptr || paths.isEmpty()) {
        return;
    }
    session_ = session;
    work_ = std::make_shared<engine::RemoteFileWork>(session->endpoint());
    cancellation_ = core::CancellationSource{};
    const auto generation = ++generation_;
    setStage(QStringLiteral("reading"), tr("Reading %n file(s)…", nullptr, static_cast<int>(paths.size())));
    std::vector<std::string> raw_paths;
    for (const auto& path : paths) {
        if (const auto raw = protocol::decode_raw_path(path.toStdString())) {
            raw_paths.push_back(*raw);
        }
    }
    const QPointer self{this};
    pool_.start([self, this, generation, work = work_, raw_paths, token = cancellation_.token()] {
        // Read where the files are: the baseline every edit is staged on,
        // and the revision a write is later checked against.
        std::vector<metadata::StagedMetadataSource> sources;
        QString failure;
        const auto access = work->access();
        for (const auto& raw_path : raw_paths) {
            if (token.is_cancellation_requested()) {
                return;
            }
            auto read = access.read(raw_path, token);
            if (!read) {
                failure = tr("%1 could not be read: %2")
                              .arg(fileName(raw_path), QString::fromStdString(read.error().message));
                break;
            }
            sources.push_back(metadata::StagedMetadataSource{.raw_path = raw_path,
                                                             .source_revision = read->source_revision,
                                                             .baseline = std::move(read->document)});
        }
        std::shared_ptr<const metadata::StagedMetadataSelection> selection;
        if (failure.isEmpty()) {
            auto created = metadata::StagedMetadataSelection::create(std::move(sources), preferred_fields);
            if (created) {
                selection = std::make_shared<const metadata::StagedMetadataSelection>(std::move(*created));
            } else {
                failure = QString::fromStdString(created.error().message);
            }
        }
        QMetaObject::invokeMethod(
            self,
            [self, this, generation, selection, failure] {
                if (!self || generation != generation_) {
                    return;
                }
                if (!selection) {
                    setStage(QStringLiteral("idle"), failure);
                    return;
                }
                selection_ = selection;
                patches_ = std::make_shared<metadata::StagedMetadataPatchSet>();
                setStage(QStringLiteral("editing"));
            },
            Qt::QueuedConnection);
    });
}

QVariantList TagEditor::fields() const {
    QVariantList rows;
    if (!selection_ || !patches_) {
        return rows;
    }
    for (std::size_t index = 0; index < selection_->field_count(); ++index) {
        const auto& field = selection_->field(index);
        const auto projection = patches_->project_field(*selection_, index);
        rows.push_back(QVariantMap{
            {QStringLiteral("index"), static_cast<int>(index)},
            {QStringLiteral("name"), QString::fromStdString(field.display_name)},
            {QStringLiteral("state"), stateName(projection.state)},
            {QStringLiteral("value"), joined(projection.common_values)},
            {QStringLiteral("edited"), patches_->field_patch_count(index) > 0},
        });
    }
    return rows;
}

void TagEditor::setField(const int field, const QString& text) {
    if (!selection_ || !patches_ || stage_ != QStringLiteral("editing") || field < 0 ||
        static_cast<std::size_t>(field) >= selection_->field_count()) {
        return;
    }
    const auto index = static_cast<std::size_t>(field);
    for (std::size_t item = 0; item < selection_->item_count(); ++item) {
        const auto staged = text.isEmpty() ? patches_->remove_field(*selection_, item, index)
                                           : patches_->replace_values(*selection_, item, index, split(text));
        if (!staged) {
            setStage(stage_, QString::fromStdString(staged.error().message));
            return;
        }
    }
    setStage(stage_);
}

void TagEditor::revertField(const int field) {
    if (!selection_ || !patches_ || field < 0 || static_cast<std::size_t>(field) >= selection_->field_count()) {
        return;
    }
    for (std::size_t item = 0; item < selection_->item_count(); ++item) {
        static_cast<void>(patches_->revert(*selection_, item, static_cast<std::size_t>(field)));
    }
    setStage(stage_);
}

void TagEditor::addField(const QString& name, const QString& text) {
    if (!selection_) {
        return;
    }
    if (const auto index = selection_->field_index(name.trimmed().toStdString())) {
        setField(static_cast<int>(*index), text);
    } else {
        // A field no file has and no one asked for up front: the selection
        // would need building again with it. Said, rather than dropped.
        setStage(stage_, tr("“%1” is not a field this editor knows; choose one from the list.").arg(name));
    }
}

void TagEditor::preview() {
    if (!selection_ || !patches_ || patches_->empty() || stage_ != QStringLiteral("editing")) {
        return;
    }
    const auto generation = generation_;
    setStage(QStringLiteral("planning"), tr("Checking the files…"));
    const QPointer self{this};
    // Copies: the plan is built on the worker while the editor stays as is.
    pool_.start([self, this, generation, work = work_, selection = selection_,
                 patches = std::make_shared<const metadata::StagedMetadataPatchSet>(*patches_),
                 token = cancellation_.token()] {
        auto built = metadata::build_metadata_write_plan(*selection, *patches, work->access(), token);
        std::shared_ptr<const metadata::MetadataWritePlan> plan;
        QString failure;
        if (built) {
            plan = std::make_shared<const metadata::MetadataWritePlan>(std::move(*built));
        } else {
            failure = QString::fromStdString(built.error().message);
        }
        QMetaObject::invokeMethod(
            self,
            [self, this, generation, plan, failure] {
                if (!self || generation != generation_) {
                    return;
                }
                if (!plan) {
                    setStage(QStringLiteral("editing"), failure);
                    return;
                }
                write_plan_ = plan;
                buildPlanRows();
                setStage(QStringLiteral("reviewing"),
                         plan->ready() ? tr("%n file(s) will be written.", nullptr,
                                            static_cast<int>(plan->ready_source_count()))
                                       : tr("%n problem(s) keep this from being written.", nullptr,
                                            static_cast<int>(plan->blocking_issue_count())));
            },
            Qt::QueuedConnection);
    });
}

void TagEditor::buildPlanRows() {
    plan_rows_.clear();
    if (!write_plan_) {
        return;
    }
    for (const auto& source : write_plan_->sources) {
        QVariantList changes;
        for (const auto& change : source.changes) {
            QString after;
            if (!change.intents.empty()) {
                const auto& intent = change.intents.front();
                after = intent.kind == metadata::StagedMetadataPatchKind::remove_field ? tr("(removed)")
                                                                                        : joined(intent.values);
            }
            changes.push_back(QVariantMap{
                {QStringLiteral("field"), QString::fromStdString(change.display_name)},
                {QStringLiteral("before"), change.original_present ? joined(change.original_values) : QString{}},
                {QStringLiteral("after"), after},
                {QStringLiteral("conflicting"), change.conflicting_intents},
            });
        }
        QStringList issues;
        for (const auto& issue : source.issues) {
            issues.push_back(QString::fromStdString(issue.error.message));
        }
        plan_rows_.push_back(QVariantMap{
            {QStringLiteral("name"), fileName(source.raw_path)},
            {QStringLiteral("changes"), changes},
            {QStringLiteral("issues"), issues},
            {QStringLiteral("ready"), source.ready()},
        });
    }
}

bool TagEditor::planReady() const { return write_plan_ && write_plan_->ready(); }

void TagEditor::back() {
    if (stage_ == QStringLiteral("reviewing")) {
        write_plan_.reset();
        plan_rows_.clear();
        setStage(QStringLiteral("editing"));
    }
}

void TagEditor::apply() {
    if (!planReady() || stage_ != QStringLiteral("reviewing")) {
        return;
    }
    const auto generation = generation_;
    setStage(QStringLiteral("writing"), tr("Writing…"));
    const QPointer self{this};
    const auto total = static_cast<int>(write_plan_->sources.size());
    pool_.start([self, this, generation, total, work = work_, plan = write_plan_, token = cancellation_.token()] {
        // Files finish in any order -- two are written at once -- so what is
        // shown is how many have, not which one reported last.
        auto finished = std::make_shared<std::atomic<int>>(0);
        const auto progress = [self, this, generation, total, finished](const operations::MetadataApplyProgress& at) {
            if (at.state == operations::MetadataApplySourceState::pending ||
                at.state == operations::MetadataApplySourceState::running) {
                return;
            }
            const auto done = ++*finished;
            QMetaObject::invokeMethod(
                self,
                [self, this, generation, done, total] {
                    if (self && generation == generation_ && stage_ == QStringLiteral("writing")) {
                        setStage(stage_, tr("%1 of %2 written…").arg(done).arg(total));
                    }
                },
                Qt::QueuedConnection);
        };
        auto applied = work->apply(*plan, progress, token);
        QMetaObject::invokeMethod(
            self,
            [self, this, generation, applied = std::move(applied)] {
                if (!self || generation != generation_) {
                    return;
                }
                if (!applied) {
                    setStage(QStringLiteral("reviewing"), QString::fromStdString(applied.error().message));
                    return;
                }
                const auto written = applied->committed_source_count();
                const auto failed = applied->failed_source_count();
                QString message = tr("%n file(s) written.", nullptr, static_cast<int>(written));
                if (failed > 0) {
                    message += QLatin1Char(' ') + tr("%n could not be.", nullptr, static_cast<int>(failed));
                    for (const auto& source : applied->sources) {
                        if (source.issue) {
                            message += QStringLiteral("\n") + fileName(source.raw_path) + QStringLiteral(": ") +
                                       QString::fromStdString(source.issue->message);
                        }
                    }
                }
                if (applied->cancellation_requested) {
                    message += QLatin1Char(' ') + tr("Stopped before the rest.");
                }
                auto* session = session_;
                // What was written is the new baseline: editing again starts
                // from the files as they now are.
                selection_.reset();
                patches_.reset();
                write_plan_.reset();
                plan_rows_.clear();
                setStage(QStringLiteral("done"), message);
                if (written > 0) {
                    emit this->written(session);
                }
            },
            Qt::QueuedConnection);
    });
}

void TagEditor::cancel() { cancellation_.request_cancellation(); }

void TagEditor::close() {
    ++generation_;
    cancellation_.request_cancellation();
    session_ = nullptr;
    selection_.reset();
    patches_.reset();
    write_plan_.reset();
    plan_rows_.clear();
    setStage(QStringLiteral("idle"));
}

} // namespace trackknife::quick
