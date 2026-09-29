// SPDX-License-Identifier: GPL-3.0-only

#include "bench/engine_launcher.hpp"
#include "quick/cover_provider.hpp"
#include "quick/engine.hpp"

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QTimer>

int main(int argc, char* argv[]) {
    QGuiApplication application(argc, argv);
    // The widgets window's names: they decide where this computer's engine
    // keeps its state and socket, and this window must find the same one.
    QGuiApplication::setOrganizationName(QStringLiteral("trackknife"));
    QGuiApplication::setApplicationName(QStringLiteral("trackknife"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Trackknife"));

    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption screenshot(QStringLiteral("screenshot"),
                                        QStringLiteral("Save the window to <file> after <delay> ms, then quit."),
                                        QStringLiteral("file"));
    const QCommandLineOption delay(QStringLiteral("delay"), QStringLiteral("Milliseconds before --screenshot."),
                                   QStringLiteral("ms"), QStringLiteral("4000"));
    parser.addOption(screenshot);
    parser.addOption(delay);
    parser.process(application);

    // Crisper at the small sizes a track list uses than the default
    // distance-field text, and closer to how the rest of the desktop draws.
    QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);

    trackknife::bench::allowLocalEngine(true);
    trackknife::quick::Engine engine;

    QQmlApplicationEngine qml;
    qml.addImageProvider(QStringLiteral("cover"), new trackknife::quick::CoverProvider(
                                                      [&engine](const int index) { return engine.clientAt(index); }));
    QObject::connect(
        &qml, &QQmlApplicationEngine::objectCreationFailed, &application, [] { QCoreApplication::exit(1); },
        Qt::QueuedConnection);
    qml.loadFromModule("Trackknife.Quick", "Main");

    if (parser.isSet(screenshot)) {
        const auto path = parser.value(screenshot);
        QTimer::singleShot(parser.value(delay).toInt(), &application, [&qml, path] {
            auto* window = qobject_cast<QQuickWindow*>(qml.rootObjects().value(0));
            const bool saved = window != nullptr && window->grabWindow().save(path);
            QCoreApplication::exit(saved ? 0 : 1);
        });
    }
    return QGuiApplication::exec();
}
