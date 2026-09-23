// SPDX-License-Identifier: GPL-3.0-only

#include "bench/engine_launcher.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include <signal.h>

#include <atomic>
#include <cstdlib>
#include <thread>

namespace trackknife::bench {
namespace {

[[nodiscard]] core::Error launch_error(std::string message) {
    return core::Error{
        .code = core::ErrorCode::backend, .message = std::move(message), .context = {}};
}

[[nodiscard]] std::filesystem::path runtime_directory() {
    if (const auto* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime != nullptr && *runtime) {
        return runtime;
    }
    return std::filesystem::temp_directory_path();
}

// What melodyd uses when started with no arguments, so the engine a
// workspace starts and one started by hand or by the user service are the
// same engine on the same socket.
[[nodiscard]] std::filesystem::path standard_state() {
    if (const auto* data_home = std::getenv("XDG_DATA_HOME"); data_home != nullptr && *data_home) {
        return std::filesystem::path{data_home} / "trackknife" / "trackknife";
    }
    return std::filesystem::path{QDir::homePath().toStdString()} / ".local" / "share" /
           "trackknife" / "trackknife";
}

[[nodiscard]] QString path_text(const std::filesystem::path& path) {
    return QFile::decodeName(QByteArray::fromStdString(path.native()));
}

std::atomic_bool local_engine_allowed{false};

} // namespace

void allowLocalEngine(const bool allowed) { local_engine_allowed.store(allowed); }

std::optional<LocalEngine> localEngine() {
    if (!local_engine_allowed.load()) {
        return std::nullopt;
    }
    const auto data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (data.isEmpty()) {
        return std::nullopt;
    }
    LocalEngine engine{.state = std::filesystem::path{QFile::encodeName(data).toStdString()},
                       .socket = {}};
    // Another database gets another socket, or its engine would be asked
    // about a library it does not have.
    if (std::filesystem::weakly_canonical(engine.state) ==
        std::filesystem::weakly_canonical(standard_state())) {
        engine.socket = runtime_directory() / "melodyd.sock";
    } else {
        const auto digest =
            QCryptographicHash::hash(QFile::encodeName(data), QCryptographicHash::Sha256)
                .toHex()
                .left(12);
        engine.socket = runtime_directory() / ("melodyd-" + digest.toStdString() + ".sock");
    }
    return engine;
}

QString engineProgram() {
    if (const auto explicit_program = qEnvironmentVariable("TRACKKNIFE_ENGINE");
        !explicit_program.isEmpty()) {
        return explicit_program;
    }
    const QDir here{QCoreApplication::applicationDirPath()};
    // Installed side by side, or the build tree's src/bench beside src/daemon.
    for (const auto& candidate : {here.filePath(QStringLiteral("melodyd")),
                                  here.filePath(QStringLiteral("../daemon/melodyd"))}) {
        if (QFileInfo{candidate}.isExecutable()) {
            return QDir::cleanPath(candidate);
        }
    }
    return {};
}

core::Result<std::unique_ptr<protocol::Client>>
connectLocalEngine(const LocalEngine& engine, const std::chrono::milliseconds timeout) {
    const protocol::Endpoint endpoint{.socket = engine.socket, .host = {}, .port = 0, .token = {}};
    if (auto running = protocol::Client::connect(endpoint)) {
        return running;
    }
    const auto program = engineProgram();
    if (program.isEmpty()) {
        return std::unexpected(launch_error("melodyd is not installed"));
    }
    std::error_code ignored;
    std::filesystem::create_directories(engine.state, ignored);
    const auto log = path_text(engine.state / "melodyd.log");
    QProcess process;
    process.setProgram(program);
    process.setArguments({QStringLiteral("--socket"), path_text(engine.socket),
                          QStringLiteral("--state"), path_text(engine.state)});
    process.setWorkingDirectory(path_text(engine.state));
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(log, QIODevice::Append);
    process.setStandardErrorFile(log, QIODevice::Append);
    // Its own session: closing the terminal or the window must not take the
    // music with it.
    qint64 pid = 0;
    if (!process.startDetached(&pid)) {
        return std::unexpected(launch_error("could not start " + program.toStdString()));
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto started = protocol::Client::connect(endpoint)) {
            return started;
        }
        // An engine that exited has said why in its log; waiting on would
        // only delay saying so. Another engine winning a start race exits
        // too, and is then the one that answers.
        if (pid > 0 && ::kill(static_cast<pid_t>(pid), 0) != 0) {
            if (auto other = protocol::Client::connect(endpoint)) {
                return other;
            }
            return std::unexpected(
                launch_error("melodyd stopped at startup; see " + log.toStdString()));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    return std::unexpected(
        launch_error("melodyd did not start listening; see " + log.toStdString()));
}

} // namespace trackknife::bench
