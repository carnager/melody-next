// SPDX-License-Identifier: GPL-3.0-only

#include "quick/quick_script.hpp"

namespace trackknife::quick {

QuickScript::QuickScript(bench::ScriptSession* session, QObject* parent)
    : QObject(parent), session_(session) {
    session_->setParent(this);
    connect(session_, &bench::ScriptSession::changed, this, &QuickScript::changed);
    connect(session_, &bench::ScriptSession::savedChanged, this, &QuickScript::savedChanged);
    connect(session_, &bench::ScriptSession::stepsChanged, this, &QuickScript::stepsChanged);
    connect(session_, &bench::ScriptSession::rawChanged, this, &QuickScript::rawChanged);
    connect(session_, &bench::ScriptSession::previewChanged, this, &QuickScript::previewChanged);
    connect(session_, &bench::ScriptSession::accepted, this, &QuickScript::accepted);
    connect(session_, &bench::ScriptSession::closeRequested, this, &QuickScript::closeRequested);
}

QVariantMap QuickScript::state() const {
    const auto& session = *session_;
    return {
        {QStringLiteral("name"), session.name()},
        {QStringLiteral("summary"), session.summary()},
        {QStringLiteral("catalogStatus"), session.catalogStatus()},
        {QStringLiteral("rawReadOnly"), session.rawReadOnly()},
        {QStringLiteral("rawDiagnostics"), session.rawDiagnostics()},
        {QStringLiteral("unsaved"), session.unsaved()},
        {QStringLiteral("editing"), session.editing()},
        {QStringLiteral("canSelectSaved"), session.canSelectSaved()},
        {QStringLiteral("saveText"), session.saveText()},
        {QStringLiteral("canSave"), session.canSave()},
        {QStringLiteral("canSaveAsNew"), session.canSaveAsNew()},
        {QStringLiteral("canDelete"), session.canDelete()},
        {QStringLiteral("canImport"), session.canImport()},
        {QStringLiteral("canExport"), session.canExport()},
        {QStringLiteral("canAdd"), session.canAdd()},
        {QStringLiteral("canStage"), session.canStage()},
    };
}

QVariantList QuickScript::stepKinds() {
    QVariantList kinds;
    for (const auto& kind : bench::ScriptSession::stepKinds()) {
        kinds.push_back(QVariantMap{{QStringLiteral("label"), kind.label},
                                    {QStringLiteral("kind"), kind.kind},
                                    {QStringLiteral("toolTip"), kind.tool_tip}});
    }
    return kinds;
}

QVariantMap QuickScript::stepForm(const int kind, const int capture_source) {
    const auto form = bench::ScriptSession::stepForm(kind, capture_source);
    return {{QStringLiteral("target"), form.target},
            {QStringLiteral("input"), form.input},
            {QStringLiteral("inputLabel"), form.input_label},
            {QStringLiteral("inputPlaceholder"), form.input_placeholder},
            {QStringLiteral("replacement"), form.replacement},
            {QStringLiteral("numbering"), form.numbering},
            {QStringLiteral("characters"), form.characters},
            {QStringLiteral("captureSource"), form.capture_source},
            {QStringLiteral("captureArgument"), form.capture_argument},
            {QStringLiteral("captureArgumentLabel"), form.capture_argument_label},
            {QStringLiteral("captureArgumentPlaceholder"), form.capture_argument_placeholder},
            {QStringLiteral("fieldList"), form.field_list},
            {QStringLiteral("ratingScale"), form.rating_scale}};
}

QVariantMap QuickScript::translateRuleScript(const QString& source) {
    const auto translated = bench::ScriptSession::translateRuleScript(source);
    return {{QStringLiteral("diagnostics"), translated.diagnostics},
            {QStringLiteral("ready"), translated.ready}};
}

QString QuickScript::focusName(const bench::ScriptSession::Focus focus) {
    switch (focus) {
    case bench::ScriptSession::Focus::target:
        return QStringLiteral("target");
    case bench::ScriptSession::Focus::input:
        return QStringLiteral("input");
    case bench::ScriptSession::Focus::capture_argument:
        return QStringLiteral("argument");
    case bench::ScriptSession::Focus::name:
        return QStringLiteral("name");
    case bench::ScriptSession::Focus::none:
        break;
    }
    return {};
}

QString QuickScript::addStep(const QVariantMap& step) {
    return focusName(session_->addStep(bench::ScriptSession::StepInput{
        .kind = step.value(QStringLiteral("kind"), -1).toInt(),
        .target = step.value(QStringLiteral("target")).toString(),
        .input = step.value(QStringLiteral("input")).toString(),
        .replacement = step.value(QStringLiteral("replacement")).toString(),
        .number_start = step.value(QStringLiteral("numberStart"), 1).toInt(),
        .number_padding = step.value(QStringLiteral("numberPadding"), 0).toInt(),
        .character_count = step.value(QStringLiteral("characterCount"), 4).toInt(),
        .capture_source = step.value(QStringLiteral("captureSource"), 0).toInt(),
        .capture_argument = step.value(QStringLiteral("captureArgument")).toString(),
        .rating_scale = step.value(QStringLiteral("ratingScale"), 0).toInt(),
    }));
}

QString QuickScript::save(const bool as_new) { return focusName(session_->save(as_new)); }

QString QuickScript::requestClose() {
    switch (session_->requestClose()) {
    case bench::ScriptSession::CloseAnswer::close:
        return QStringLiteral("close");
    case bench::ScriptSession::CloseAnswer::confirm_discard:
        return QStringLiteral("confirm");
    case bench::ScriptSession::CloseAnswer::wait:
        return QStringLiteral("wait");
    }
    return QStringLiteral("close");
}

} // namespace trackknife::quick
