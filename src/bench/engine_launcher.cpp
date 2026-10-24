// SPDX-License-Identifier: GPL-3.0-only

#include "bench/engine_launcher.hpp"

#include "bench/remote_engines.hpp"
#include "bench/settings_keys.hpp"
#include "trackknife/core/engine_state.hpp"
#include "trackknife/core/environment.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QStandardPaths>

#include <signal.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <libproc.h>
#include <sys/proc.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace trackknife::bench {
namespace {

[[nodiscard]] core::Error launch_error(std::string message) {
    return core::Error{
        .code = core::ErrorCode::backend, .message = std::move(message), .context = {}};
}

[[nodiscard]] std::filesystem::path runtime_directory() {
    if (const auto* runtime = trackknife::core::environment_variable("XDG_RUNTIME_DIR");
        runtime != nullptr && *runtime) {
        return runtime;
    }
    return std::filesystem::temp_directory_path();
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
    // ADR-0287: the engine's own folder, not the window's: the one melodyd
    // uses when started with no arguments.
    LocalEngine engine{.state = core::engine_state_directory(), .socket = {}, .standard = false};
    const auto data = QFile::decodeName(QByteArray::fromStdString(engine.state.native()));
    // Another database gets another socket, or its engine would be asked
    // about a library it does not have.
    if (core::environment_variable("MELODY_STATE_DIR") == nullptr) {
        engine.socket = runtime_directory() / "melodyd.sock";
        engine.standard = true;
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
    if (const auto explicit_program = qEnvironmentVariable("MELODY_ENGINE");
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

namespace {

// ADR-0239: a rate melodyd takes -- 0, or Opus at 16 to 512 kbps -- else
// the default.
[[nodiscard]] int kbps_setting(const QSettings& settings, const char* key, const int fallback) {
    const auto kbps = settings.value(QLatin1String(key), fallback).toInt();
    return kbps == 0 || (kbps >= 16 && kbps <= 512) ? kbps : fallback;
}

[[nodiscard]] int streamNearbyKbps(const QSettings& settings) {
    return kbps_setting(settings, SettingsKeys::engine_stream_nearby_key,
                        SettingsKeys::engine_stream_nearby_default);
}

[[nodiscard]] int streamAwayKbps(const QSettings& settings) {
    return kbps_setting(settings, SettingsKeys::engine_stream_away_key,
                        SettingsKeys::engine_stream_away_default);
}

} // namespace

LocalEngineSharing localEngineSharing() {
    const QSettings settings;
    return LocalEngineSharing{
        // ADR-0286: with a password the engine is on the network, as melodyd
        // is by itself; there is no switch of its own.
        .share = !settings.value(QLatin1String(SettingsKeys::engine_password_key), QString{})
                      .toString()
                      .isEmpty(),
        .listen = settings
                      .value(QLatin1String(SettingsKeys::engine_listen_key),
                             QString::fromLatin1(SettingsKeys::engine_listen_default))
                      .toString()
                      .trimmed(),
        .stream_port = settings
                           .value(QLatin1String(SettingsKeys::engine_stream_port_key),
                                  SettingsKeys::engine_stream_port_default)
                           .toInt(),
        .password =
            settings.value(QLatin1String(SettingsKeys::engine_password_key), QString{}).toString(),
        .music_root = settings.value(QLatin1String(SettingsKeys::engine_music_root_key), QString{})
                          .toString()
                          .trimmed(),
        .play_for =
            [&settings] {
                std::vector<LocalEngineSharing::PlayFor> targets;
                if (!settings.value(QLatin1String(SettingsKeys::engine_play_for_remote_key), true)
                         .toBool()) {
                    return targets;
                }
                const auto nearby = streamNearbyKbps(settings);
                const auto away = streamAwayKbps(settings);
                for (const auto& engine : loadRemoteEngines()) {
                    // ADR-0239: one choice for this engine, or the two by route.
                    const bool fixed = engine.stream_kbps == 0 ||
                                       (engine.stream_kbps >= 16 && engine.stream_kbps <= 512);
                    targets.push_back({.address = engine.address,
                                       .password = engine.effectivePassword(),
                                       .music_root = engine.reachable_at,
                                       .nearby_kbps = fixed ? engine.stream_kbps : nearby,
                                       .away_kbps = fixed ? engine.stream_kbps : away});
                }
                return targets;
            }(),
        .play_for_found =
            settings.value(QLatin1String(SettingsKeys::engine_play_for_remote_key), true).toBool(),
        .found_nearby_kbps = streamNearbyKbps(settings),
        .found_away_kbps = streamAwayKbps(settings),
        .upnp = MELODY_ENABLE_UPNP &&
                settings.value(QLatin1String(SettingsKeys::engine_upnp_key), false).toBool(),
    };
}

namespace {

// ADR-0286: a section for an engine played for, by its address's host --
// matched by the engine played for as by name (Guests' address match).
[[nodiscard]] std::string section_name(const QString& address) {
    const auto colon = address.lastIndexOf(QLatin1Char(':'));
    return (colon > 0 ? address.left(colon) : address).toStdString();
}

[[nodiscard]] std::string stream_text(const int kbps) {
    return kbps == 0 ? std::string{"original"} : "opus " + std::to_string(kbps);
}

// One request to this computer's engine, on a connection of its own.
[[nodiscard]] core::Result<protocol::Json>
ask(const LocalEngine& engine, const std::string& method,
    const protocol::Json& params = protocol::Json::object()) {
    auto client = protocol::Client::connect(
        protocol::Endpoint{.socket = engine.socket, .host = {}, .port = 0, .token = {}});
    if (!client) {
        return std::unexpected(std::move(client.error()));
    }
    auto answer = (*client)->call(method, params);
    (*client)->close();
    return answer;
}

// `systemctl --user ARGUMENTS`: its exit status and what it printed; -1 when
// there is no systemctl.
struct Ran final {
    int status{-1};
    QString output;
};
[[nodiscard]] Ran systemctl(const QStringList& arguments) {
    QProcess process;
    process.start(QStringLiteral("systemctl"), QStringList{QStringLiteral("--user")} + arguments);
    if (!process.waitForStarted(3'000) || !process.waitForFinished(15'000)) {
        process.kill();
        return {};
    }
    return Ran{.status = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1,
               .output = QString::fromUtf8(process.readAllStandardOutput()).trimmed()};
}

#ifndef __APPLE__
// melodyd.socket is installed and enabled for this user: the package enables
// it for everyone; one disabled or masked is left alone. Asked on Linux only.
[[nodiscard]] bool socket_unit_enabled() {
    // Tests never touch the systemd of whoever runs them.
    if (!qEnvironmentVariableIsEmpty("MELODY_NO_SYSTEMD")) {
        return false;
    }
    const auto enabled =
        systemctl({QStringLiteral("is-enabled"), QStringLiteral("melodyd.socket")});
    return enabled.status == 0 && (enabled.output == QStringLiteral("enabled") ||
                                   enabled.output == QStringLiteral("static") ||
                                   enabled.output == QStringLiteral("enabled-runtime"));
}
#endif

[[nodiscard]] bool wait_until_gone(const std::vector<pid_t>& holders,
                                   const std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    const auto alive = [&holders] { return std::ranges::any_of(holders, processRunning); };
    while (alive() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    return !alive();
}

} // namespace

protocol::Json sharingChanges(const LocalEngineSharing& now, const LocalEngineSharing& before) {
    using protocol::Json;
    auto changes = Json::array();
    const auto put = [&changes](const std::string& key, const std::optional<std::string>& value,
                                const std::string& engine = {}) {
        auto change = Json{{"key", key}, {"value", value ? Json(*value) : Json(nullptr)}};
        if (!engine.empty()) {
            change["engine"] = engine;
        }
        changes.push_back(std::move(change));
    };
    // Shared only with a password: every TCP connection must give one
    // (ADR-0223), and Settings will not save sharing without.
    const bool shared = now.share && !now.listen.isEmpty() && !now.password.isEmpty();
    put("password", shared ? std::optional{now.password.toStdString()} : std::nullopt);
    put("password-file", std::nullopt);
    put("local-only", shared ? std::nullopt : std::optional<std::string>{"yes"});
    put("listen", shared ? std::optional{now.listen.toStdString()} : std::nullopt);
    std::optional<std::string> http;
    if (shared) {
        if (const auto colon = now.listen.lastIndexOf(QLatin1Char(':'));
            colon > 0 && now.stream_port > 0) {
            http = QStringLiteral("%1:%2")
                       .arg(now.listen.left(colon))
                       .arg(now.stream_port)
                       .toStdString();
        }
    } else if (now.upnp) {
        // UPnP renderers fetch from the stream port, shared or not.
        http = QStringLiteral("0.0.0.0:%1").arg(now.stream_port).toStdString();
    }
    put("http", http);
    put("music-root",
        now.music_root.isEmpty() ? std::nullopt : std::optional{now.music_root.toStdString()});
    put("upnp", now.upnp ? std::optional<std::string>{"yes"} : std::nullopt);
    // Played for: every engine found, unless turned off; with a password
    // the engine does so by itself.
    put("agent", now.play_for_found ? std::nullopt : std::optional<std::string>{"no"});
    put("agent-stream-home", now.found_nearby_kbps == SettingsKeys::engine_stream_nearby_default
                                 ? std::nullopt
                                 : std::optional{stream_text(now.found_nearby_kbps)});
    put("agent-stream-away", now.found_away_kbps == SettingsKeys::engine_stream_away_default
                                 ? std::nullopt
                                 : std::optional{stream_text(now.found_away_kbps)});
    // Each engine elsewhere: an [engine HOST] with its address, used while
    // the network does not find it, its mount, its own password and streams.
    std::vector<std::string> kept;
    for (const auto& target : now.play_for) {
        const auto name = section_name(target.address);
        kept.push_back(name);
        put("address", target.address.toStdString(), name);
        put("music-root",
            target.music_root.isEmpty() ? std::nullopt
                                        : std::optional{target.music_root.toStdString()},
            name);
        // Its own password, unless it is the one this engine has: one not
        // shared has none, and gives the remote's as that engine's own.
        put("password",
            target.password.isEmpty() || (shared && target.password == now.password)
                ? std::nullopt
                : std::optional{target.password.toStdString()},
            name);
        put("stream-home",
            target.nearby_kbps == now.found_nearby_kbps
                ? std::nullopt
                : std::optional{stream_text(target.nearby_kbps)},
            name);
        put("stream-away",
            target.away_kbps == now.found_away_kbps ? std::nullopt
                                                    : std::optional{stream_text(target.away_kbps)},
            name);
    }
    for (const auto& target : before.play_for) {
        const auto name = section_name(target.address);
        if (std::ranges::contains(kept, name)) {
            continue;
        }
        for (const auto* key :
             {"address", "music-root", "password", "stream-home", "stream-away"}) {
            put(key, std::nullopt, name);
        }
    }
    return changes;
}

core::Result<void> stopLocalEngine(const LocalEngine& engine) {
    // The engine is whoever holds its lock. Asked to stop for good -- under
    // systemd, not restarted after (ADR-0286) -- else, an engine older than
    // engine.stop, signalled: found through /proc/locks, because flock(2)
    // does not say who holds it.
    const auto holders = lockHolders(engine.state / "engine.lock");
    if (holders.empty()) {
        return {};
    }
    if (!ask(engine, "engine.stop")) {
        for (const auto pid : holders) {
            ::kill(pid, SIGTERM);
        }
    }
    // Either way the queue is saved first; it comes back paused.
    if (!wait_until_gone(holders, std::chrono::seconds{10})) {
        return std::unexpected(launch_error("the running engine did not stop"));
    }
    return {};
}

core::Result<void> restartLocalEngine(const LocalEngine& engine) {
    const auto holders = lockHolders(engine.state / "engine.lock");
    // A service's engine is started again by its service manager; one this
    // computer started (macOS, a test's) is stopped and started here.
    if (localEngineIsService(engine) && ask(engine, "engine.restart")) {
        if (!wait_until_gone(holders, std::chrono::seconds{10})) {
            return std::unexpected(launch_error("the running engine did not restart"));
        }
    } else if (auto stopped = stopLocalEngine(engine); !stopped) {
        return stopped;
    }
    auto started = connectLocalEngine(engine);
    if (!started) {
        return std::unexpected(std::move(started.error()));
    }
    (*started)->close();
    return {};
}

core::Result<void> configureLocalEngine(const LocalEngine& engine, const protocol::Json& changes) {
    if (auto set = ask(engine, "config.set", protocol::Json{{"changes", changes}}); !set) {
        return std::unexpected(std::move(set.error()));
    }
    return restartLocalEngine(engine);
}

bool localEngineIsService(const LocalEngine& engine) {
    const auto info = ask(engine, "engine.info");
    return info && info->value("service", false);
}

bool localEngineWantsService(const LocalEngine& engine) {
#ifdef __APPLE__
    static_cast<void>(engine);
    return false;
#else
    // Cheapest first: under systemd, as it usually is, one question answers.
    return engine.standard && !localEngineIsService(engine) &&
           !lockHolders(engine.state / "engine.lock").empty() && socket_unit_enabled();
#endif
}

core::Result<void> handOverToService(const LocalEngine& engine) {
    if (auto stopped = stopLocalEngine(engine); !stopped) {
        return stopped;
    }
    if (systemctl({QStringLiteral("start"), QStringLiteral("melodyd.socket")}).status != 0) {
        return std::unexpected(
            launch_error("could not start melodyd.socket: systemctl --user start melodyd.socket"));
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
#ifdef __APPLE__
        char executable[PROC_PIDPATHINFO_MAXSIZE]{};
        if (::proc_pidpath(pid, executable, sizeof(executable)) <= 0) {
            continue;
        }
        const std::filesystem::path running{executable};
#else
        const auto running =
            std::filesystem::read_symlink("/proc/" + std::to_string(pid) + "/exe", error);
#endif
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

#ifndef __APPLE__
namespace {

// The threads a process has now, its exited main thread among them.
[[nodiscard]] std::size_t threadCount(const pid_t pid) {
    std::error_code error;
    std::size_t threads = 0U;
    for ([[maybe_unused]] const auto& task :
         std::filesystem::directory_iterator{"/proc/" + std::to_string(pid) + "/task", error}) {
        ++threads;
    }
    return threads;
}

// Its main thread has exited, its other threads perhaps not: "1234 (melodyd)
// Z ...", the state after the command name, which is in parentheses and may
// itself contain them.
[[nodiscard]] bool mainThreadExited(const pid_t pid) {
    std::ifstream stat{"/proc/" + std::to_string(pid) + "/stat"};
    std::string line;
    if (!std::getline(stat, line)) {
        return false;
    }
    const auto name_end = line.rfind(')');
    return name_end != std::string::npos && name_end + 2 < line.size() &&
           line[name_end + 2] == 'Z';
}

} // namespace
#endif

bool processRunning(const pid_t pid) {
    if (::kill(pid, 0) != 0 && errno != EPERM) {
        return false;
    }
#ifdef __APPLE__
    proc_bsdinfo info{};
    if (::proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) ==
        static_cast<int>(sizeof(info))) {
        return info.pbi_status != SZOMB;
    }
    return true;
#else
    if (!mainThreadExited(pid)) {
        return true;
    }
    // A zombie main thread is not a process gone: its other threads may still
    // be running and holding its files -- the engine's socket, still taking
    // connections, and its lock. It is gone once the last of them is.
    return threadCount(pid) > 1;
#endif
}

std::vector<pid_t> lockHolders(const std::filesystem::path& lock) {
    std::vector<pid_t> holders;
    std::error_code error;
    const auto target = std::filesystem::weakly_canonical(lock, error);
    if (error) {
        return holders;
    }
#ifdef __APPLE__
    QProcess lsof;
    lsof.start(QStringLiteral("/usr/sbin/lsof"),
               {QStringLiteral("-t"), QStringLiteral("--"), path_text(target)});
    if (lsof.waitForFinished(3000)) {
        for (const auto& line : lsof.readAllStandardOutput().split('\n')) {
            bool valid = false;
            const int pid = line.toInt(&valid);
            if (valid && pid > 0) {
                holders.push_back(static_cast<pid_t>(pid));
            }
        }
    }
#else
    // flock(2) does not say who holds a lock, but the kernel lists every lock
    // in /proc/locks: "1: FLOCK  ADVISORY  WRITE 1234 00:2a:5678 0 EOF", the
    // holder's pid and the file's device and inode. The inode alone is
    // matched -- on btrfs the device there is not the one stat() gives -- and
    // each process it names is asked whether it has the file open, which
    // settles an inode of the same number elsewhere.
    struct stat locked{};
    if (::stat(target.c_str(), &locked) != 0) {
        return holders;
    }
    const auto inode = ":" + std::to_string(locked.st_ino);
    const auto holds = [&target](const std::filesystem::path& descriptors) {
        std::error_code unreadable;
        for (const auto& descriptor :
             std::filesystem::directory_iterator{descriptors, unreadable}) {
            std::error_code gone;
            if (std::filesystem::read_symlink(descriptor.path(), gone) == target) {
                return true;
            }
        }
        return false;
    };
    std::ifstream locks{"/proc/locks"};
    for (std::string line; std::getline(locks, line);) {
        std::istringstream fields{line};
        std::string number, kind, mode, access, file;
        long pid = 0;
        // A waiter is listed as "1: -> FLOCK ...": not a holder.
        if (!(fields >> number >> kind) || kind == "->" ||
            !(fields >> mode >> access >> pid >> file) || pid <= 0 ||
            !file.ends_with(inode)) {
            continue;
        }
        const auto holder = static_cast<pid_t>(pid);
        if (std::ranges::contains(holders, holder)) {
            continue;
        }
        const std::filesystem::path process{"/proc/" + std::to_string(pid)};
        // A process whose main thread has exited shows no files of its own
        // while its other threads still hold them: looked for there too.
        bool held = holds(process / "fd");
        if (!held && mainThreadExited(holder)) {
            std::error_code unreadable;
            for (const auto& task :
                 std::filesystem::directory_iterator{process / "task", unreadable}) {
                if (holds(task.path() / "fd")) {
                    held = true;
                    break;
                }
            }
        }
        if (held) {
            holders.push_back(holder);
        }
    }
#endif
    return holders;
}

core::Result<std::unique_ptr<protocol::Client>>
connectLocalEngine(const LocalEngine& engine, const std::chrono::milliseconds timeout) {
    const protocol::Endpoint endpoint{.socket = engine.socket, .host = {}, .port = 0, .token = {}};
    if (auto running = protocol::Client::connect(endpoint)) {
        return running;
    }
#ifndef __APPLE__
    // ADR-0286: this user's engine is systemd's, started on the socket's
    // first connection. The socket itself is started if it is enabled and
    // not running yet -- just installed, say -- and otherwise what to do is
    // said.
    if (engine.standard) {
        // Running by systemd's account, yet its socket file gone: an update
        // replaced the unit under it, systemd's engine started without the
        // socket took the path itself, and stopping took the file with it.
        // Started again, the socket is there to start the engine.
        const auto socket_unit = [&engine] {
            if (systemctl({QStringLiteral("start"), QStringLiteral("melodyd.socket")}).status != 0) {
                return false;
            }
            std::error_code unreadable;
            return std::filesystem::exists(engine.socket, unreadable) ||
                   systemctl({QStringLiteral("restart"), QStringLiteral("melodyd.socket")})
                           .status == 0;
        };
        if (socket_unit_enabled() && socket_unit()) {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline) {
                if (auto started = protocol::Client::connect(endpoint)) {
                    return started;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
            }
        }
        return std::unexpected(core::Error{
            .code = core::ErrorCode::not_found,
            .message = "no engine runs on this computer; start it with: systemctl --user "
                       "enable --now melodyd.socket",
            .context = {}});
    }
#endif
    // Started here: on macOS, and an engine of another folder -- a test's,
    // a second workspace's -- with a melodyd.conf of its own beside it.
    const auto program = engineProgram();
    if (program.isEmpty()) {
        return std::unexpected(launch_error("melodyd is not installed"));
    }
    std::error_code ignored;
    std::filesystem::create_directories(engine.state, ignored);
    const auto log = path_text(engine.state / "melodyd.log");
    QProcess process;
    process.setProgram(program);
    QStringList arguments{QStringLiteral("--socket"), path_text(engine.socket),
                          QStringLiteral("--state"), path_text(engine.state)};
    if (!engine.standard) {
        arguments << QStringLiteral("--config") << path_text(engine.state / "melodyd.conf");
    }
    process.setArguments(arguments);
    // Started here, not by a service manager: what this process inherited
    // from one -- a desktop that runs apps as units -- is not the engine's.
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto* name : {"INVOCATION_ID", "JOURNAL_STREAM", "LISTEN_PID", "LISTEN_FDS",
                             "LISTEN_FDNAMES", "NOTIFY_SOCKET"}) {
        environment.remove(QString::fromLatin1(name));
    }
    process.setProcessEnvironment(environment);
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
        // only delay saying so. One that lost a start race exits too, and
        // the winner, holding the lock, answers once it is listening.
        if (pid > 0 && !processRunning(static_cast<pid_t>(pid))) {
            if (auto other = protocol::Client::connect(endpoint)) {
                return other;
            }
            if (lockHolders(engine.state / "engine.lock").empty()) {
                return std::unexpected(
                    launch_error("melodyd stopped at startup; see " + log.toStdString()));
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    return std::unexpected(
        launch_error("melodyd did not start listening; see " + log.toStdString()));
}

} // namespace trackknife::bench
