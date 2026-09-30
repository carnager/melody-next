// SPDX-License-Identifier: GPL-3.0-only

#include "quick/quick_convert.hpp"

namespace trackknife::quick {

QuickConvert::QuickConvert(bench::ConvertJob* job, QObject* parent) : QObject(parent), job_(job) {
    job_->setParent(this);
    connect(job_, &bench::ConvertJob::changed, this, &QuickConvert::changed);
    connect(job_, &bench::ConvertJob::presetsChanged, this, &QuickConvert::presetsChanged);
    connect(job_, &bench::ConvertJob::profilesChanged, this, &QuickConvert::profilesChanged);
    connect(job_, &bench::ConvertJob::filesConverted, this, &QuickConvert::filesConverted);
}

QVariantMap QuickConvert::state() const {
    const auto& job = *job_;
    return {{QStringLiteral("presetIndex"), job.presetIndex()},
            {QStringLiteral("canAddPreset"), job.canAddPreset()},
            {QStringLiteral("canDeletePreset"), job.canDeletePreset()},
            {QStringLiteral("layoutChoice"), job.layoutChoice()},
            {QStringLiteral("destinationChoice"), job.destinationChoice()},
            {QStringLiteral("destination"), job.destination()},
            {QStringLiteral("mirror"), job.mirror()},
            {QStringLiteral("directoryExpression"), job.directoryExpression()},
            {QStringLiteral("basenameExpression"), job.basenameExpression()},
            {QStringLiteral("resample"), job.resample()},
            {QStringLiteral("bitDepth"), job.bitDepth()},
            {QStringLiteral("channels"), job.channels()},
            {QStringLiteral("gain"), job.gain()},
            {QStringLiteral("gainWarning"), job.gainWarning()},
            {QStringLiteral("gainConfirmation"), job.gainConfirmation()},
            {QStringLiteral("embedArtwork"), job.embedArtwork()},
            {QStringLiteral("parallelism"), job.parallelism()},
            {QStringLiteral("preview"), job.preview()},
            {QStringLiteral("status"), job.status()},
            {QStringLiteral("problems"), job.problems().join(QStringLiteral("\n"))},
            {QStringLiteral("running"), job.running()},
            {QStringLiteral("canRun"), job.canRun()},
            {QStringLiteral("progress"), job.progressValue()},
            {QStringLiteral("progressMaximum"), job.progressMaximum()}};
}

QVariantList QuickConvert::presets() const {
    QVariantList rows;
    for (const auto& preset : job_->presets()) {
        rows.push_back(QVariantMap{{QStringLiteral("id"), preset.id},
                                   {QStringLiteral("name"), preset.name},
                                   {QStringLiteral("available"), preset.available},
                                   {QStringLiteral("detail"), preset.detail},
                                   {QStringLiteral("separator"), preset.separator}});
    }
    return rows;
}

QVariantList QuickConvert::choices(const std::vector<bench::ConvertJob::Choice>& list) {
    QVariantList rows;
    for (const auto& choice : list) {
        rows.push_back(QVariantMap{{QStringLiteral("label"), choice.label},
                                   {QStringLiteral("value"), choice.value}});
    }
    return rows;
}

QVariantList QuickConvert::resampleChoices() {
    return choices(bench::ConvertJob::resampleChoices());
}

QVariantList QuickConvert::bitDepthChoices() {
    return choices(bench::ConvertJob::bitDepthChoices());
}

QVariantList QuickConvert::channelChoices() { return choices(bench::ConvertJob::channelChoices()); }

QVariantList QuickConvert::gainChoices() { return choices(bench::ConvertJob::gainChoices()); }

QVariantMap QuickConvert::presetDraft() const {
    const auto draft = job_->presetDraft();
    return {{QStringLiteral("format"), draft.format},
            {QStringLiteral("qualityMode"), draft.quality_mode},
            {QStringLiteral("bitrate"), draft.bitrate_kbps},
            {QStringLiteral("quality"), draft.quality}};
}

void QuickConvert::savePreset(const QVariantMap& draft) {
    job_->savePreset(bench::ConvertJob::PresetDraft{
        .name = draft.value(QStringLiteral("name")).toString(),
        .format = draft.value(QStringLiteral("format")).toInt(),
        .quality_mode = draft.value(QStringLiteral("qualityMode")).toBool(),
        .bitrate_kbps = draft.value(QStringLiteral("bitrate"), 192).toInt(),
        .quality = draft.value(QStringLiteral("quality"), 4).toInt()});
}

} // namespace trackknife::quick
