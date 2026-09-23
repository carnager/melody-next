// SPDX-License-Identifier: GPL-3.0-only
#include "bench/bench_main_window.hpp"
#include "bench/lastfm_service.hpp"
#include "trackknife/audio/local_audition.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QFormLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <QPointer>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QHBoxLayout>
#include <QGroupBox>
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
    // Who this window is signed in as, to tell an engine already using the
    // same account from one that is not.
    connect(lastfm_, &LastFmService::completed, this,
            [this](const QString&, const QJsonObject& state, const QString&) {
                if (state.contains(QStringLiteral("user"))) {
                    lastfm_user_ = state.value(QStringLiteral("user")).toString();
                }
            });
    lastfm_->execute(QStringLiteral("status"));
}
void BenchMainWindow::askEngineLastFm(const protocol::Endpoint& endpoint, QLabel* state,
                                      QPushButton* use) {
    const QPointer<QLabel> label{state};
    const QPointer<QPushButton> button{use};
    // The line to show, and the account the engine uses (empty for none).
    using Answer = std::pair<QString, QString>;
    auto* watcher = new QFutureWatcher<Answer>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, label, button] {
        watcher->deleteLater();
        const auto [text, user] = watcher->result();
        if (label) {
            label->setText(text);
        }
        showEngineAccount(button, user);
    });
    watcher->setFuture(QtConcurrent::run([endpoint]() -> Answer {
        auto client = protocol::Client::connect(endpoint);
        if (!client) {
            return {QStringLiteral("Not reachable"), {}};
        }
        auto answer = (*client)->call("lastfm.status", protocol::Json::object(),
                                      std::chrono::seconds{3});
        (*client)->close();
        if (!answer) {
            return {QStringLiteral("Cannot scrobble (engine too old)"), {}};
        }
        const auto user = answer->value("user", protocol::Json{});
        if (!user.is_string() || !answer->value("enabled", false)) {
            return {QStringLiteral("Not scrobbling"), {}};
        }
        const auto name = QString::fromStdString(user.get<std::string>());
        return {QStringLiteral("Scrobbling as %1 · %2 waiting")
                    .arg(name)
                    .arg(answer->value("pending", 0)),
                name};
    }));
}

void BenchMainWindow::showEngineAccount(QPushButton* use, const QString& engine_user) {
    if (use == nullptr) {
        return;
    }
    // Handing over the account the engine already has would do nothing.
    const bool in_use = !engine_user.isEmpty() && engine_user == lastfm_user_;
    use->setEnabled(!in_use);
    use->setText(in_use ? QStringLiteral("In use") : QStringLiteral("Use this account"));
}

