// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "quick/engine_client.hpp"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QtQmlIntegration>

namespace trackknife::quick {

// What the engine plays, and the transport. Everything here is the engine's:
// a command is sent and the state it answers with is adopted, so two windows
// on one engine show the same thing.
class PlayerState final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Reached through Engine.player")

    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(bool playing READ playing NOTIFY changed)
    Q_PROPERTY(QString entry READ entry NOTIFY changed)
    Q_PROPERTY(QString path READ path NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY metadataChanged)
    Q_PROPERTY(QString artist READ artist NOTIFY metadataChanged)
    Q_PROPERTY(QString album READ album NOTIFY metadataChanged)
    Q_PROPERTY(QString date READ date NOTIFY metadataChanged)
    Q_PROPERTY(qreal position READ position NOTIFY positionChanged)
    Q_PROPERTY(qreal duration READ duration NOTIFY changed)
    Q_PROPERTY(int volume READ volume NOTIFY changed)
    Q_PROPERTY(bool repeat READ repeat NOTIFY changed)
    Q_PROPERTY(bool random READ random NOTIFY changed)
    Q_PROPERTY(int single READ single NOTIFY changed)
    Q_PROPERTY(int consume READ consume NOTIFY changed)
    Q_PROPERTY(QString replayGain READ replayGain NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(int requests READ requests NOTIFY changed)

  public:
    explicit PlayerState(EngineClient& client, QObject* parent = nullptr);

    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] bool playing() const { return status_ == QStringLiteral("playing"); }
    [[nodiscard]] QString entry() const { return entry_; }
    [[nodiscard]] QString path() const { return path_; }
    [[nodiscard]] QString title() const { return title_; }
    [[nodiscard]] QString artist() const { return artist_; }
    [[nodiscard]] QString album() const { return album_; }
    [[nodiscard]] QString date() const { return date_; }
    // Seconds. Interpolated between the engine's answers: it does not
    // broadcast position (ADR-0222), and asking many times a second would
    // be traffic carrying nothing.
    [[nodiscard]] qreal position() const;
    [[nodiscard]] qreal duration() const { return static_cast<qreal>(duration_ms_) / 1000.0; }
    [[nodiscard]] int volume() const { return volume_; }
    [[nodiscard]] bool repeat() const { return repeat_; }
    [[nodiscard]] bool random() const { return random_; }
    [[nodiscard]] int single() const { return single_; }
    [[nodiscard]] int consume() const { return consume_; }
    [[nodiscard]] QString replayGain() const { return replay_gain_; }
    [[nodiscard]] QString error() const { return error_; }
    [[nodiscard]] int requests() const { return requests_; }
    [[nodiscard]] quint64 queueRevision() const { return queue_revision_; }

    Q_INVOKABLE void toggle();
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    Q_INVOKABLE void seek(qreal seconds);
    Q_INVOKABLE void setVolume(int percent);
    Q_INVOKABLE void setRepeat(bool on);
    Q_INVOKABLE void setRandom(bool on);
    // 0 off, 1 on, 2 once.
    Q_INVOKABLE void setSingle(int state);
    Q_INVOKABLE void setConsume(int state);
    Q_INVOKABLE void cycleReplayGain();

  signals:
    void changed();
    void metadataChanged();
    void positionChanged();
    // The engine's queue or up-next changed, by any client.
    void queueChanged();

  private:
    void refresh();
    void adopt(const EngineClient::Json& state);
    void send(const QString& method, EngineClient::Json params = EngineClient::Json::object());
    void fetchMetadata();

    EngineClient& client_;
    QString status_{QStringLiteral("stopped")};
    QString entry_;
    QString path_;
    QString title_;
    QString artist_;
    QString album_;
    QString date_;
    qint64 position_ms_{0};
    qint64 duration_ms_{0};
    QElapsedTimer since_;
    int volume_{100};
    bool repeat_{false};
    bool random_{false};
    int single_{0};
    int consume_{0};
    QString replay_gain_{QStringLiteral("off")};
    QString error_;
    int requests_{0};
    quint64 queue_revision_{0};
    std::uint64_t sequence_{0};
    QTimer tick_;
    QTimer poll_;
};

} // namespace trackknife::quick
