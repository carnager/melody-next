// SPDX-License-Identifier: GPL-3.0-only

// ADR-0302: listening together, from a link -- an invite opened in a browser
// hands melody://together/join?invite=… to Melody, which asks its engine to
// join the room.

#include "bench/bench_main_window.hpp"

#include "bench/animated_panel_dock.hpp"
#include "bench/desktop_notifier.hpp"
#include "workspace/plugins.hpp"

#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <memory>

namespace trackknife::bench {

bool BenchMainWindow::openLink(const QString& link) {
    const QUrl url{link};
    if (url.scheme() != QStringLiteral("melody")) {
        return false;
    }
    // A vote at a listening party, or -- the host -- a suggestion removed.
    if (url.host() == QStringLiteral("together") &&
        (url.path() == QStringLiteral("/vote") || url.path() == QStringLiteral("/remove"))) {
        bool number = false;
        const auto id = QUrlQuery{url}.queryItemValue(QStringLiteral("id")).toULongLong(&number);
        auto* engine = transport_;
        if (!number || engine == nullptr || !engine->active()) {
            return true;
        }
        const bool voting = url.path() == QStringLiteral("/vote");
        engine->request(
            voting ? QStringLiteral("together.vote") : QStringLiteral("together.remove"),
            protocol::Json{{"id", id}}, [this, voting](const core::Result<protocol::Json>& done) {
                showMessage(
                    done ? (voting ? QStringLiteral("Voted") : QStringLiteral("Suggestion removed"))
                         : QString::fromStdString(done.error().message),
                    5'000);
            });
        return true;
    }
    if (url.host() != QStringLiteral("together") || url.path() != QStringLiteral("/join")) {
        showMessage(QStringLiteral("Melody does not know what to do with %1").arg(link), 10'000);
        return true;
    }
    const auto invite =
        QUrlQuery{url}.queryItemValue(QStringLiteral("invite"), QUrl::FullyDecoded).trimmed();
    if (invite.isEmpty()) {
        showMessage(QStringLiteral("The link has no invite in it: ask for it again"), 10'000);
        return true;
    }
    // The name the Listen together plugin is told to use, else the login's.
    auto name = QSettings{}
                    .value(pluginSettingKey(QStringLiteral("together"), QStringLiteral("name")))
                    .toString()
                    .trimmed();
    if (name.isEmpty()) {
        name = qEnvironmentVariable("USER", QStringLiteral("a friend"));
    }
    auto* engine = transport_;
    if (engine == nullptr) {
        showMessage(QStringLiteral("No engine to listen with"), 10'000);
        return true;
    }
    // Just started from the link, the engine is still being reached: asked
    // once it is.
    if (!engine->active()) {
        showMessage(QStringLiteral("Joining the listening room once the engine is reached…"), 0);
        auto waiting = std::make_shared<QMetaObject::Connection>();
        *waiting = connect(engine, &EnginePlayback::connected, this, [this, link, waiting] {
            disconnect(*waiting);
            openLink(link);
        });
        return true;
    }
    showMessage(QStringLiteral("Joining the listening room…"), 0);
    engine->request(QStringLiteral("together.join"),
                    protocol::Json{{"invite", invite.toStdString()}, {"name", name.toStdString()}},
                    [this](const core::Result<protocol::Json>& joined) {
                        const auto said =
                            joined ? QStringLiteral("Listening along with your friends")
                                   : QStringLiteral("Could not join the listening room: %1")
                                         .arg(QString::fromStdString(joined.error().message));
                        showMessage(said, 10'000);
                        if (notifier_ != nullptr) {
                            notifier_->announce(QStringLiteral("Listen together"), said);
                        }
                    });
    return true;
}

void BenchMainWindow::listeningRoomChanged() {
    if (room_panels_timer_ == nullptr) {
        room_panels_timer_ = new QTimer(this);
        room_panels_timer_->setSingleShot(true);
        room_panels_timer_->setInterval(500);
        connect(room_panels_timer_, &QTimer::timeout, this, [this] {
            for (auto& slot : plugin_panels_) {
                if (slot->panel.refresh_room && !slot->dock->isHidden()) {
                    askPluginPanel(*slot);
                }
            }
        });
    }
    if (!room_panels_timer_->isActive()) {
        room_panels_timer_->start();
    }
}

} // namespace trackknife::bench
