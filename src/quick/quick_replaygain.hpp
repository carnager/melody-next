// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "workspace/replaygain_job.hpp"

#include <QObject>
#include <QQmlEngine>
#include <QVariantMap>

namespace trackknife::quick {

// "ReplayGain…" in the Qt Quick window: a ReplayGainJob, as QML draws it.
class QuickReplayGain final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Opened by the workspace")
    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(QStringList groupings READ groupings CONSTANT)
    Q_PROPERTY(QVariantMap state READ state NOTIFY changed)

  public:
    explicit QuickReplayGain(bench::ReplayGainJob* job, QObject* parent = nullptr);

    [[nodiscard]] QString title() const { return job_->title(); }
    [[nodiscard]] static QStringList groupings() { return bench::ReplayGainJob::groupings(); }
    [[nodiscard]] QVariantMap state() const;

    Q_INVOKABLE void setGrouping(int index) { job_->setGrouping(index); }
    Q_INVOKABLE void setExpression(const QString& expression) { job_->setExpression(expression); }
    Q_INVOKABLE void setSidecarOnly(bool on) { job_->setSidecarOnly(on); }
    Q_INVOKABLE void setTruePeak(bool on) { job_->setTruePeak(on); }
    Q_INVOKABLE void preview() { job_->preview(); }
    Q_INVOKABLE void run() { job_->run(); }
    Q_INVOKABLE void stop() { job_->stop(); }
    // Its window closed: gone.
    Q_INVOKABLE void release() { deleteLater(); }

  signals:
    void changed();

  private:
    bench::ReplayGainJob* job_;
};

} // namespace trackknife::quick
