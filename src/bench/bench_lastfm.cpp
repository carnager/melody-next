// SPDX-License-Identifier: GPL-3.0-only
#include "bench/bench_main_window.hpp"
#include "bench/lastfm_service.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "workspace/lastfm_settings_session.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableView>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>

namespace trackknife::bench {
QWidget* BenchMainWindow::buildLastFmSettings(QWidget* parent) {
    auto* page = new QWidget(parent);
    page->setObjectName(QStringLiteral("lastfm-settings"));
    auto* session = new LastFmSettingsSession(workspace_, page);
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
    credentials_layout->addWidget(reuse);
    layout->addWidget(credentials);
    auto* security =
        new QLabel(QStringLiteral("Credentials are saved privately on this computer."), page);
    security->setWordWrap(true);
    layout->addWidget(security);
    auto* buttons = new QHBoxLayout;
    auto* begin = new QPushButton(page);
    begin->setObjectName(QStringLiteral("lastfm-authorize"));
    auto* cancel = new QPushButton(QStringLiteral("Cancel"), page);
    cancel->setObjectName(QStringLiteral("lastfm-cancel"));
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

    auto* engines = new QGroupBox(QStringLiteral("Engines scrobble what they play"), page);
    engines->setObjectName(QStringLiteral("lastfm-engines"));
    auto* engines_form = new QFormLayout(engines);
    auto* another = new QPushButton(QStringLiteral("Another engine…"), engines);
    another->setObjectName(QStringLiteral("lastfm-engine-another"));
    engines_form->addRow(another);
    layout->addWidget(engines);
    layout->addStretch();

    const auto sync = [session, key, secret, reuse, credentials, begin, cancel, enabled, status] {
        const auto show = [](QLineEdit* field, const QString& text) {
            if (field->text() != text) {
                const QSignalBlocker blocker{field};
                field->setText(text);
            }
        };
        show(key, session->key());
        show(secret, session->secret());
        {
            const QSignalBlocker blocker{reuse};
            reuse->setChecked(session->reuseKey());
        }
        credentials->setVisible(!session->credentialsSaved());
        begin->setText(session->connectText());
        begin->setProperty("credentials-saved", session->credentialsSaved());
        begin->setEnabled(session->canConnect());
        cancel->setVisible(session->waiting());
        {
            const QSignalBlocker blocker{enabled};
            enabled->setChecked(session->scrobbling());
        }
        enabled->setEnabled(session->connected());
        status->setText(session->status());
    };
    // One row per engine: what it says, and handing the account to it.
    auto rows = std::make_shared<std::vector<std::pair<QLabel*, QPushButton*>>>();
    const auto sync_engines = [session, engines, engines_form, rows] {
        const auto& listed = session->engines();
        while (rows->size() < listed.size()) {
            const auto row = static_cast<int>(rows->size());
            const auto& engine = listed[rows->size()];
            auto* line = new QHBoxLayout;
            auto* state = new QLabel(engines);
            state->setObjectName(engine.id + QStringLiteral("-state"));
            auto* use = new QPushButton(engines);
            use->setObjectName(engine.id + QStringLiteral("-use"));
            line->addWidget(state, 1);
            line->addWidget(use);
            engines_form->insertRow(engines_form->rowCount() - 1, engine.name + QStringLiteral(":"),
                                    line);
            QObject::connect(use, &QPushButton::clicked, session,
                             [session, row] { session->useAccount(row); });
            rows->emplace_back(state, use);
        }
        for (std::size_t row = 0; row < rows->size(); ++row) {
            const auto [state, use] = (*rows)[row];
            state->setText(listed[row].state);
            const bool in_use = session->inUse(static_cast<int>(row));
            use->setEnabled(!in_use);
            use->setText(in_use ? QStringLiteral("In use") : QStringLiteral("Use this account"));
        }
    };
    connect(session, &LastFmSettingsSession::changed, page, sync);
    connect(session, &LastFmSettingsSession::enginesChanged, page, sync_engines);
    // Settings shows the reused key too, so its Save cannot overwrite it.
    connect(session, &LastFmSettingsSession::keyReused, page, [parent](const QString& reused) {
        if (auto* field =
                parent->findChild<QLineEdit*>(QStringLiteral("bench-settings-lastfm-key")))
            field->setText(reused);
    });
    connect(key, &QLineEdit::textChanged, session, &LastFmSettingsSession::setKey);
    connect(secret, &QLineEdit::textChanged, session, &LastFmSettingsSession::setSecret);
    connect(reuse, &QCheckBox::toggled, session, &LastFmSettingsSession::setReuseKey);
    connect(begin, &QPushButton::clicked, session, &LastFmSettingsSession::connectAccount);
    connect(cancel, &QPushButton::clicked, session, &LastFmSettingsSession::stopWaiting);
    connect(disconnect, &QPushButton::clicked, session, &LastFmSettingsSession::disconnectAccount);
    connect(enabled, &QCheckBox::toggled, session, &LastFmSettingsSession::setScrobbling);
    connect(another, &QPushButton::clicked, this, [this, engines, session] {
        bool accepted = false;
        const auto address =
            QInputDialog::getText(engines, QStringLiteral("Another engine"),
                                  QStringLiteral("Address of the engine (host:port):"),
                                  QLineEdit::Normal, {}, &accepted)
                .trimmed();
        if (!accepted || address.isEmpty()) {
            return;
        }
        const auto password = QInputDialog::getText(engines, QStringLiteral("Another engine"),
                                                    QStringLiteral("Its password:"),
                                                    QLineEdit::Password, {}, &accepted);
        if (!accepted) {
            return;
        }
        if (const auto error = session->addEngine(address, password); !error.isEmpty()) {
            statusBar()->showMessage(error, 6'000);
        }
    });
    sync();
    sync_engines();
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
