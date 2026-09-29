// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "quick/engine_client.hpp"
#include "quick/up_next_model.hpp"

#include <QAbstractListModel>
#include <QtQmlIntegration>

#include <functional>
#include <vector>

namespace trackknife::quick {

class EngineSession;

// An engine's library by folder, as the index holds it (catalogue.folder):
// the library's folders, then the folders in each that hold music and the
// tracks in it. Nothing here reads the engine's files.
class FolderModel final : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Reached through EngineSession.folders")

    // Where it is: empty for the library's folders.
    Q_PROPERTY(QString path READ path NOTIFY moved)
    Q_PROPERTY(QString name READ name NOTIFY moved)
    Q_PROPERTY(bool atTop READ atTop NOTIFY moved)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString error READ error NOTIFY loadingChanged)

  public:
    enum Role : int {
        KindRole = Qt::UserRole + 1,
        KeyRole,
        TitleRole,
        ArtistRole,
        AlbumRole,
        NumberRole,
        DurationRole,
        CoverRole,
    };

    FolderModel(EngineClient& client, EngineSession& session, QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] QString path() const { return path_; }
    [[nodiscard]] QString name() const { return name_; }
    [[nodiscard]] bool atTop() const { return path_.isEmpty(); }
    [[nodiscard]] bool loading() const { return loading_; }
    [[nodiscard]] QString error() const { return error_; }

    // Into a folder row; up to where this one is.
    Q_INVOKABLE void open(int row);
    Q_INVOKABLE void up();
    Q_INVOKABLE void refresh();
    // A track, or every track under a folder, onto up-next or into a list:
    // the same verbs as the library's, so a drop does not ask which it is.
    Q_INVOKABLE void enqueue(int row);
    Q_INVOKABLE void addToList(int row, const QString& listId, int position);

  signals:
    void moved();
    void loadingChanged();

  private:
    struct Row final {
        bool folder{false};
        QString key;
        QString title;
        QString artist;
        QString album;
        int number{0};
        qint64 duration_ms{0};
    };

    void go(const QString& path);
    void resolve(int row, std::function<void(std::vector<QueuedTrack>)> done);
    void collect(QString folder, QString after, std::shared_ptr<EngineClient::Json> paths,
                 std::function<void(std::vector<QueuedTrack>)> done);

    EngineClient& client_;
    EngineSession& session_;
    QString path_;
    QString name_;
    QString parent_;
    std::vector<Row> rows_;
    bool loading_{false};
    QString error_;
    std::uint64_t generation_{0};
};

} // namespace trackknife::quick
