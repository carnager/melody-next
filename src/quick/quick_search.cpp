// SPDX-License-Identifier: GPL-3.0-only

#include "quick/quick_search.hpp"

namespace trackknife::quick {

QuickSearch::QuickSearch(bench::SearchSession* session, QObject* parent)
    : QObject(parent), session_(session) {
    session_->setParent(this);
    connect(session_, &bench::SearchSession::changed, this, &QuickSearch::changed);
    connect(session_, &bench::SearchSession::savedChanged, this, &QuickSearch::savedChanged);
    connect(session_, &bench::SearchSession::resultsChanged, this, &QuickSearch::resultsChanged);
}

QStringList QuickSearch::scopes() const {
    QStringList labels;
    for (const auto& scope : session_->scopes()) {
        labels.append(scope.label);
    }
    return labels;
}

QVariantList QuickSearch::presetGroups() {
    QVariantList groups;
    for (const auto& group : bench::SearchSession::presetGroups()) {
        QVariantList presets;
        for (const auto& preset : group.presets) {
            presets.append(QVariantMap{{QStringLiteral("title"), preset.title},
                                       {QStringLiteral("id"), preset.id},
                                       {QStringLiteral("index"), preset.index}});
        }
        groups.append(QVariantMap{{QStringLiteral("topic"), group.topic},
                                  {QStringLiteral("presets"), presets}});
    }
    return groups;
}

QVariantMap QuickSearch::state() const {
    return {{QStringLiteral("scope"), session_->scope()},
            {QStringLiteral("text"), session_->text()},
            {QStringLiteral("queryMode"), session_->queryMode()},
            {QStringLiteral("error"), session_->error()},
            {QStringLiteral("status"), session_->status()},
            {QStringLiteral("canOpen"), session_->canOpen()}};
}

QVariantMap QuickSearch::saved() const {
    return {{QStringLiteral("names"), session_->savedNames()},
            {QStringLiteral("tooltips"), session_->savedTooltips()},
            {QStringLiteral("index"), session_->savedIndex()},
            {QStringLiteral("status"), session_->savedStatus()},
            {QStringLiteral("available"), session_->savedAvailable()},
            {QStringLiteral("canSave"), session_->canSave()},
            {QStringLiteral("canUpdate"), session_->canUpdate()},
            {QStringLiteral("canRename"), session_->canRenameOrDelete()},
            {QStringLiteral("suggestedName"), session_->suggestedName()},
            {QStringLiteral("selectedName"), session_->selectedName()},
            {QStringLiteral("deleteQuestion"), session_->deleteQuestion()}};
}

QVariantList QuickSearch::results() const {
    QVariantList rows;
    for (const auto& result : session_->results()) {
        rows.append(QVariantMap{{QStringLiteral("label"), result.label},
                                {QStringLiteral("heading"), result.heading}});
    }
    return rows;
}

QVariantMap QuickSearch::presetInput(const int preset) {
    const auto input = bench::SearchSession::presetInput(preset);
    const auto kind = input.kind == bench::SearchSession::PresetInput::Kind::integer
                          ? QStringLiteral("integer")
                      : input.kind == bench::SearchSession::PresetInput::Kind::text
                          ? QStringLiteral("text")
                          : QStringLiteral("none");
    return {{QStringLiteral("kind"), kind},         {QStringLiteral("title"), input.title},
            {QStringLiteral("prompt"), input.prompt}, {QStringLiteral("value"), input.value},
            {QStringLiteral("minimum"), input.minimum}, {QStringLiteral("maximum"), input.maximum},
            {QStringLiteral("step"), input.step}};
}

void QuickSearch::openAll(const int action) {
    session_->openAll(static_cast<bench::LocalLibraryAction>(action));
}

void QuickSearch::openRows(const QVariantList& rows, const int action) {
    std::vector<int> chosen;
    for (const auto& row : rows) {
        chosen.push_back(row.toInt());
    }
    session_->openRows(std::move(chosen), static_cast<bench::LocalLibraryAction>(action));
}

} // namespace trackknife::quick
