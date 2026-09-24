// SPDX-License-Identifier: GPL-3.0-only

#include "bench/engine_launcher.hpp"

#include "bench/settings_dialog.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>

#include <signal.h>

#include <algorithm>
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

[[nodiscard]] std::filesystem::path path_from_text(const QString& text) {
    return std::filesystem::path{QFile::encodeName(text).toStdString()};
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

LocalEngineSharing localEngineSharing() {
    const QSettings settings;
    return LocalEngineSharing{
        .share = settings.value(QLatin1String(SettingsDialog::engine_share_key), false).toBool(),
        .listen = settings
                      .value(QLatin1String(SettingsDialog::engine_listen_key),
                             QString::fromLatin1(SettingsDialog::engine_listen_default))
                      .toString()
                      .trimmed(),
        .stream_port = settings
                           .value(QLatin1String(SettingsDialog::engine_stream_port_key),
                                  SettingsDialog::engine_stream_port_default)
                           .toInt(),
        .password =
            settings.value(QLatin1String(SettingsDialog::engine_password_key), QString{})
                .toString(),
        .music_root =
            settings.value(QLatin1String(SettingsDialog::engine_music_root_key), QString{})
                .toString()
                .trimmed(),
        .play_for = settings.value(QLatin1String(SettingsDialog::engine_play_for_remote_key), true)
                            .toBool()
                        ? settings.value(QLatin1String(SettingsDialog::library_engine_socket_key))
                              .toString()
                              .trimmed()
                        : QString{},
        .play_for_password =
            settings.value(QLatin1String(SettingsDialog::library_engine_token_key)).toString(),
        .play_for_music_root =
            settings.value(QLatin1String(SettingsDialog::library_remote_mount_key))
                .toString()
                .trimmed(),
        .play_for_found =
            settings.value(QLatin1String(SettingsDialog::engine_play_for_remote_key), true)
                .toBool(),
    };
}

QStringList localEngineArguments(const LocalEngine& engine, const LocalEngineSharing& sharing) {
    QStringList arguments;
    const auto password_file = engine.state / "engine.password";
    std::error_code ignored;
    if (!sharing.music_root.isEmpty()) {
        arguments << QStringLiteral("--music-root") << sharing.music_root;
    }
    // Any engine found on the network may play here -- and the configured
    // remote by name as well, for one multicast does not reach (WireGuard).
    if (sharing.play_for_found) {
        arguments << QStringLiteral("--agent");
    }
    // Only a remote on the network: one on this computer's own socket plays
    // here already.
    const auto guest_password = engine.state / "play-for.password";
    std::filesystem::remove(guest_password, ignored);
    if (sharing.play_for.contains(QLatin1Char(':'))) {
        arguments << QStringLiteral("--play-for") << sharing.play_for;
        if (!sharing.play_for_music_root.isEmpty()) {
            arguments << QStringLiteral("--play-for-music-root") << sharing.play_for_music_root;
        }
        if (!sharing.play_for_password.isEmpty()) {
            QFile file{path_text(guest_password)};
            if (file.open(QIODevice::WriteOnly | QIODevice::Truncate,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
                file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                file.write(sharing.play_for_password.toUtf8() + '\n');
                file.close();
                arguments << QStringLiteral("--play-for-password-file") << path_text(guest_password);
            }
        }
    }
    if (!sharing.share || sharing.listen.isEmpty()) {
        // Not shared: this computer only, not melodyd's default network ports.
        arguments << QStringLiteral("--local-only");
        std::filesystem::remove(password_file, ignored);
        return arguments;
    }
    arguments << QStringLiteral("--listen") << sharing.listen;
    // Streams on the same address as the engine, on their own port.
    const auto colon = sharing.listen.lastIndexOf(QLatin1Char(':'));
    if (colon > 0 && sharing.stream_port > 0) {
        arguments << QStringLiteral("--http")
                  << QStringLiteral("%1:%2").arg(sharing.listen.left(colon)).arg(sharing.stream_port);
    }
    if (sharing.password.isEmpty()) {
        std::filesystem::remove(password_file, ignored);
        return arguments;
    }
    QFile file{path_text(password_file)};
    // Owner-only from the moment it exists, not after.
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate, QFileDevice::ReadOwner |
                                                                   QFileDevice::WriteOwner)) {
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        file.write(sharing.password.toUtf8() + '\n');
        file.close();
        arguments << QStringLiteral("--password-file") << path_text(password_file);
    }
    return arguments;
}

core::Result<void> stopLocalEngine(const LocalEngine& engine) {
    // The engine is whoever holds its lock. Found through /proc, as fuser
    // does, because flock(2) does not say who holds it -- and signalled,
    // because an engine older than this workspace knows no request for it.
    const auto holders = lockHolders(engine.state / "engine.lock");
    for (const auto pid : holders) {
        ::kill(pid, SIGTERM);
    }
    // SIGTERM saves the queue first; it comes back paused.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    const auto alive = [&holders] {
        return std::ranges::any_of(holders, [](const pid_t pid) { return ::kill(pid, 0) == 0; });
    };
    while (alive() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    if (alive()) {
        return std::unexpected(launch_error("the running engine did not stop"));
    }
    return {};
}

core::Result<void> restartLocalEngine(const LocalEngine& engine) {
    if (auto stopped = stopLocalEngine(engine); !stopped) {
        return stopped;
    }
    auto started = connectLocalEngine(engine);
    if (!started) {
        return std::unexpected(std::move(started.error()));
    }
    (*started)->close();
    return {};
}

bool localEngineOutdated(const LocalEngine& engine) {
    const auto program = engineProgram();
    for (const auto pid : lockHolders(engine.state / "engine.lock")) {
        std::error_code error;
        const auto running =
            std::filesystem::read_symlink("/proc/" + std::to_string(pid) + "/exe", error);
        if (error) {
            continue;
        }
        // Replaced since it started: the kernel names what it runs as gone.
        if (running.native().ends_with(" (deleted)")) {
            return true;
        }
        // Or not the engine this build would start at all.
        if (!program.isEmpty() &&
            !std::filesystem::equivalent(running, path_from_text(program), error) && !error) {
            return true;
        }
    }
    return false;
}

std::vector<pid_t> lockHolders(const std::filesystem::path& lock) {
    std::vector<pid_t> holders;
    std::error_code error;
    const auto target = std::filesystem::weakly_canonical(lock, error);
    if (error) {
        return holders;
    }
    for (const auto& process : std::filesystem::directory_iterator{"/proc", error}) {
        const auto name = process.path().filename().string();
        if (name.empty() || !std::ranges::all_of(name, [](const char c) { return c >= '0' && c <= '9'; })) {
            continue;
        }
        std::error_code unreadable;
        for (const auto& descriptor :
             std::filesystem::directory_iterator{process.path() / "fd", unreadable}) {
            std::error_code gone;
            if (std::filesystem::read_symlink(descriptor.path(), gone) == target) {
                holders.push_back(static_cast<pid_t>(std::stoi(name)));
                break;
            }
        }
    }
    return holders;
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
    process.setArguments(QStringList{QStringLiteral("--socket"), path_text(engine.socket),
                                     QStringLiteral("--state"), path_text(engine.state)} +
                         localEngineArguments(engine, localEngineSharing()));
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
