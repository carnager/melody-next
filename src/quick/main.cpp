// SPDX-License-Identifier: GPL-3.0-only

// The Trackknife window in Qt Quick (ADR-0220): the same workspace as the
// widgets window, drawn by QML, in the desktop's own style.

#include "bench/engine_launcher.hpp"
#include "quick/image_providers.hpp"
#include "quick/quick_artwork.hpp"
#include "quick/quick_workspace.hpp"
#include "uicommon/debug_log.hpp"
#include "workspace/startup.hpp"

#include <QApplication>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMouseEvent>
#include <QPalette>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>
#include <QtQml/QQmlExtensionPlugin>

#include <string>
#include <vector>

Q_IMPORT_QML_PLUGIN(Trackknife_StylePlugin)

namespace {

// The Trackknife style on every system (ADR-0240): the same flat controls on
// Linux, macOS and Windows, in the system's colours; Fusion under it for
// what it does not draw. QT_QUICK_CONTROLS_STYLE still chooses another.
void chooseStyle() {
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        QQuickStyle::setStyle(QStringLiteral("Trackknife.Style"));
        QQuickStyle::setFallbackStyle(QStringLiteral("Fusion"));
    }
}

// QA hook (--dark): a dark palette like the desktop's, set on a window so
// every control in it inherits it over its type's own.
void darken(QObject* window) {
    auto* palette = window->property("palette").value<QObject*>();
    if (palette == nullptr) {
        return;
    }
    const std::pair<const char*, QColor> roles[] = {
        {"window", QColor(0x2a, 0x2e, 0x32)},
        {"windowText", QColor(0xfc, 0xfc, 0xfc)},
        {"base", QColor(0x1b, 0x1e, 0x20)},
        {"alternateBase", QColor(0x23, 0x26, 0x29)},
        {"text", QColor(0xfc, 0xfc, 0xfc)},
        {"button", QColor(0x29, 0x2c, 0x30)},
        {"buttonText", QColor(0xfc, 0xfc, 0xfc)},
        {"highlight", QColor(0x3d, 0xae, 0xe9)},
        {"highlightedText", QColor(0xfc, 0xfc, 0xfc)},
        {"placeholderText", QColor(0xa1, 0xa9, 0xb1)},
        {"toolTipBase", QColor(0x31, 0x36, 0x3b)},
        {"toolTipText", QColor(0xfc, 0xfc, 0xfc)},
        {"light", QColor(0x40, 0x46, 0x4c)},
        {"mid", QColor(0x1e, 0x21, 0x24)},
        {"dark", QColor(0x14, 0x16, 0x18)},
    };
    for (const auto& [role, color] : roles) {
        palette->setProperty(role, color);
    }
}

