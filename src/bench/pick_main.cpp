// SPDX-License-Identifier: GPL-3.0-only

// melody-pick (ADR-0346): Quick album and Quick track on their own, for a
// key binding -- the window's picker, looking as it does there, over any
// engine melody-cli would talk to. What is picked goes to that engine's
// queue or Up Next, as melody-cli would put it; the window, open or not,
// shows it there.

#include "bench/quick_pick_popup.hpp"
#include "bench/widget_color_scheme.hpp"
#include "engine_access.hpp"
#include "trackknife/core/environment.hpp"
#include "trackknife/engine/remote_catalogue.hpp"
#include "workspace/interface_scale.hpp"

#include <QApplication>
#include <QIcon>
#include <QMessageBox>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace {

using trackknife::bench::LocalLibraryAction;
using trackknife::bench::QuickPickKind;
using trackknife::cli::Placement;

constexpr std::string_view usage =
    "usage: melody-pick [albums|tracks] [--server HOST:PORT|SOCKET] [--engine NAME]\n"
    "                   [--local] [--password PASSWORD] [--password-file FILE]\n"
    "\n"
    "Quick album or Quick track in a window of its own. The engine is the one\n"
    "melody-cli would use: --server ($MELODY_SERVER), --engine for one found\n"
    "nearby, --local for this computer's; without any, this computer's engine\n"
    "when it runs, else the one nearby. The password: --password,\n"
    "--password-file, $MELODY_PASSWORD or $MELODY_PASSWORD_FILE.\n"
    "\n"
    "Enter adds to what is queued, Shift+Enter replaces it and plays, Ctrl+Enter\n"
    "plays next, Ctrl+Shift+Enter adds to Up Next; Escape leaves.\n";

struct Request final {
    QuickPickKind kind{QuickPickKind::album};
    trackknife::cli::EngineChoice choice;
};

// What was asked for, or nothing when the program is done: help printed, or
// a mistake said.
[[nodiscard]] std::optional<Request> parse(const QStringList& arguments, int& status) {
    Request request;
    std::string password_file;
    if (const char* server = trackknife::core::environment_variable("MELODY_SERVER");
        server != nullptr) {
        request.choice.server = server;
    }
    if (const char* password = trackknife::core::environment_variable("MELODY_PASSWORD");
        password != nullptr) {
        request.choice.password = password;
    } else if (const char* file = trackknife::core::environment_variable("MELODY_PASSWORD_FILE");
               file != nullptr) {
        password_file = file;
    }
    bool server_given = false;
    const auto fail = [&status](const std::string& message) {
        std::cerr << "melody-pick: " << message << "\n";
        status = EXIT_FAILURE;
        return std::nullopt;
    };
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        const auto argument = arguments.at(index).toStdString();
        const auto value = [&]() -> std::optional<std::string> {
            if (index + 1 >= arguments.size()) {
                return std::nullopt;
            }
            return arguments.at(++index).toStdString();
        };
        if (argument == "albums" || argument == "album") {
            request.kind = QuickPickKind::album;
        } else if (argument == "tracks" || argument == "track") {
            request.kind = QuickPickKind::track;
        } else if (argument == "--local") {
            request.choice.local = true;
        } else if (argument == "--server" || argument == "--engine" || argument == "--password" ||
                   argument == "--password-file") {
            auto given = value();
            if (!given) {
                return fail(argument + " wants a value");
            }
            if (argument == "--server") {
                request.choice.server = std::move(*given);
                server_given = true;
            } else if (argument == "--engine") {
                request.choice.engine = std::move(*given);
            } else if (argument == "--password") {
                request.choice.password = std::move(*given);
                password_file.clear();
            } else {
                password_file = std::move(*given);
            }
        } else if (argument == "--help" || argument == "-h") {
            std::cout << usage;
            status = EXIT_SUCCESS;
            return std::nullopt;
        } else {
            return fail("unrecognised argument " + argument + "\n" + std::string{usage});
        }
    }
    // $MELODY_SERVER is the default --local sets aside, as melody-cli's is;
    // --server or --engine given as well is asking for two engines.
    if (request.choice.local && (server_given || !request.choice.engine.empty())) {
        return fail("--local, --server or --engine: one of them");
    }
    if (!password_file.empty()) {
        auto read = trackknife::protocol::read_password_file(password_file);
        if (!read) {
            return fail(read.error().message);
        }
        request.choice.password = std::move(*read);
    }
    return request;
}

