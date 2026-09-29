// SPDX-License-Identifier: GPL-3.0-only

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QTimer>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("Trackknife Quick mockup"));

    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption screenshot(
        QStringLiteral("screenshot"),
        QStringLiteral("Save the window to <file> once it has rendered, then quit."),
        QStringLiteral("file"));
    parser.addOption(screenshot);
    parser.process(app);

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("Trackknife.Mockup", "Main");

    if (parser.isSet(screenshot)) {
        const QString path = parser.value(screenshot);
        QTimer::singleShot(1500, &app, [&engine, path] {
            auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
            const bool saved = window != nullptr && window->grabWindow().save(path);
            QCoreApplication::exit(saved ? 0 : 1);
        });
    }
    return QGuiApplication::exec();
}
