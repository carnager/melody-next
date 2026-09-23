// SPDX-License-Identifier: GPL-3.0-only
#include "bench/bench_main_window.hpp"
#include "bench/lastfm_service.hpp"
#include "trackknife/audio/local_audition.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QFormLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableView>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

namespace trackknife::bench {
void BenchMainWindow::buildLastFm() {
    lastfm_ = new LastFmService(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                                    QStringLiteral("/lastfm-v1.json"),
                                this);
    lastfm_clock_.start();
    auto feedback = [this](const QString& op, const QJsonObject&, const QString& error) {
        if (!error.isEmpty() && op != QStringLiteral("info") && op != QStringLiteral("status"))
            statusBar()->showMessage(error, 7000);
        else if (op == QStringLiteral("love") || op == QStringLiteral("unlove"))
            statusBar()->showMessage(
                QStringLiteral("Last.fm updated. Refresh loved-track playlists to see the change."),
                5000);
    };
    connect(lastfm_, &LastFmService::completed, this, feedback);
}
// ADR-0220: an interim, and named as one. Scrobbling belongs to whoever owns
// playback, so its eventual home is the engine -- which would also scrobble
// with no window open. It lives here for now because the engine knows paths
// and durations while the tags a scrobble needs are in this process's rows.
// The accounting itself is core::ListenAccounting either way, so the move
// when it comes is of the network client, not of the rules.
void BenchMainWindow::sampleLastFmFromEngine(const EnginePlayback::State& state) {
    if (!lastfm_ || lastfm_clock_.elapsed() - lastfm_sample_time_ < 500) {
        return;
    }
    lastfm_sample_time_ = lastfm_clock_.elapsed();
    LocalTrackRow track;
    if (const auto entry = core::StableId::parse(state.entry.toStdString())) {
        if (auto* tab = tabForDocument(playback_.anchors.document)) {
            if (const auto row = tab->model->rowOfEntry(*entry, playback_.row); row >= 0) {
                track = tab->model->rows()[static_cast<std::size_t>(row)];
            }
        }
    }
    // Observable for offscreen tests and diagnostics: "is anything being
    // credited, and for which track" is otherwise only answerable by watching
    // the network.
    setProperty("trackknife-lastfm-sample",
                QStringLiteral("%1|%2|%3")
                    .arg(QString::fromStdString(track.artist), QString::fromStdString(track.title),
                         state.status == QStringLiteral("playing") ? QStringLiteral("playing")
                                                                   : state.status));
    lastfm_->observe(
        {// The engine's playback instance, so the same track played twice is
         // two listens rather than one long one.
         {"identity", state.instance == 0U ? QString{} : QString::number(state.instance)},
         {"artist", QString::fromStdString(track.artist)},
         {"title", QString::fromStdString(track.title)},
         {"album", QString::fromStdString(track.album)},
         {"duration",
          state.duration_ms > 0 ? static_cast<double>(state.duration_ms) / 1000.0 : 0.0},
         {"position", static_cast<double>(state.position_ms) / 1000.0},
         {"playing", state.status == QStringLiteral("playing")},
         {"monotonic", lastfm_sample_time_}});
}

QWidget* BenchMainWindow::buildLastFmSettings(QWidget* parent) {
    auto* page = new QWidget(parent);
    page->setObjectName(QStringLiteral("lastfm-settings"));
    auto* layout = new QVBoxLayout(page);
    auto* note = new QLabel(QStringLiteral("Playback is scrobbled while Trackknife is open. "
                                           "Account actions take effect immediately."),
                            page);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* credentials = new QWidget(page);
    credentials->setObjectName(QStringLiteral("lastfm-credentials"));
    auto* credentials_layout = new QVBoxLayout(credentials);
    credentials_layout->setContentsMargins(0, 0, 0, 0);
    auto* instructions = new QLabel(
        QStringLiteral("1. Register a free Last.fm API application using the link below. "
                       "Choose an application name such as Trackknife; "
                       "no callback URL is needed for desktop authorization.\n"
                       "2. Paste the API key and shared secret here once.\n"
                       "3. Connect to enable scrobbling, then approve access in your browser. "
                       "This page connects automatically once you approve."),
        credentials);
    instructions->setWordWrap(true);
    credentials_layout->addWidget(instructions);
    auto* form = new QFormLayout;
    auto* key = new QLineEdit(page);
    key->setObjectName(QStringLiteral("lastfm-account-key"));
    key->setEchoMode(QLineEdit::Password);
    key->setText(QSettings{}.value(QStringLiteral("lastfm/api-key")).toString());
    auto* secret = new QLineEdit(page);
    secret->setObjectName(QStringLiteral("lastfm-account-secret"));
    secret->setEchoMode(QLineEdit::Password);
    form->addRow(QStringLiteral("API key:"), key);
    form->addRow(QStringLiteral("Shared secret:"), secret);
    credentials_layout->addLayout(form);
    auto* link = new QLabel(
        QStringLiteral(
            "<a href=\"https://www.last.fm/api/account/create\">Create a Last.fm API account</a> · "
            "<a href=\"https://www.last.fm/api/accounts\">Find your key and shared secret</a>"),
        page);
    link->setOpenExternalLinks(true);
    link->setWordWrap(true);
    credentials_layout->addWidget(link);
    auto* reuse =
        new QCheckBox(QStringLiteral("Use this API key for dynamic playlists too"), credentials);
    reuse->setObjectName(QStringLiteral("lastfm-reuse-key"));
    reuse->setChecked(true);
    credentials_layout->addWidget(reuse);
    layout->addWidget(credentials);
    auto* security =
        new QLabel(QStringLiteral("Credentials are saved privately on this computer."), page);
    security->setWordWrap(true);
    layout->addWidget(security);
    auto* buttons = new QHBoxLayout;
    auto* begin = new QPushButton(QStringLiteral("Connect to Last.fm…"), page);
    begin->setObjectName(QStringLiteral("lastfm-authorize"));
    auto* cancel = new QPushButton(QStringLiteral("Cancel"), page);
    cancel->setObjectName(QStringLiteral("lastfm-cancel"));
    cancel->hide();
    auto* poll = new QTimer(page);
    poll->setObjectName(QStringLiteral("lastfm-auth-poll"));
    poll->setSingleShot(true);
    poll->setInterval(3000);
    auto* deadline = new QTimer(page);
    deadline->setObjectName(QStringLiteral("lastfm-auth-deadline"));
    deadline->setSingleShot(true);
    deadline->setInterval(5 * 60 * 1000);
    auto waiting = [page, begin, cancel, poll, deadline](bool active) {
        page->setProperty("auth-waiting", active);
        const bool idle = !active && !page->property("auth-request-pending").toBool();
        begin->setEnabled(idle);
        cancel->setVisible(active);
        if (active)
            deadline->start();
        else {
            poll->stop();
            deadline->stop();
        }
    };
    auto* disconnect = new QPushButton(QStringLiteral("Disconnect / clear pending"), page);
    buttons->addWidget(begin);
    buttons->addWidget(cancel);
    buttons->addWidget(disconnect);
    layout->addLayout(buttons);
    auto* enabled = new QCheckBox(QStringLiteral("Scrobble playback to Last.fm"), page);
    enabled->setObjectName(QStringLiteral("lastfm-enabled"));
    layout->addWidget(enabled);
    auto* status = new QLabel(page);
    status->setObjectName(QStringLiteral("lastfm-status"));
    status->setWordWrap(true);
    layout->addWidget(status);
    layout->addStretch();
    auto send = [this, status, page](const QString& op, const QStringList& args = QStringList{}) {
        if (op == QStringLiteral("begin") || op == QStringLiteral("finish"))
            page->setProperty("auth-request-pending", true);
        if (op != QStringLiteral("status") && op != QStringLiteral("finish"))
            status->setText(QStringLiteral("Working…"));
        lastfm_->execute(op, args);
    };
    connect(poll, &QTimer::timeout, page, [send] { send(QStringLiteral("finish")); });
    connect(deadline, &QTimer::timeout, page, [waiting, status] {
        waiting(false);
        status->setText(QStringLiteral("Authorization timed out. Connect again to retry."));
    });
    connect(cancel, &QPushButton::clicked, page, [waiting, status] {
        waiting(false);
        status->setText(QStringLiteral("Stopped waiting for approval. Connect again to retry."));
    });
    auto receive = [page, status, enabled, credentials, begin, secret, poll,
                    waiting](const QString& op, const QJsonObject& state, const QString& error) {
        const bool auth_reply = op == QStringLiteral("begin") || op == QStringLiteral("finish");
        if (auth_reply) {
            page->setProperty("auth-request-pending", false);
            if (!page->property("auth-waiting").toBool()) {
                waiting(false);
                return;
            }
        }
        if (!error.isEmpty()) {
            // The provider's "not yet authorized" answer means keep waiting.
            if (op == QStringLiteral("finish") && error.contains(QStringLiteral("(code 14)"))) {
                poll->start();
                return;
            }
            if (auth_reply)
                waiting(false);
            status->setText(error);
            return;
        }
        QSignalBlocker block(enabled);
        enabled->setChecked(state.value("enabled").toBool());
        enabled->setEnabled(state.value("connected").toBool());
        if (op == QStringLiteral("finish") && state.value("authorization_pending").toBool()) {
            poll->start();
            return;
        }
        if (op == QStringLiteral("finish"))
            waiting(false);
        const bool saved = state.value("credentials_saved").toBool();
        credentials->setVisible(!saved);
        begin->setProperty("credentials-saved", saved);
        begin->setText(saved ? QStringLiteral("Reconnect in browser…")
                             : QStringLiteral("Connect to Last.fm…"));
        if (op == QStringLiteral("begin"))
            secret->clear();
        status->setText(
            QStringLiteral("%1 · %2 pending\n%3")
                .arg(state.value("connected").toBool()
                         ? QStringLiteral("Connected as %1").arg(state.value("user").toString())
                         : QStringLiteral("Not connected"))
                .arg(state.value("pending").toInt())
                .arg(state.value("message").toString()));
        if (op == QStringLiteral("begin")) {
            status->setText(QStringLiteral("Waiting for browser approval…"));
            poll->start();
            const QUrl url(state.value("url").toString());
            if (url.scheme() == QStringLiteral("https") &&
                url.host() == QStringLiteral("www.last.fm"))
                QDesktopServices::openUrl(url);
        }
    };
    connect(lastfm_, &LastFmService::completed, page, receive);
    connect(
        begin, &QPushButton::clicked, page,
        [send, key, secret, reuse, begin, status, parent, waiting] {
            if (begin->property("credentials-saved").toBool()) {
                waiting(true);
                send(QStringLiteral("begin"));
                return;
            }
            const auto api_key = key->text().trimmed();
            const auto shared_secret = secret->text().trimmed();
            const auto valid = [](const QString& value) {
                return value.size() == 32 && std::all_of(value.begin(), value.end(), [](QChar c) {
                           return (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f') ||
                                  (c >= u'A' && c <= u'F');
                       });
            };
            if (!valid(api_key) || !valid(shared_secret)) {
                status->setText(QStringLiteral("Paste the 32-character API key and shared secret "
                                               "from your Last.fm API account."));
                return;
            }
            if (reuse->isChecked()) {
                QSettings{}.setValue(QStringLiteral("lastfm/api-key"), api_key);
                // Keep the other settings page in sync so Save cannot overwrite the reused key.
                if (auto* field =
                        parent->findChild<QLineEdit*>(QStringLiteral("bench-settings-lastfm-key")))
                    field->setText(api_key);
            }
            waiting(true);
            send(QStringLiteral("begin"), {api_key, shared_secret});
        });
    connect(disconnect, &QPushButton::clicked, page, [send, waiting] {
        waiting(false);
        send(QStringLiteral("disconnect"));
    });
    connect(enabled, &QCheckBox::toggled, page, [send](bool value) {
        send(QStringLiteral("enable"), {value ? QStringLiteral("1") : QStringLiteral("0")});
    });
    auto* timer = new QTimer(page);
    connect(timer, &QTimer::timeout, page, [page, send] {
        if (page->isVisible() && !page->property("auth-waiting").toBool())
            send(QStringLiteral("status"));
    });
    timer->start(10000);
    send(QStringLiteral("status"));
    return page;
}
void BenchMainWindow::addLastFmActions(QMenu* menu, QTableView* view) {
    if (!lastfm_ || !view->selectionModel())
        return;
    QString artist, title;
    const auto rows = view->selectionModel()->selectedRows();
    if (rows.size() == 1) {
        if (auto* model = qobject_cast<LocalListModel*>(view->model())) {
            const auto& track = model->rows()[static_cast<std::size_t>(rows.first().row())];
            artist = QString::fromStdString(track.artist);
            title = QString::fromStdString(track.title);
        }
    }
    auto* submenu = menu->addMenu(QStringLiteral("Last.fm"));
    const bool available = !artist.isEmpty() && !title.isEmpty();
    submenu->setEnabled(available);
    auto* info = submenu->addAction(QStringLiteral("Checking loved state…"));
    info->setEnabled(false);
    for (const auto& op : {QStringLiteral("love"), QStringLiteral("unlove")}) {
        auto* action =
            submenu->addAction(op == QStringLiteral("love") ? QStringLiteral("Love track")
                                                            : QStringLiteral("Unlove track"));
        connect(action, &QAction::triggered, this,
                [this, op, artist, title] { lastfm_->execute(op, {artist, title}); });
    }
    auto receive = [info, artist, title](const QString& op, const QJsonObject& state,
                                         const QString& error) {
        if (op != QStringLiteral("info"))
            return;
        if (!error.isEmpty()) {
            info->setText(error);
            return;
        }
        if (state.value("artist").toString() == artist && state.value("title").toString() == title)
            info->setText(state.value("loved").toBool() ? QStringLiteral("♥ Loved on Last.fm")
                                                        : QStringLiteral("Not loved on Last.fm"));
    };
    connect(lastfm_, &LastFmService::completed, submenu, receive);
    connect(submenu, &QMenu::aboutToShow, submenu,
            [this, artist, title] { lastfm_->execute(QStringLiteral("info"), {artist, title}); });
}
} // namespace trackknife::bench
