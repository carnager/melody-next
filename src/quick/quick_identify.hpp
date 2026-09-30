// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "workspace/identify_session.hpp"

#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QVariantList>

namespace trackknife::quick {

// "Identify with MusicBrainz" in the Qt Quick window: an IdentifySession
// and, once a version is chosen, its TrackMatchSession, as QML draws them.
class QuickIdentify final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Opened by a tag editor")
    Q_PROPERTY(QString initialArtist READ initialArtist CONSTANT)
    Q_PROPERTY(QString initialRelease READ initialRelease CONSTANT)
    Q_PROPERTY(QVariantMap state READ state NOTIFY changed)
    Q_PROPERTY(QVariantList candidates READ candidates NOTIFY candidatesChanged)
    Q_PROPERTY(bool matching READ matching NOTIFY changed)
    Q_PROPERTY(QVariantMap match READ match NOTIFY matchChanged)
    Q_PROPERTY(QVariantList matchRows READ matchRows NOTIFY matchChanged)

  public:
    explicit QuickIdentify(bench::IdentifySession* session, QObject* parent = nullptr);

    [[nodiscard]] QString initialArtist() const { return session_->initialArtist(); }
    [[nodiscard]] QString initialRelease() const { return session_->initialRelease(); }
    [[nodiscard]] QVariantMap state() const;
    [[nodiscard]] QVariantList candidates() const;
    [[nodiscard]] bool matching() const { return match_ != nullptr; }
    [[nodiscard]] QVariantMap match() const;
    [[nodiscard]] QVariantList matchRows() const;

    Q_INVOKABLE void search(const QString& artist, const QString& release) {
        session_->search(artist, release);
    }
    Q_INVOKABLE void scan() { session_->scan(); }
    Q_INVOKABLE void use(int row) { session_->use(row); }
    Q_INVOKABLE void back() { session_->back(); }
    Q_INVOKABLE [[nodiscard]] bool canMoveUp(int row) const;
    Q_INVOKABLE [[nodiscard]] bool canMoveDown(int row) const;
    Q_INVOKABLE [[nodiscard]] bool canUnmatch(int row) const;
    Q_INVOKABLE void move(int row, int direction);
    Q_INVOKABLE void moveFile(int from, int to);
    Q_INVOKABLE void unmatch(int row);
    Q_INVOKABLE void resetOrder(bool by_filename);
    Q_INVOKABLE void stage();
    // Its window closed: gone.
    Q_INVOKABLE void release() { deleteLater(); }

  signals:
    void changed();
    void candidatesChanged();
    // The pairs changed; `select` is the row to make current, or -1.
    void matchChanged(int select);
    void accepted();

  private:
    bench::IdentifySession* session_;
    QPointer<bench::TrackMatchSession> match_;
    int candidate_count_{-1};
};

} // namespace trackknife::quick
