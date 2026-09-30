// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "workspace/artwork_session.hpp"

#include <QImage>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QUrl>
#include <QVariantList>

namespace trackknife::quick {

// The tag editor's Artwork page in the Qt Quick window: an ArtworkSession,
// as QML draws it. Its pictures are served as image://artwork/<id>/...
class QuickArtwork final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Part of a tag editor")
    Q_PROPERTY(QString id READ id CONSTANT)
    Q_PROPERTY(QVariantMap state READ state NOTIFY changed)
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    Q_PROPERTY(QAbstractItemModel* items READ items CONSTANT)
    Q_PROPERTY(QItemSelectionModel* itemSelection READ itemSelection CONSTANT)
    Q_PROPERTY(QAbstractItemModel* pending READ pending CONSTANT)
    Q_PROPERTY(QItemSelectionModel* pendingSelection READ pendingSelection CONSTANT)
    Q_PROPERTY(QAbstractItemModel* issues READ issues CONSTANT)
    Q_PROPERTY(QVariantList pickerRows READ pickerRows NOTIFY pickerChanged)
    Q_PROPERTY(QString pickerStatus READ pickerStatus NOTIFY pickerChanged)
    Q_PROPERTY(int pickerSuggested READ pickerSuggested NOTIFY pickerChanged)
    Q_PROPERTY(QStringList addRoles READ addRoles CONSTANT)

  public:
    explicit QuickArtwork(bench::ArtworkSession* session, QObject* parent = nullptr);
    ~QuickArtwork() override;

    [[nodiscard]] bench::ArtworkSession& session() { return *session_; }
    [[nodiscard]] QString id() const { return id_; }
    [[nodiscard]] QVariantMap state() const;
    [[nodiscard]] int revision() const { return revision_; }
    [[nodiscard]] QAbstractItemModel* items() const { return session_->items(); }
    [[nodiscard]] QItemSelectionModel* itemSelection() const { return session_->itemSelection(); }
    [[nodiscard]] QAbstractItemModel* pending() const { return session_->pending(); }
    [[nodiscard]] QItemSelectionModel* pendingSelection() const {
        return session_->pendingSelection();
    }
    [[nodiscard]] QAbstractItemModel* issues() const { return session_->issues(); }
    [[nodiscard]] QVariantList pickerRows() const;
    [[nodiscard]] QString pickerStatus() const { return session_->pickerStatus(); }
    [[nodiscard]] int pickerSuggested() const { return session_->pickerSuggested(); }
    [[nodiscard]] QStringList addRoles() const { return bench::ArtworkSession::addRoles(); }

    Q_INVOKABLE void stageFrontCover(const QUrl& file);
    Q_INVOKABLE void pasteFrontCover();
    Q_INVOKABLE void removeFrontCover() { session_->removeFrontCover(); }
    Q_INVOKABLE void add(const QUrl& file, int role) { session_->add(file.toLocalFile(), role); }
    Q_INVOKABLE void replace(const QUrl& file) { session_->replace(file.toLocalFile()); }
    Q_INVOKABLE void removeSelected() { session_->removeSelected(); }
    Q_INVOKABLE void copySelected() { session_->copySelected(); }
    Q_INVOKABLE void exportSelected(const QUrl& folder) {
        session_->exportSelected(folder.toLocalFile());
    }
    Q_INVOKABLE void save() { session_->save(); }
    Q_INVOKABLE void discard() { session_->discardPendingChanges(); }
    Q_INVOKABLE void undoSelected() { session_->undoSelectedPending(); }
    Q_INVOKABLE void stop() { session_->requestStop(); }
    Q_INVOKABLE void folderImagesReviewed(bool accepted) {
        session_->folderImagesReviewed(accepted);
    }
    Q_INVOKABLE bool openPicker() { return session_->openPicker(); }
    Q_INVOKABLE void usePicker(int row) { session_->usePicker(row); }
    Q_INVOKABLE void closePicker() { session_->closePicker(); }

    // What the image provider serves: a table cell's picture, the front
    // cover, a picker row's thumbnail.
    [[nodiscard]] QImage image(const QString& what) const;
    [[nodiscard]] static QPointer<QuickArtwork> find(const QString& id);

  signals:
    void changed();
    void pickerChanged();
    void pickerClosed();
    void feedbackRequested(const QString& title, const QString& summary, const QVariantList& rows);
    void folderImagesRequested(const QString& note, const QVariantList& rows);

  private:
    bench::ArtworkSession* session_;
    QString id_;
    int revision_{0};
};

// image://artwork/<id>/items/<row>, .../pending/<row>/<column>, .../front,
// .../picker/<row> -- with ?<revision> to be asked again.
class ArtworkImageProvider final : public QQuickImageProvider {
  public:
    ArtworkImageProvider();
    QImage requestImage(const QString& id, QSize* size, const QSize& requested) override;
};

} // namespace trackknife::quick