void BenchMainWindow::handOverLastFm(const protocol::Endpoint& endpoint, QLabel* state,
                                     QPushButton* use) {
    const QPointer<QLabel> label{state};
    const QPointer<QPushButton> button{use};
    if (label) {
        label->setText(QStringLiteral("Handing over…"));
    }
    // The session, from this window's own sign-in, then to the engine.
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = connect(
        lastfm_, &LastFmService::completed, this,
        [this, endpoint, label, button, connection](const QString& op,
                                                    const QJsonObject& session,
                                            const QString& error) {
            if (op != QStringLiteral("session")) {
                return;
            }
            disconnect(*connection);
            if (!error.isEmpty()) {
                if (label) {
                    label->setText(error);
                }
                return;
            }
            const protocol::Json params{
                {"api_key", session.value("api_key").toString().toStdString()},
                {"secret", session.value("secret").toString().toStdString()},
                {"session_key", session.value("session_key").toString().toStdString()},
                {"user", session.value("user").toString().toStdString()}};
            auto* watcher = new QFutureWatcher<QString>(this);
            connect(watcher, &QFutureWatcherBase::finished, this,
                    [this, watcher, label, button, endpoint] {
                watcher->deleteLater();
                if (label) {
                    label->setText(watcher->result());
                }
                if (watcher->result().startsWith(QStringLiteral("Scrobbling as "))) {
                    showEngineAccount(button, lastfm_user_);
                }
                // The engines this window plays on may scrobble now: it
                // stops crediting them itself.
                for (auto* playback : {local_playback_, remote_playback_}) {
                    if (playback != nullptr) {
                        playback->refreshScrobbling();
                    }
                }
            });
            watcher->setFuture(QtConcurrent::run([endpoint, params] {
                auto client = protocol::Client::connect(endpoint);
                if (!client) {
                    return QStringLiteral("Not reachable: %1")
                        .arg(QString::fromStdString(client.error().message));
                }
                auto answer = (*client)->call("lastfm.set_session", params);
                (*client)->close();
                if (!answer) {
                    return QStringLiteral("Not handed over: %1")
                        .arg(QString::fromStdString(answer.error().message));
                }
                return QStringLiteral("Scrobbling as %1")
                    .arg(QString::fromStdString(answer->value("user", std::string{})));
            }));
        });
    lastfm_->execute(QStringLiteral("session"));
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
    // An engine with its own Last.fm session scrobbles what it plays; this
    // window crediting it too would count every listen twice.
    if (transport_ != nullptr && transport_->scrobblesItself()) {
        setProperty("trackknife-lastfm-sample", QStringLiteral("engine"));
        return;
    }
    // Wherever this window holds the entry: the list it was played from, Up
    // Next, or another list. Looked for only in the first, a track from Up
    // Next was credited with no artist and no title.
    LocalTrackRow track;
    if (const auto* row = playingRow(state.entry)) {
        track = *row;
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
    auto* note = new QLabel(
        QStringLiteral("Sign in here once. Hand the account to an engine below and it scrobbles "
                       "what it plays itself, with Trackknife closed; until then, playback is "
                       "scrobbled while Trackknife is open. Account actions take effect "
                       "immediately."),
        page);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
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
    instructions->setForegroundRole(QPalette::PlaceholderText);
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

    // ADR-0220: the engines scrobble what they play. One session serves them
    // all; it is handed over, never read back.
    auto* engines = new QGroupBox(QStringLiteral("Engines scrobble what they play"), page);
    engines->setObjectName(QStringLiteral("lastfm-engines"));
    auto* engines_form = new QFormLayout(engines);
    const auto add_engine = [this, engines, engines_form](const QString& name,
                                                          const protocol::Endpoint& endpoint,
                                                          const QString& object_name) {
        auto* row = new QHBoxLayout;
        auto* state = new QLabel(QStringLiteral("Asking…"), engines);
        state->setObjectName(object_name + QStringLiteral("-state"));
        auto* use = new QPushButton(QStringLiteral("Use this account"), engines);
        use->setObjectName(object_name + QStringLiteral("-use"));
        row->addWidget(state, 1);
        row->addWidget(use);
        engines_form->addRow(name + QStringLiteral(":"), row);
        askEngineLastFm(endpoint, state, use);
        connect(use, &QPushButton::clicked, this,
                [this, endpoint, state, use] { handOverLastFm(endpoint, state, use); });
    };
    if (catalogue_source_ && catalogue_source_->endpoint()) {
        add_engine(QStringLiteral("This computer"), *catalogue_source_->endpoint(),
                   QStringLiteral("lastfm-engine-local"));
    }
    if (remote_catalogue_source_ && remote_catalogue_source_->endpoint()) {
        add_engine(remote_catalogue_source_->name(), *remote_catalogue_source_->endpoint(),
                   QStringLiteral("lastfm-engine-remote"));
    }
    auto* another = new QPushButton(QStringLiteral("Another engine…"), engines);
    another->setObjectName(QStringLiteral("lastfm-engine-another"));
    engines_form->addRow(another);
    connect(another, &QPushButton::clicked, this, [this, engines, add_engine] {
        bool accepted = false;
        const auto address = QInputDialog::getText(
            engines, QStringLiteral("Another engine"),
            QStringLiteral("Address of the engine (host:port):"), QLineEdit::Normal, {}, &accepted)
                                 .trimmed();
        if (!accepted || address.isEmpty()) {
            return;
        }
        const auto password =
            QInputDialog::getText(engines, QStringLiteral("Another engine"),
                                  QStringLiteral("Its password, if it has one:"),
                                  QLineEdit::Password, {}, &accepted);
        if (!accepted) {
            return;
        }
        const auto endpoint = protocol::Endpoint::parse(address.toStdString(), password.toStdString());
        if (!endpoint) {
            statusBar()->showMessage(QStringLiteral("Not an engine address: %1").arg(address), 6'000);
            return;
        }
        add_engine(address, *endpoint, QStringLiteral("lastfm-engine-other"));
    });
    layout->addWidget(engines);
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