// The same for what reads the application's palette: a model's colours.
void darkenApplication() {
    QPalette dark;
    dark.setColor(QPalette::Window, QColor(0x2a, 0x2e, 0x32));
    dark.setColor(QPalette::WindowText, QColor(0xfc, 0xfc, 0xfc));
    dark.setColor(QPalette::Base, QColor(0x1b, 0x1e, 0x20));
    dark.setColor(QPalette::AlternateBase, QColor(0x23, 0x26, 0x29));
    dark.setColor(QPalette::Text, QColor(0xfc, 0xfc, 0xfc));
    dark.setColor(QPalette::Button, QColor(0x29, 0x2c, 0x30));
    dark.setColor(QPalette::ButtonText, QColor(0xfc, 0xfc, 0xfc));
    dark.setColor(QPalette::Highlight, QColor(0x3d, 0xae, 0xe9));
    dark.setColor(QPalette::HighlightedText, QColor(0xfc, 0xfc, 0xfc));
    dark.setColor(QPalette::PlaceholderText, QColor(0xa1, 0xa9, 0xb1));
    QApplication::setPalette(dark);
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
    bool grab_live = false;
    QString open_for_screenshot;
    bool dark_palette = false;
    // QA hook: --click <x>,<y> (repeatable) -- left clicks, in order, once
    // what was opened is shown.
    QList<QPoint> clicks;
    std::vector<std::string> raw_paths;
    const auto arguments = QApplication::arguments();
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        if (arguments.at(index) == QStringLiteral("--screenshot") && index + 1 < arguments.size()) {
            screenshot_path = arguments.at(++index);
            continue;
        }
        // QA hook: --grab <file> is --screenshot on the real settings and
        // engine -- for a sandbox whose XDG directories are its own.
        if (arguments.at(index) == QStringLiteral("--grab") && index + 1 < arguments.size()) {
            screenshot_path = arguments.at(++index);
            grab_live = true;
            continue;
        }
        // QA hook: what to open before the screenshot -- a menu or dialog the
        // window names (Main.qml's openForScreenshot).
        if (arguments.at(index) == QStringLiteral("--open") && index + 1 < arguments.size()) {
            open_for_screenshot = arguments.at(++index);
            continue;
        }
        if (arguments.at(index) == QStringLiteral("--click") && index + 1 < arguments.size()) {
            const auto at = arguments.at(++index).split(QLatin1Char(','));
            if (at.size() == 2) {
                clicks.append(QPoint{at.at(0).toInt(), at.at(1).toInt()});
            }
            continue;
        }
        // QA hook: --dark draws in a dark palette like the desktop's, to see
        // the window as a dark desktop shows it.
        if (arguments.at(index) == QStringLiteral("--dark")) {
            dark_palette = true;
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
    trackknife::bench::allowLocalEngine(screenshot_path.isEmpty() || grab_live);

    trackknife::quick::QuickWorkspace workspace(&application);
    trackknife::quick::QuickWorkspace::setInstance(&workspace);

    QQmlApplicationEngine qml;
    chooseStyle();
    qml.addImageProvider(QStringLiteral("cover"), new trackknife::quick::CoverProvider(workspace));
    qml.addImageProvider(QStringLiteral("icon"), new trackknife::quick::IconProvider());
    qml.addImageProvider(QStringLiteral("artwork"), new trackknife::quick::ArtworkImageProvider());
    QObject::connect(
        &qml, &QQmlApplicationEngine::objectCreationFailed, &application,
        [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    QObject::connect(&workspace, &trackknife::quick::QuickWorkspace::quitRequested, &application,
                     &QCoreApplication::quit, Qt::QueuedConnection);
    qml.loadFromModule("Trackknife.Quick", "Main");
    if (dark_palette) {
        darkenApplication();
        // Every window, the main one and each opened later.
        auto* sweep = new QTimer(&application);
        QObject::connect(sweep, &QTimer::timeout, &application, [] {
            for (auto* window : QGuiApplication::topLevelWindows()) {
                if (!window->property("trackknifeDark").toBool()) {
                    window->setProperty("trackknifeDark", true);
                    darken(window);
                }
            }
        });
        sweep->start(100);
        if (auto* root = qml.rootObjects().value(0)) {
            darken(root);
        }
    }
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
        if (!open_for_screenshot.isEmpty()) {
            QTimer::singleShot(2'000, &application, [&qml, open_for_screenshot] {
                if (auto* root = qml.rootObjects().value(0)) {
                    QMetaObject::invokeMethod(root, "openForScreenshot",
                                              Q_ARG(QVariant, open_for_screenshot));
                }
            });
        }
        for (qsizetype click = 0; click < clicks.size(); ++click) {
            QTimer::singleShot(4'000 + static_cast<int>(click) * 500, &application,
                               [&qml, at = clicks.at(click)] {
                auto* window = qobject_cast<QQuickWindow*>(qml.rootObjects().value(0));
                if (window == nullptr) {
                    return;
                }
                const QPointF local{at};
                const auto global = window->mapToGlobal(local);
                const auto stamp = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
                // Pointed at first, as a hand would: what shows on hover shows,
                // where the pointer is.
                QCursor::setPos(global.toPoint());
                QMouseEvent move{QEvent::MouseMove, local, global,
                                 Qt::NoButton, Qt::NoButton, Qt::NoModifier};
                move.setTimestamp(stamp);
                QCoreApplication::sendEvent(window, &move);
                QMouseEvent press{QEvent::MouseButtonPress, local, global,
                                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier};
                press.setTimestamp(stamp + 20);
                QCoreApplication::sendEvent(window, &press);
                // Released a moment later, as a click is.
                QTimer::singleShot(60, window, [window, local, global, stamp] {
                    QMouseEvent release{QEvent::MouseButtonRelease, local, global,
                                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier};
                    release.setTimestamp(stamp + 80);
                    QCoreApplication::sendEvent(window, &release);
                });
            });
        }
        const int grab_at = (grab_live ? 6'000 : 3'000) + static_cast<int>(clicks.size()) * 500;
        QTimer::singleShot(grab_at, &application, [&qml, screenshot_path] {
            // The window opened last -- a tag editor, say -- else the main one.
            auto* window = qobject_cast<QQuickWindow*>(qml.rootObjects().value(0));
            for (bool deeper = true; deeper;) {
                deeper = false;
                for (auto* candidate : QGuiApplication::topLevelWindows()) {
                    if (auto* quick = qobject_cast<QQuickWindow*>(candidate);
                        quick != nullptr && quick != window && quick->isVisible() &&
                        quick->transientParent() == window) {
                        window = quick;
                        deeper = true;
                        break;
                    }
                }
            }
            const bool saved = window != nullptr && window->grabWindow().save(screenshot_path);
            QApplication::exit(saved ? 0 : 1);
        });
    }
    return QApplication::exec();
}
