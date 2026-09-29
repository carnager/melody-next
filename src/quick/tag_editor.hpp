// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/engine/remote_file_work.hpp"
#include "trackknife/metadata/staged_patch.hpp"
#include "trackknife/metadata/staged_selection.hpp"
#include "trackknife/metadata/write_plan.hpp"

#include <QObject>
#include <QThreadPool>
#include <QVariantList>
#include <QtQmlIntegration>

#include <memory>
#include <optional>

namespace trackknife::quick {

class EngineSession;

// Editing tags, done by the engine that holds the files (ADR-0237), with
// the preview and explicit commit AGENTS.md asks for: the files are read
// there, edits are staged here without touching them, a write plan is
// built and shown -- every change and every reason it cannot be made --
// and only a plan without blockers is written, by the engine, journaled.
//
// The staging, planning and writing are the core's (metadata::Staged*,
// build_metadata_write_plan, RemoteFileWork): the same the widgets tagger
// uses, so both write alike.
class TagEditor final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Reached through Engine.tagEditor")

    // "idle", "reading", "editing", "planning", "reviewing", "writing".
    Q_PROPERTY(QString stage READ stage NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(int files READ files NOTIFY changed)
    // [{index, name, state, value, edited}] -- state is common, mixed,
    // partial or missing; value is the common value, several joined by "; ".
    Q_PROPERTY(QVariantList fields READ fields NOTIFY changed)
    // The plan: [{name, changes: [{field, before, after}], issues: [text],
    // ready}], and whether it can be written as it is.
    Q_PROPERTY(QVariantList plan READ plan NOTIFY changed)
    Q_PROPERTY(bool planReady READ planReady NOTIFY changed)
    Q_PROPERTY(bool edited READ edited NOTIFY changed)

  public:
    explicit TagEditor(QObject* parent);
    TagEditor(const TagEditor&) = delete;
    TagEditor(TagEditor&&) = delete;
    TagEditor& operator=(const TagEditor&) = delete;
    TagEditor& operator=(TagEditor&&) = delete;
    ~TagEditor() override;

    [[nodiscard]] QString stage() const { return stage_; }
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] int files() const;
    [[nodiscard]] QVariantList fields() const;
    [[nodiscard]] QVariantList plan() const { return plan_rows_; }
    [[nodiscard]] bool planReady() const;
    [[nodiscard]] bool edited() const { return patches_ && !patches_->empty(); }

    // Opens the files -- encoded paths on `session`'s engine -- for editing.
    Q_INVOKABLE void open(trackknife::quick::EngineSession* session, const QStringList& paths);
    // A field for every file: "; " separates values, and nothing removes it.
    Q_INVOKABLE void setField(int field, const QString& text);
    Q_INVOKABLE void revertField(int field);
    // A field by name, for one that no file has yet.
    Q_INVOKABLE void addField(const QString& name, const QString& text);
    Q_INVOKABLE void preview();
    // Back from reviewing to editing.
    Q_INVOKABLE void back();
    Q_INVOKABLE void apply();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void close();

  signals:
    void changed();
    // Files were written: lists and the library show them anew.
    void written(trackknife::quick::EngineSession* session);

  private:
    void setStage(const QString& stage, const QString& status = {});
    void buildPlanRows();

    EngineSession* session_{nullptr};
    std::shared_ptr<engine::RemoteFileWork> work_;
    std::shared_ptr<const metadata::StagedMetadataSelection> selection_;
    std::shared_ptr<metadata::StagedMetadataPatchSet> patches_;
    std::shared_ptr<const metadata::MetadataWritePlan> write_plan_;
    QVariantList plan_rows_;
    QString stage_{QStringLiteral("idle")};
    QString status_;
    core::CancellationSource cancellation_;
    // Answers for an editor since closed or reopened are dropped by this.
    std::uint64_t generation_{0};
    QThreadPool pool_;
};

} // namespace trackknife::quick
