// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"
#include "bench/engine_launcher.hpp"
#include "trackknife/persistence/workspace_backup.hpp"
#include "uicommon/debug_log.hpp"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QTimer>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace {

QtMessageHandler default_message_handler = nullptr;

// Qt's Wayland backend logs a mouse-grab complaint on every ordinary
// menu-bar interaction while a menu is open (upstream QTBUG-87303 family);
// the navigation itself works, so the known-noise line is dropped and
// everything else reaches the default handler untouched.
void filtered_message_handler(const QtMsgType type, const QMessageLogContext& context,
                              const QString& message) {
    if (message ==
        QLatin1String("This plugin supports grabbing the mouse only for popup windows")) {
        return;
    }
    if (default_message_handler != nullptr) {
        default_message_handler(type, context, message);
    }
}

} // namespace

// QA soak hook: TRACKKNIFE_SOAK_LOG=<path> appends one line per minute —
// timestamp, resident memory, live QObject/widget counts, and event-loop
// lateness — so gradual degradation over long sessions becomes measurable
// instead of anecdotal.
namespace {
void startSoakLog(QObject* parent) {
    const auto path = qEnvironmentVariable("TRACKKNIFE_SOAK_LOG");
    if (path.isEmpty()) {
        return;
    }
    auto* timer = new QTimer(parent);
    timer->setInterval(60'000);
    auto* lateness = new qint64{0};
    QObject::connect(timer, &QTimer::destroyed, parent, [lateness] { delete lateness; });
    auto* expected = new QElapsedTimer{};
    QObject::connect(timer, &QTimer::destroyed, parent, [expected] { delete expected; });
    expected->start();
    QObject::connect(timer, &QTimer::timeout, parent, [path, timer, lateness, expected] {
        // How late the timer fired is a direct sample of event-loop
        // congestion — the thing a user feels as sluggish tab switches.
        *lateness = expected->elapsed() - timer->interval();
        expected->restart();
        long long resident_pages = 0;
        if (QFile statm{QStringLiteral("/proc/self/statm")}; statm.open(QIODevice::ReadOnly)) {
            const auto fields = QString::fromLatin1(statm.readAll()).split(QLatin1Char(' '));
            if (fields.size() > 1) {
                resident_pages = fields[1].toLongLong();
            }
        }
        std::size_t object_count = 0;
        const auto widgets = QApplication::allWidgets();
        for (const auto* widget : widgets) {
            object_count += static_cast<std::size_t>(widget->children().size());
        }
        QFile log{path};
        if (log.open(QIODevice::Append | QIODevice::Text)) {
            log.write(QStringLiteral("%1 rss_kb=%2 widgets=%3 child_objects=%4 late_ms=%5\n")
                          .arg(QDateTime::currentDateTime().toString(Qt::ISODate))
                          .arg(resident_pages * 4)
                          .arg(widgets.size())
                          .arg(object_count)
                          .arg(*lateness)
                          .toUtf8());
        }
    });
    timer->start();
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    default_message_handler = qInstallMessageHandler(filtered_message_handler);
    QApplication::setOrganizationName(QStringLiteral("trackknife"));
    QApplication::setApplicationName(QStringLiteral("trackknife"));
    QApplication::setApplicationDisplayName(QStringLiteral("Trackknife"));

    // The application reclaimed its original name; adopt settings and the
    // workspace database written under the interim "trackbench" identity
    // exactly once, before anything opens them.
    {
        const auto new_settings = QSettings{}.fileName();
        const auto old_settings =
            QFileInfo{new_settings}.dir().filePath(QStringLiteral("trackbench.conf"));
        if (!QFile::exists(new_settings) && QFile::exists(old_settings)) {
            QFile::rename(old_settings, new_settings);
        }
        const auto new_data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const auto old_data = QFileInfo{new_data}.dir().filePath(QStringLiteral("trackbench"));
        if (!QDir{new_data}.exists() && QDir{old_data}.exists()) {
            QDir{}.rename(old_data, new_data);
        }
    }

    QString restore_notice;
    {
        QSettings settings;
        const auto pending =
            settings.value(QStringLiteral("recovery/pending-workspace-restore")).toString();
        const auto pending_settings =
            settings.value(QStringLiteral("recovery/pending-settings-restore")).toString();
        if (!pending.isEmpty()) {
            const auto data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
            QDir{}.mkpath(data);
            const auto live = std::filesystem::path{
                QFile::encodeName(data + QStringLiteral("/lists.sqlite")).toStdString()};
            const auto rollback_name =
                QStringLiteral("/lists-before-restore-%1.sqlite")
                    .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss")));
            const auto rollback =
                std::filesystem::path{QFile::encodeName(data + rollback_name).toStdString()};
            const auto backup = std::filesystem::path{QFile::encodeName(pending).toStdString()};
            auto restored =
                trackknife::persistence::restore_workspace_database_backup(backup, live, rollback);
            if (restored) {
                QString settings_error;
                if (!pending_settings.isEmpty()) {
                    QSettings imported{pending_settings, QSettings::IniFormat};
                    if (imported.value(QStringLiteral("backup/format")).toInt() != 1 ||
                        imported.status() != QSettings::NoError) {
                        settings_error = QStringLiteral("; settings backup was invalid");
                    } else {
                        settings.clear();
                        for (const auto& key : imported.allKeys()) {
                            if (key.startsWith(QStringLiteral("values/"))) {
                                settings.setValue(key.sliced(7), imported.value(key));
                            }
                        }
                    }
                }
                settings.remove(QStringLiteral("recovery/pending-workspace-restore"));
                settings.remove(QStringLiteral("recovery/pending-settings-restore"));
                settings.sync();
                restore_notice =
                    QStringLiteral("Workspace restored. Previous database: %1%2")
                        .arg(QFile::decodeName(QByteArray::fromStdString(rollback.native())),
                             settings_error);
            } else {
                restore_notice = QStringLiteral("Workspace restore failed: %1")
                                     .arg(QString::fromStdString(restored.error().message));
            }
        }
    }

    // QA hook: --screenshot <file.png> renders the workspace, grabs it once
    // background probing has had a moment, and exits. It switches to the
    // test-mode settings location so real user state stays untouched.
    QString screenshot_path;
    std::vector<std::string> raw_paths;
    const auto arguments = QApplication::arguments();
    raw_paths.reserve(static_cast<std::size_t>(arguments.size()));
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        if (arguments.at(index) == QStringLiteral("--screenshot") && index + 1 < arguments.size()) {
            screenshot_path = arguments.at(++index);
            continue;
        }
        // --debug traces server commands, context switches and what the
        // workspace restored, on stderr. Run it from a terminal when
        // something looks wrong after a restart.
        if (arguments.at(index) == QStringLiteral("--debug")) {
            trackknife::ui::enableDebugLogging();
            continue;
        }
        const auto encoded = QFile::encodeName(arguments.at(index));
        raw_paths.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
    }
    if (!screenshot_path.isEmpty()) {
        QStandardPaths::setTestModeEnabled(true);
    }

    // ADR-0226: only the application starts an engine, and not when it is
    // taking screenshots against test data.
    trackknife::bench::allowLocalEngine(screenshot_path.isEmpty());
    startSoakLog(&application);
    trackknife::bench::BenchMainWindow window;
    window.show();
    if (!restore_notice.isEmpty()) {
        QTimer::singleShot(0, &window, [&window, restore_notice] {
            QMessageBox::information(&window, QStringLiteral("Workspace restore"), restore_notice);
        });
    }
    if (!raw_paths.empty()) {
        window.openLocalPaths(std::move(raw_paths));
    }
    if (!screenshot_path.isEmpty()) {
        QTimer::singleShot(3'000, &application, [&window, screenshot_path] {
            const auto image = window.grab().toImage();
            const auto saved = image.save(screenshot_path);
            QApplication::exit(saved ? 0 : 1);
        });
    }

    return QApplication::exec();
}
