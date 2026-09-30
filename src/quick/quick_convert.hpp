// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "workspace/convert_job.hpp"

#include <QObject>
#include <QQmlEngine>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

namespace trackknife::quick {

// "Convert files…" in the Qt Quick window: a ConvertJob, as QML draws it.
class QuickConvert final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Opened by the workspace")
    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(QVariantMap state READ state NOTIFY changed)
    Q_PROPERTY(QVariantList presets READ presets NOTIFY presetsChanged)
    Q_PROPERTY(QStringList layouts READ layouts NOTIFY profilesChanged)
    Q_PROPERTY(QStringList destinations READ destinations NOTIFY profilesChanged)
    Q_PROPERTY(QVariantList resampleChoices READ resampleChoices CONSTANT)
    Q_PROPERTY(QVariantList bitDepthChoices READ bitDepthChoices CONSTANT)
    Q_PROPERTY(QVariantList channelChoices READ channelChoices CONSTANT)
    Q_PROPERTY(QVariantList gainChoices READ gainChoices CONSTANT)
    Q_PROPERTY(QStringList editorFormats READ editorFormats CONSTANT)
    Q_PROPERTY(int maximumParallelism READ maximumParallelism CONSTANT)

  public:
    explicit QuickConvert(bench::ConvertJob* job, QObject* parent = nullptr);

    [[nodiscard]] QString title() const { return job_->title(); }
    [[nodiscard]] QVariantMap state() const;
    [[nodiscard]] QVariantList presets() const;
    [[nodiscard]] QStringList layouts() const { return job_->layouts(); }
    [[nodiscard]] QStringList destinations() const { return job_->destinations(); }
    [[nodiscard]] static QVariantList resampleChoices();
    [[nodiscard]] static QVariantList bitDepthChoices();
    [[nodiscard]] static QVariantList channelChoices();
    [[nodiscard]] static QVariantList gainChoices();
    [[nodiscard]] static QStringList editorFormats() { return bench::ConvertJob::editorFormats(); }
    [[nodiscard]] static int maximumParallelism() {
        return bench::ConvertJob::maximumParallelism();
    }

    Q_INVOKABLE [[nodiscard]] static bool formatLossless(int format) {
        return bench::ConvertJob::editorFormatLossless(format);
    }
    // The new-preset editor's starting values: name, format, qualityMode,
    // bitrate, quality.
    Q_INVOKABLE [[nodiscard]] QVariantMap presetDraft() const;
    Q_INVOKABLE void selectPreset(const QString& id) { job_->selectPreset(id); }
    Q_INVOKABLE void setDestination(const QString& root, bool typed) {
        job_->setDestination(root, typed);
    }
    Q_INVOKABLE void chooseDestinationFolder(const QUrl& folder) {
        job_->setDestination(folder.toLocalFile(), true);
    }
    Q_INVOKABLE void selectLayout(int choice) { job_->selectLayout(choice); }
    Q_INVOKABLE void selectDestination(int choice) { job_->selectDestination(choice); }
    Q_INVOKABLE void setMirror(bool on) { job_->setMirror(on); }
    Q_INVOKABLE void setDirectoryExpression(const QString& text) {
        job_->setDirectoryExpression(text, true);
    }
    Q_INVOKABLE void setBasenameExpression(const QString& text) {
        job_->setBasenameExpression(text, true);
    }
    Q_INVOKABLE void setResample(const QVariant& value) { job_->setResample(value); }
    Q_INVOKABLE void setBitDepth(const QVariant& value) { job_->setBitDepth(value); }
    Q_INVOKABLE void setChannels(const QVariant& value) { job_->setChannels(value); }
    Q_INVOKABLE void setGain(int value) { job_->setGain(value); }
    Q_INVOKABLE void setEmbedArtwork(bool on) { job_->setEmbedArtwork(on); }
    Q_INVOKABLE void setParallelism(int value) { job_->setParallelism(value); }
    Q_INVOKABLE void run() { job_->run(); }
    Q_INVOKABLE void stop() { job_->stop(); }
    Q_INVOKABLE void savePreset(const QVariantMap& draft);
    Q_INVOKABLE void exportPreset(const QUrl& file) { job_->exportPreset(file.toLocalFile()); }
    Q_INVOKABLE [[nodiscard]] QString suggestedPresetExportName() const {
        return job_->suggestedPresetExportName();
    }
    Q_INVOKABLE void deletePreset() { job_->deletePreset(); }
    Q_INVOKABLE bool requestClose() { return job_->requestClose(); }
    // Its window closed: gone.
    Q_INVOKABLE void release() { deleteLater(); }

  signals:
    void changed();
    void presetsChanged();
    void profilesChanged();
    void filesConverted();

  private:
    [[nodiscard]] static QVariantList choices(const std::vector<bench::ConvertJob::Choice>& list);

    bench::ConvertJob* job_;
};

} // namespace trackknife::quick
