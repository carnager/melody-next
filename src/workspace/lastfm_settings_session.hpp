// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/protocol/client.hpp"

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>

#include <vector>

namespace trackknife::bench {

class Workspace;

// The Last.fm page (ADR-0220): signing in once -- the API key and shared
// secret pasted, access approved in the browser while this waits for it --
// scrobbling switched on or off, and the account handed to each engine so
// it scrobbles what it plays itself. Account actions take effect at once.
// The window's Last.fm page draws it.
class LastFmSettingsSession final : public QObject {
    Q_OBJECT

  public:
    // An engine the account can be handed to.
    struct Engine {
        QString name;
        // What its row is called, for finding it: lastfm-engine-local, ...
        QString id;
        QString state{QStringLiteral("Asking…")};
        // The account it scrobbles as; empty for none.
        QString user;
        protocol::Endpoint endpoint;
    };

    explicit LastFmSettingsSession(Workspace& work, QObject* parent = nullptr);

    // The draft credentials.
    [[nodiscard]] QString key() const { return key_; }
    [[nodiscard]] QString secret() const { return secret_; }
    [[nodiscard]] bool reuseKey() const { return reuse_key_; }
    void setKey(const QString& key);
    void setSecret(const QString& secret);
    void setReuseKey(bool on);

    // What is shown.
    [[nodiscard]] bool credentialsSaved() const { return credentials_saved_; }
    [[nodiscard]] QString connectText() const;
    [[nodiscard]] bool canConnect() const { return !waiting_ && !request_pending_; }
    [[nodiscard]] bool waiting() const { return waiting_; }
    [[nodiscard]] bool connected() const { return connected_; }
    [[nodiscard]] bool scrobbling() const { return scrobbling_; }
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] const std::vector<Engine>& engines() const { return engines_; }
    // The engine already scrobbles as this account: handing it over again
    // would do nothing.
    [[nodiscard]] bool inUse(int row) const;

    // What the user does.
    void connectAccount();
    void stopWaiting();
    void disconnectAccount();
    void setScrobbling(bool on);
    void useAccount(int row);
    // Another engine, by address and password; the error, if it is not one.
    [[nodiscard]] QString addEngine(const QString& address, const QString& password);

  signals:
    void changed();
    void enginesChanged();
    // The key was kept for dynamic playlists too: Settings shows it.
    void keyReused(const QString& key);

  private:
    friend class BenchMainWindowTest;

    void send(const QString& op, const QStringList& args = {});
    void setWaiting(bool active);
    void received(const QString& op, const QJsonObject& state, const QString& error);
    void ask(int row);
    void setEngineState(const QString& id, const QString& state, const QString& user,
                        bool answered);

    Workspace& work_;
    QString key_;
    QString secret_;
    bool reuse_key_{true};
    bool credentials_saved_{false};
    bool waiting_{false};
    bool request_pending_{false};
    bool connected_{false};
    bool scrobbling_{false};
    QString status_;
    std::vector<Engine> engines_;
    int other_engines_{0};
    QTimer poll_;
    QTimer deadline_;
    QTimer refresh_;
};

} // namespace trackknife::bench
