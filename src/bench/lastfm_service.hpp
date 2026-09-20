// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "trackknife/core/listen_accounting.hpp"
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QStringList>
#include <QThread>
#include <QUrl>

namespace trackknife::bench {
// The worker owns network, credentials, and disk; observations never do I/O on the UI thread.
class LastFmService final : public QObject {
    Q_OBJECT
  public:
    explicit LastFmService(QString state_path, QObject* parent = nullptr, QUrl endpoint = {});
    ~LastFmService() override;
    void execute(const QString& operation, const QStringList& arguments = {});
    void observe(QJsonObject sample);
  signals:
    void completed(const QString& operation, const QJsonObject& state, const QString& error);

  private:
    QThread thread_;
    QObject* worker_{};
};
QByteArray lastFmSignature(const QMap<QString, QString>& parameters, const QString& secret);
// Shared deterministic accounting contract, independently exercised with injected times.
using LastFmListen = core::ListenAccounting;

} // namespace trackknife::bench
