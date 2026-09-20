// SPDX-License-Identifier: GPL-3.0-only
#include "bench/bench_main_window.hpp"
#include "bench/lastfm_service.hpp"
#include "quick/mpd_probe_controller.hpp"
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
    connect(mpd_controller_, &quick::MpdProbeController::lastFmCompleted, this,
            [feedback](const QString& op, const QByteArray& payload, const QString& error) {
                feedback(op, QJsonDocument::fromJson(payload).object(), error);
            });
}
void BenchMainWindow::sampleLastFm(const audio::LocalAuditionSnapshot& snapshot) {
    if (!lastfm_ || lastfm_clock_.elapsed() - lastfm_sample_time_ < 500)
        return;
    lastfm_sample_time_ = lastfm_clock_.elapsed();
    LocalTrackRow track;
    if (auto* tab = tabForDocument(playback_document_id_)) {
        const auto row = tab->model->rowOfSource(
            {snapshot.raw_path, snapshot.selection, snapshot.segment}, playback_row_);
        if (row >= 0)
            track = tab->model->rows()[static_cast<std::size_t>(row)];
    }
    if (snapshot.occurrence_token != 0) {
        if (local_requests_.active() && local_requests_.active()->id == snapshot.occurrence_token)
            track = local_requests_.active()->source;
        else
            for (const auto& request : local_requests_.pending())
                if (request.id == snapshot.occurrence_token) {
                    track = request.source;
                    break;
                }
    }
    double position = 0, duration = 0;
    if (snapshot.format && snapshot.format->sample_rate > 0) {
        position = static_cast<double>(snapshot.position_sample) / snapshot.format->sample_rate;
        if (snapshot.end_sample)
            duration = static_cast<double>(*snapshot.end_sample) / snapshot.format->sample_rate;
    }
    const bool playing = (snapshot.state == audio::LocalAuditionState::playing ||
                          snapshot.state == audio::LocalAuditionState::draining) &&
                         snapshot.output_target_available && !snapshot.output_suspended;
    lastfm_->observe(
        {{"identity",
          snapshot.raw_path.empty() ? QString{} : QString::number(snapshot.playback_instance)},
         {"artist", QString::fromStdString(track.artist)},
         {"title", QString::fromStdString(track.title)},
         {"album", QString::fromStdString(track.album)},
         {"duration", duration},
         {"position", position},
         {"playing", playing},
         {"monotonic", lastfm_sample_time_}});
}
QWidget* BenchMainWindow::buildLastFmSettings(QWidget* parent) {
    auto* page = new QWidget(parent);
    page->setObjectName(QStringLiteral("lastfm-settings"));
    auto* layout = new QVBoxLayout(page);
    auto* authority = new QComboBox(page);
    authority->setObjectName(QStringLiteral("lastfm-authority"));
    authority->addItems({QStringLiteral("Local playback"), QStringLiteral("Melody server")});
    authority->setCurrentIndex(isMpdContext() ? 1 : 0);
    layout->addWidget(authority);
    auto* note = new QLabel(
        QStringLiteral("Connect each player separately. Melody scrobbles server playback even when "
                       "Trackbench is closed. Local playback is scrobbled only by Trackbench. "
                       "Account actions take effect immediately."),
        page);
    note->setWordWrap(true);
    layout->addWidget(note);
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
    layout->addLayout(form);
    auto* link = new QLabel(
        QStringLiteral(
            "<a href=\"https://www.last.fm/api/account/create\">Create a Last.fm API account</a> · "
            "<a href=\"https://www.last.fm/api/accounts\">Find your key and shared secret</a>"),
        page);
    link->setOpenExternalLinks(true);
    link->setWordWrap(true);
    layout->addWidget(link);
    auto* security = new QLabel(
        QStringLiteral("Credentials are saved privately on the selected player. Server setup uses "
                       "your MPD connection; use a trusted network or tunnel."),
        page);
    security->setWordWrap(true);
    layout->addWidget(security);
    auto* buttons = new QHBoxLayout;
    auto* begin = new QPushButton(QStringLiteral("Authorize in browser…"), page);
    begin->setObjectName(QStringLiteral("lastfm-authorize"));
    auto* finish = new QPushButton(QStringLiteral("Finish authorization"), page);
    finish->setObjectName(QStringLiteral("lastfm-finish"));
    auto* disconnect = new QPushButton(QStringLiteral("Disconnect / clear pending"), page);
    buttons->addWidget(begin);
    buttons->addWidget(finish);
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
    auto send = [this, authority, status](const QString& op,
                                          const QStringList& args = QStringList{}) {
        if (op != QStringLiteral("status"))
            status->setText(QStringLiteral("Working…"));
        if (authority->currentIndex() == 1)
            mpd_controller_->lastFm(op, args);
        else
            lastfm_->execute(op, args);
    };
    auto receive = [status, enabled](const QString& op, const QJsonObject& state,
                                     const QString& error) {
        QSignalBlocker block(enabled);
        enabled->setChecked(state.value("enabled").toBool());
        enabled->setEnabled(state.value("connected").toBool());
        if (!error.isEmpty()) {
            status->setText(error);
            return;
        }
        status->setText(
            QStringLiteral("%1 · %2 pending\n%3")
                .arg(state.value("connected").toBool()
                         ? QStringLiteral("Connected as %1").arg(state.value("user").toString())
                         : QStringLiteral("Not connected"))
                .arg(state.value("pending").toInt())
                .arg(state.value("message").toString()));
        if (op == QStringLiteral("begin")) {
            const QUrl url(state.value("url").toString());
            if (url.scheme() == QStringLiteral("https") &&
                url.host() == QStringLiteral("www.last.fm"))
                QDesktopServices::openUrl(url);
        }
    };
    connect(
        lastfm_, &LastFmService::completed, page,
        [authority, receive](const QString& op, const QJsonObject& state, const QString& error) {
            if (authority->currentIndex() == 0)
                receive(op, state, error);
        });
    connect(
        mpd_controller_, &quick::MpdProbeController::lastFmCompleted, page,
        [authority, receive](const QString& op, const QByteArray& payload, const QString& error) {
            if (authority->currentIndex() == 1)
                receive(op, QJsonDocument::fromJson(payload).object(), error);
        });
    connect(begin, &QPushButton::clicked, page, [send, key, secret] {
        send(QStringLiteral("begin"), {key->text().trimmed(), secret->text().trimmed()});
        secret->clear();
    });
    connect(finish, &QPushButton::clicked, page, [send] { send(QStringLiteral("finish")); });
    connect(disconnect, &QPushButton::clicked, page,
            [send] { send(QStringLiteral("disconnect")); });
    connect(enabled, &QCheckBox::toggled, page, [send](bool value) {
        send(QStringLiteral("enable"), {value ? QStringLiteral("1") : QStringLiteral("0")});
    });
    connect(authority, &QComboBox::currentIndexChanged, page, [send, secret] {
        secret->clear();
        send(QStringLiteral("status"));
    });
    auto* timer = new QTimer(page);
    connect(timer, &QTimer::timeout, page, [page, send] {
        if (page->isVisible())
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
    const bool local = qobject_cast<LocalListModel*>(view->model()) != nullptr;
    if (rows.size() == 1) {
        if (auto* model = qobject_cast<LocalListModel*>(view->model())) {
            const auto& track = model->rows()[static_cast<std::size_t>(rows.first().row())];
            artist = QString::fromStdString(track.artist);
            title = QString::fromStdString(track.title);
        } else {
            const auto tracks = selectedMpdViewTracks(view);
            if (tracks.size() == 1) {
                artist = QString::fromStdString(
                    std::string(tracks[0].metadata.first("Artist").value_or("")));
                title = QString::fromStdString(
                    std::string(tracks[0].metadata.first("Title").value_or("")));
            }
        }
    }
    auto* submenu = menu->addMenu(QStringLiteral("Last.fm"));
    const bool available =
        !artist.isEmpty() && !title.isEmpty() &&
        (local || mpd_controller_->supportsCommand(QStringLiteral("melody_lastfm")));
    submenu->setEnabled(available);
    auto* info = submenu->addAction(QStringLiteral("Checking loved state…"));
    info->setEnabled(false);
    for (const auto& op : {QStringLiteral("love"), QStringLiteral("unlove")}) {
        auto* action =
            submenu->addAction(op == QStringLiteral("love") ? QStringLiteral("Love track")
                                                            : QStringLiteral("Unlove track"));
        connect(action, &QAction::triggered, this, [this, local, op, artist, title] {
            if (local)
                lastfm_->execute(op, {artist, title});
            else
                mpd_controller_->lastFm(op, {artist, title});
        });
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
    if (local)
        connect(lastfm_, &LastFmService::completed, submenu, receive);
    else
        connect(mpd_controller_, &quick::MpdProbeController::lastFmCompleted, submenu,
                [receive](const QString& op, const QByteArray& payload, const QString& error) {
                    receive(op, QJsonDocument::fromJson(payload).object(), error);
                });
    connect(submenu, &QMenu::aboutToShow, submenu, [this, local, artist, title] {
        if (local)
            lastfm_->execute(QStringLiteral("info"), {artist, title});
        else
            mpd_controller_->lastFm(QStringLiteral("info"), {artist, title});
    });
}
} // namespace trackknife::bench
