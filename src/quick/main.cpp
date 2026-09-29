// SPDX-License-Identifier: GPL-3.0-only

// The Trackknife window in Qt Quick (ADR-0220): the same workspace as the
// widgets window, drawn by QML, in the desktop's own style.

#include "bench/engine_launcher.hpp"
#include "quick/image_providers.hpp"
#include "quick/quick_workspace.hpp"
#include "uicommon/debug_log.hpp"
#include "workspace/startup.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>

#include <string>
#include <vector>

namespace {

// The desktop's style, drawn by the desktop's own theme engine (KDE's
// qqc2-desktop-style, through Kvantum or Breeze), so the window looks like
// the widgets one beside it; Fusion where that is not installed.
void chooseStyle(const QQmlApplicationEngine& qml) {
    if (!qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        return;
    }
    for (const auto& path : qml.importPathList()) {
        if (QFile::exists(path + QStringLiteral("/org/kde/desktop/qmldir"))) {
            QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
            QQuickStyle::setFallbackStyle(QStringLiteral("Fusion"));
            return;
        }
    }
    QQuickStyle::setStyle(QStringLiteral("Fusion"));
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    // QA hook: --screenshot renders against test data only, decided before
    // anything reads settings or the workspace.
    const bool screenshot_run = QApplication::arguments().contains(QStringLiteral("--screenshot"));
    if (screenshot_run) {
        QStandardPaths::setTestModeEnabled(true);
    }
    QApplication::setOrganizationName(QStringLiteral("trackknife"));
    QApplication::setApplicationName(QStringLiteral("trackknife"));
    QApplication::setApplicationDisplayName(QStringLiteral("Trackknife"));
    trackknife::bench::adoptInterimIdentity();
    const auto restore_notice = trackknife::bench::applyPendingWorkspaceRestore();

    QString screenshot_path;
    std::vector<std::string> raw_paths;
    const auto arguments = QApplication::arguments();
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        if (arguments.at(index) == QStringLiteral("--screenshot") && index + 1 < arguments.size()) {
            screenshot_path = arguments.at(++index);
            continue;
        }
        if (arguments.at(index) == QStringLiteral("--debug")) {
            trackknife::ui::enableDebugLogging();
            continue;
        }
        const auto encoded = QFile::encodeName(arguments.at(index));
        raw_paths.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
    }
    // ADR-0226: only the application starts an engine, and not when it is
    // taking screenshots against test data.
    trackknife::bench::allowLocalEngine(screenshot_path.isEmpty());

    trackknife::quick::QuickWorkspace workspace(&application);
    trackknife::quick::QuickWorkspace::setInstance(&workspace);

    QQmlApplicationEngine qml;
    chooseStyle(qml);
    qml.addImageProvider(QStringLiteral("cover"), new trackknife::quick::CoverProvider(workspace));
    qml.addImageProvider(QStringLiteral("icon"), new trackknife::quick::IconProvider());
    QObject::connect(
        &qml, &QQmlApplicationEngine::objectCreationFailed, &application,
        [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    QObject::connect(&workspace, &trackknife::quick::QuickWorkspace::quitRequested, &application,
                     &QCoreApplication::quit, Qt::QueuedConnection);
    qml.loadFromModule("Trackknife.Quick", "Main");
    workspace.start();
    if (!restore_notice.isEmpty()) {
        QTimer::singleShot(0, &workspace, [&workspace, restore_notice] {
            emit workspace.information(QStringLiteral("Workspace restore"), restore_notice);
        });
    }
    if (!raw_paths.empty()) {
        workspace.workspace().openLocalPaths(std::move(raw_paths));
    }
    if (!screenshot_path.isEmpty()) {
        QTimer::singleShot(3'000, &application, [&qml, screenshot_path] {
            auto* window = qobject_cast<QQuickWindow*>(qml.rootObjects().value(0));
            const bool saved = window != nullptr && window->grabWindow().save(screenshot_path);
            QApplication::exit(saved ? 0 : 1);
        });
    }
    return QApplication::exec();
}
