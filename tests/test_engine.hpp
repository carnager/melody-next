// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// A melodyd for one test, on the database the window under test uses, the way
// the application runs one on its own (ADR-0226). A child process rather than
// a detached one: whatever happens to the test, the engine goes with it.

#include "bench/settings_dialog.hpp"
#include "trackknife/protocol/client.hpp"

#include <QDir>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <chrono>
#include <filesystem>
#include <thread>

namespace trackknife::bench::testing {

class TestEngine final {
  public:
    TestEngine() = default;
    TestEngine(const TestEngine&) = delete;
    TestEngine& operator=(const TestEngine&) = delete;
    ~TestEngine() { stop(); }

    // Starts on `state_directory` -- its database is lists.sqlite there -- or
    // on the application's data directory, and names the engine in settings,
    // so a window or catalogue built afterwards uses it. False if it never
    // listened; the process output is then in log().
    [[nodiscard]] bool start(const std::filesystem::path& state_directory = {}) {
        stop();
        const auto state = state_directory.empty()
                               ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                               : QString::fromStdString(state_directory.string());
        QDir{}.mkpath(state);
        socket_ = runtime_.filePath(QStringLiteral("melodyd.sock"));
        process_.setProgram(QStringLiteral(TRACKKNIFE_ENGINE_BINARY));
        process_.setArguments(
            {QStringLiteral("--socket"), socket_, QStringLiteral("--state"), state});
        process_.setProcessChannelMode(QProcess::MergedChannels);
        process_.start();
        if (!process_.waitForStarted()) {
            return false;
        }
        const protocol::Endpoint endpoint{
            .socket = socket_.toStdString(), .host = {}, .port = 0, .token = {}};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (std::chrono::steady_clock::now() < deadline) {
            if (process_.state() != QProcess::Running) {
                return false;
            }
            if (protocol::Client::connect(endpoint)) {
                QSettings{}.setValue(QLatin1String(SettingsDialog::library_engine_socket_key),
                                     socket_);
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return false;
    }

    // Before the data directory is removed: the engine holds the database.
    void stop() {
        if (process_.state() == QProcess::NotRunning) {
            return;
        }
        process_.terminate();
        if (!process_.waitForFinished(5'000)) {
            process_.kill();
            process_.waitForFinished();
        }
    }

    [[nodiscard]] QByteArray log() { return process_.readAll(); }
    [[nodiscard]] const QString& socket() const noexcept { return socket_; }

  private:
    QTemporaryDir runtime_;
    QProcess process_;
    QString socket_;
};

} // namespace trackknife::bench::testing
