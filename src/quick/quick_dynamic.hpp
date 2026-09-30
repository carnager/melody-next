// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "workspace/dynamic_playlist_session.hpp"

#include <QObject>
#include <QQmlEngine>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

namespace trackknife::quick {

class QuickWorkspace;

// Dynamic playlists in the Qt Quick window: a DynamicPlaylistSession, as
// QML draws it, with what can be done with the tracks it finds.
class QuickDynamic final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Opened by the workspace")
    Q_PROPERTY(QStringList libraries READ libraries CONSTANT)
    Q_PROPERTY(QVariantList sources READ sources CONSTANT)
    Q_PROPERTY(QVariantMap state READ state NOTIFY changed)
    Q_PROPERTY(QStringList catalog READ catalog NOTIFY catalogChanged)
    Q_PROPERTY(QVariantList results READ results NOTIFY resultsChanged)

  public:
    QuickDynamic(bench::DynamicPlaylistSession* session, QuickWorkspace& workspace,
                 QObject* parent = nullptr);

    [[nodiscard]] bench::DynamicPlaylistSession* session() const { return session_; }
    [[nodiscard]] QStringList libraries() const { return session_->libraryNames(); }
    [[nodiscard]] static QVariantList sources();
    [[nodiscard]] QVariantMap state() const;
    [[nodiscard]] QStringList catalog() const { return session_->catalogNames(); }
    [[nodiscard]] QVariantList results() const;

    Q_INVOKABLE void chooseLibrary(int index) { session_->chooseLibrary(index); }
    Q_INVOKABLE void selectDefinition(int index) { session_->selectDefinition(index); }
    Q_INVOKABLE void setName(const QString& text) { session_->setName(text); }
    Q_INVOKABLE void setSource(const QString& source) { session_->setSource(source); }
    Q_INVOKABLE void setQuery(const QString& text) { session_->setQuery(text); }
    Q_INVOKABLE void setArtist(const QString& text) { session_->setArtist(text); }
    Q_INVOKABLE void setTrack(const QString& text) { session_->setTrack(text); }
    Q_INVOKABLE void setUser(const QString& text) { session_->setUser(text); }
    Q_INVOKABLE void setTag(const QString& text) { session_->setTag(text); }
    Q_INVOKABLE void setLimit(int limit) { session_->setLimit(limit); }
    Q_INVOKABLE void setShuffle(bool on) { session_->setShuffle(on); }
    Q_INVOKABLE void save() { session_->save(); }
    Q_INVOKABLE void remove() { session_->remove(); }
    Q_INVOKABLE void refresh() { session_->refresh(); }
    Q_INVOKABLE void stop() { session_->stop(); }
    Q_INVOKABLE void setShown(bool shown) { session_->setShown(shown); }

    // What is found: played from `row` in a tab of its own, or kept as an
    // editable snapshot.
    Q_INVOKABLE void play(int row);
    Q_INVOKABLE void openSnapshot();
    // The rows chosen (`current` the one the cursor is on), as a list's
    // would be.
    Q_INVOKABLE void queue(const QVariantList& rows, bool next);
    Q_INVOKABLE void editTags(const QVariantList& rows);
    Q_INVOKABLE void replayGain(const QVariantList& rows);
    Q_INVOKABLE void convertFiles(const QVariantList& rows);
    Q_INVOKABLE void locate(int row, bool album);
    Q_INVOKABLE void copyToNewTab(const QVariantList& rows, const QString& name);
    Q_INVOKABLE void copyTo(const QVariantList& rows, const QString& list_id);
    Q_INVOKABLE QVariantMap ratingState(const QVariantList& rows) const;
    Q_INVOKABLE void rate(const QVariantList& rows, bool album, int rating);
    Q_INVOKABLE QVariantMap lastFmTrack(const QVariantList& rows) const;
    Q_INVOKABLE void askLastFm(const QVariantList& rows);
    Q_INVOKABLE void loveOnLastFm(const QVariantList& rows, bool love);
    Q_INVOKABLE void release() { deleteLater(); }

  signals:
    void changed();
    void catalogChanged();
    void resultsChanged();

  private:
    [[nodiscard]] std::vector<int> rowsOf(const QVariantList& rows) const;
    void markPlaying();

    bench::DynamicPlaylistSession* session_;
    QuickWorkspace& workspace_;
    QString playback_context_;
    int playing_{-1};
    QTimer markers_;
};

} // namespace trackknife::quick