[[nodiscard]] std::optional<Placement> placementFor(const LocalLibraryAction action) {
    switch (action) {
    case LocalLibraryAction::append:
        return Placement::add;
    case LocalLibraryAction::replace:
        return Placement::play;
    case LocalLibraryAction::request_next:
        return Placement::insert;
    case LocalLibraryAction::request_end:
        return Placement::upnext;
    case LocalLibraryAction::next:
    case LocalLibraryAction::new_list:
        break;
    }
    return std::nullopt;
}

int run(int argc, char** argv) {
    // ADR-0251: the window's size, so the picker matches it.
    trackknife::bench::applyInterfaceScale(argc, argv);
    QApplication application(argc, argv);
    // The window's settings, read for its colours; nothing is written.
    QApplication::setOrganizationName(QStringLiteral("melody"));
    QApplication::setApplicationName(QStringLiteral("melody"));
    QApplication::setApplicationDisplayName(QStringLiteral("Melody"));
    QApplication::setWindowIcon(
        QIcon::fromTheme(QStringLiteral("melody"), QIcon{QStringLiteral(":/melody/melody.svg")}));
    QApplication::setDesktopFileName(QStringLiteral("melody-pick"));

    int status = EXIT_SUCCESS;
    auto request = parse(QApplication::arguments(), status);
    if (!request) {
        return status;
    }
    trackknife::bench::followColorSchemes();

    std::string notice;
    auto connected = trackknife::cli::connect_engine(request->choice, "melody-pick", notice);
    if (!connected) {
        const auto said = QString::fromStdString(connected.error().message);
        std::cerr << "melody-pick: " << connected.error().message << "\n";
        QMessageBox::warning(nullptr, QStringLiteral("Melody"), said);
        return EXIT_FAILURE;
    }
    if (!notice.empty()) {
        std::cerr << "melody-pick: " << notice << "\n";
    }
    const std::shared_ptr<trackknife::protocol::Client> client = std::move(*connected);
    // The engine's name beside the words, as the window shows the library's.
    QString scope;
    if (auto info = client->call("engine.info"); info && info->is_object()) {
        scope = QString::fromStdString(info->value("name", std::string{}));
    }

    auto* picker = new trackknife::bench::QuickPickPopup(
        request->kind, std::make_shared<trackknife::engine::RemoteCatalogue>(client), scope,
        nullptr, true);
    const bool albums = request->kind == QuickPickKind::album;
    QObject::connect(
        picker, &trackknife::bench::QuickPickPopup::chosen, picker,
        [picker, client, albums](std::vector<trackknife::persistence::LibraryEntry> picked,
                                 const LocalLibraryAction action) {
            const auto placement = placementFor(action);
            if (picked.empty() || !placement) {
                return;
            }
            picker->say(QObject::tr("Queuing…"));
            const QPointer<trackknife::bench::QuickPickPopup> alive{picker};
            // Off the window's thread: an engine over the network takes a
            // moment.
            auto* watcher = new QFutureWatcher<QString>(picker);
            QObject::connect(watcher, &QFutureWatcher<QString>::finished, picker, [watcher, alive] {
                const auto failed = watcher->result();
                watcher->deleteLater();
                if (!alive) {
                    return;
                }
                if (failed.isEmpty()) {
                    alive->close();
                } else {
                    alive->say(failed);
                }
            });
            watcher->setFuture(QtConcurrent::run(
                [client, albums, key = picked.front().key, placement = *placement]() -> QString {
                    auto tracks = trackknife::cli::picked_tracks(*client, albums, key);
                    if (!tracks) {
                        return QString::fromStdString(tracks.error().message);
                    }
                    auto placed = trackknife::cli::place(*client, placement,
                                                         trackknife::cli::queue_entries(*tracks));
                    return placed ? QString{} : QString::fromStdString(placed.error().message);
                }));
        });
    picker->showAlone();
    return QApplication::exec();
}

} // namespace

int main(int argc, char** argv) {
    // Anything thrown is said, and the program fails, rather than ending in
    // terminate().
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "melody-pick: " << error.what() << "\n";
    } catch (...) {
        std::cerr << "melody-pick: an unknown error\n";
    }
    return EXIT_FAILURE;
}
