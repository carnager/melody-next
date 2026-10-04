// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QLockFile>
#include <QObject>
#include <QString>

#include <memory>
#include <string>
#include <vector>

class QLocalServer;

namespace trackknife::bench {

// One Trackknife per settings location (ADR-0267): the first claims it; a
// later one hands its files to the first, which comes forward, and exits.
// Two windows over one workspace would each save over the other's lists,
// layout and settings.
class SingleInstance final : public QObject {
    Q_OBJECT
  public:
    // `key` names whose instance this is -- by default, the settings
    // location, so a sandbox with its own runs apart.
    explicit SingleInstance(QString key = defaultKey(), QObject* parent = nullptr);
    ~SingleInstance() override;

    // True when this is the one; false when another running took `raw_paths`.
    [[nodiscard]] bool claim(const std::vector<std::string>& raw_paths);

    [[nodiscard]] static QString defaultKey();

  signals:
    // Another start, handing over the files it was given (perhaps none).
    void asked(std::vector<std::string> raw_paths);

  private:
    [[nodiscard]] bool handOver(const std::vector<std::string>& raw_paths);
    void listen();

    QString name_;
    std::unique_ptr<QLockFile> lock_;
    QLocalServer* server_{nullptr};
};

} // namespace trackknife::bench
