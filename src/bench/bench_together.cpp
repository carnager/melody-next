// SPDX-License-Identifier: GPL-3.0-only

// ADR-0302: listening together, from a link -- an invite opened in a browser
// hands melody://together/join?invite=… to Melody, which asks its engine to
// join the room.

#include "bench/bench_main_window.hpp"

#include "bench/desktop_notifier.hpp"
#include "workspace/plugins.hpp"

#include <QSettings>
#include <QUrl>
#include <QUrlQuery>

#include <memory>

namespace trackknife::bench {

bool BenchMainWindow::openLink(const QString& link) {
    const QUrl url{link};
    if (url.scheme() != QStringLiteral("melody")) {
        return false;
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

} // namespace trackknife::bench
