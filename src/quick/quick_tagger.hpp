// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "quick/quick_artwork.hpp"
#include "quick/quick_identify.hpp"
#include "workspace/field_filter.hpp"
#include "workspace/tagger_session.hpp"

#include <QIdentityProxyModel>
#include <QItemSelectionModel>
#include <QObject>
#include <QQmlEngine>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <memory>
#include <optional>
#include <vector>

namespace trackknife::quick {

// The tag editor's fields as QML reads them: the colours, weight and state
// the widgets delegate takes from Qt's own roles, as named roles.
class TaggerFieldRows final : public QIdentityProxyModel {
    Q_OBJECT

  public:
    enum Role {
        foreground_role = Qt::UserRole + 900,
        bold_role,
        italic_role,
        strike_role,
        staged_role,
        canonical_role,
        editable_role,
        placeholder_role,
    };
    using QIdentityProxyModel::QIdentityProxyModel;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
};

// One "Edit tags" window of the Qt Quick workspace: a TaggerSession, as QML
// draws it. The file and field selections are kept here, as the widgets
// window's views keep theirs; the fields' is the table's own selection model.
class QuickTagger final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Opened by the workspace")
    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(trackknife::quick::QuickArtwork* artwork READ artwork CONSTANT)
    Q_PROPERTY(bool ready READ ready NOTIFY gridReady)
    Q_PROPERTY(QAbstractItemModel* files READ files NOTIFY gridReady)
    Q_PROPERTY(QAbstractItemModel* fields READ fields NOTIFY gridReady)
    Q_PROPERTY(QString commonFolder READ commonFolder NOTIFY filesChanged)
    Q_PROPERTY(int fileSelection READ fileSelection NOTIFY fileSelectionChanged)
    Q_PROPERTY(QItemSelectionModel* fieldSelectionModel READ fieldSelectionModel NOTIFY gridReady)
    Q_PROPERTY(int currentField READ currentField NOTIFY fieldSelectionChanged)
    Q_PROPERTY(int filterRevision READ filterRevision NOTIFY filterChanged)
    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterChanged)
    Q_PROPERTY(bool changedOnly READ changedOnly WRITE setChangedOnly NOTIFY filterChanged)
    Q_PROPERTY(QString filterStatus READ filterStatus NOTIFY filterChanged)
    Q_PROPERTY(QVariantMap state READ state NOTIFY changed)
    Q_PROPERTY(QVariantList scripts READ scripts NOTIFY scriptsChanged)
    Q_PROPERTY(QVariantList fieldLayouts READ fieldLayouts NOTIFY fieldLayoutsChanged)
    Q_PROPERTY(QString activeFieldLayout READ activeFieldLayout NOTIFY fieldLayoutsChanged)
    Q_PROPERTY(QStringList layouts READ layouts NOTIFY outputProfilesChanged)
    Q_PROPERTY(QStringList destinations READ destinations NOTIFY outputProfilesChanged)

  public:
    QuickTagger(QString title, std::size_t count, bench::MetadataPropertiesSourceReader reader,
                bench::TaggerServices services,
                bench::ArtworkWritePlanApplierFactory artwork_applier,
                bench::ArtworkApplyObserver artwork_observer, QObject* parent = nullptr);
    ~QuickTagger() override;

    [[nodiscard]] QString title() const { return title_; }
    [[nodiscard]] QuickArtwork* artwork() const { return artwork_; }
    [[nodiscard]] bool ready() const { return session_->ready(); }
    [[nodiscard]] QAbstractItemModel* files() const;
    [[nodiscard]] QAbstractItemModel* fields() const { return field_rows_; }
    [[nodiscard]] QString commonFolder() const { return session_->commonFolder(); }
    [[nodiscard]] int fileSelection() const { return file_selection_revision_; }
    [[nodiscard]] QItemSelectionModel* fieldSelectionModel() const { return field_selection_; }
    [[nodiscard]] int currentField() const;
    [[nodiscard]] int filterRevision() const { return filter_revision_; }
    [[nodiscard]] QString filterText() const { return filter_.query; }
    void setFilterText(const QString& text);
    [[nodiscard]] bool changedOnly() const { return filter_.changed_only; }
    void setChangedOnly(bool on);
    [[nodiscard]] QString filterStatus() const { return filter_status_; }
    // Everything the window shows that changes as the session works.
    [[nodiscard]] QVariantMap state() const;
    [[nodiscard]] QVariantList scripts() const;
    [[nodiscard]] QVariantList fieldLayouts() const;
    [[nodiscard]] QString activeFieldLayout() const { return session_->activeFieldLayout(); }
    [[nodiscard]] QStringList layouts() const;
    [[nodiscard]] QStringList destinations() const;

    // The files: a click on a name selects it (with Ctrl or Shift, adds or
    // extends); on its check box, or Space, toggles it.
    Q_INVOKABLE [[nodiscard]] bool fileSelected(int row) const;
    Q_INVOKABLE void clickFile(int row, int modifiers, bool on_check);
    Q_INVOKABLE void selectAllFiles();
    [[nodiscard]] Q_INVOKABLE QString fileText(int row) const;

    // The fields.
    Q_INVOKABLE [[nodiscard]] bool fieldHidden(int row) const;
    Q_INVOKABLE void setField(int row, const QString& value);
    Q_INVOKABLE void removeFields();
    Q_INVOKABLE void revertFields();
    // A field added: the filter cleared and the new field made current.
    Q_INVOKABLE int addField(const QString& name);
    Q_INVOKABLE [[nodiscard]] QStringList fieldNameSuggestions(const QString& query) const;
    // The current field's exact values: row, heading, context, values.
    Q_INVOKABLE [[nodiscard]] QVariantMap exactValues() const;
    Q_INVOKABLE void replaceValues(int row, const QStringList& values);
    Q_INVOKABLE void setExactValuesOpen(bool open) { session_->setExactValuesDialogOpen(open); }
    Q_INVOKABLE void setFieldNameOpen(bool open) { session_->setFieldNameDialogOpen(open); }

    // The draft.
    Q_INVOKABLE void undo() { session_->undo(); }
    Q_INVOKABLE void redo() { session_->redo(); }
    Q_INVOKABLE void discardAll() { session_->discardAll(); }
    Q_INVOKABLE void statusLink(const QString& link);
    Q_INVOKABLE void suggest() { session_->startProposals(); }
    // "Identify…": a MusicBrainz search for the selected files; none when
    // it cannot open.
    Q_INVOKABLE trackknife::quick::QuickIdentify* identify();

    // Field sets.
    Q_INVOKABLE void selectFieldLayout(const QString& id);
    Q_INVOKABLE void saveFieldLayout(const QString& name);
    Q_INVOKABLE void removeFieldLayout() { session_->removeFieldLayout(); }

    // What Apply does.
    Q_INVOKABLE void setSaveTags(bool on);
    Q_INVOKABLE void chooseRename(bool on) { session_->chooseRename(on); }
    Q_INVOKABLE void chooseMove(bool on) { session_->chooseMove(on); }
    Q_INVOKABLE void selectLayout(int index);
    Q_INVOKABLE void selectDestination(int index);
    Q_INVOKABLE void setReplayGainGrouping(int index) { session_->setReplayGainGrouping(index); }
    Q_INVOKABLE void setReplayGainExpression(const QString& expression) {
        session_->setReplayGainExpression(expression);
    }
    Q_INVOKABLE void scanReplayGain() { session_->startReplayGainScan(); }
    Q_INVOKABLE void exportReplayGain(const QUrl& file);
    Q_INVOKABLE [[nodiscard]] QVariantMap loudnessSources() const;
    Q_INVOKABLE void toggleScript(const QString& id, bool on) {
        session_->toggleAutomaticScript(id, on);
    }
    Q_INVOKABLE void reloadOutputProfiles() { session_->reloadOutputProfiles(); }

    // Apply.
    Q_INVOKABLE void apply() { session_->startWritePlan(); }
    Q_INVOKABLE void stopApply() { session_->requestApplyStop(); }
    Q_INVOKABLE void retry() { session_->retryUnfinished(); }
    Q_INVOKABLE void feedbackFinished() { session_->feedbackFinished(); }
    Q_INVOKABLE void folderImagesReviewed(bool accepted) {
        session_->folderImagesReviewed(accepted);
    }

    // Closing: "close", "confirm-artwork", "confirm-drafts" or "wait".
    Q_INVOKABLE QString requestClose();
    Q_INVOKABLE void closing(bool discard) { session_->closing(discard); }
    // Closing without the covers staged.
    Q_INVOKABLE void discardArtwork() { artwork_session_->discardPendingChanges(); }
    // Its window closed: gone.
    Q_INVOKABLE void release() { deleteLater(); }

  signals:
    void changed();
    void gridReady();
    void filesChanged();
    void fileSelectionChanged();
    void fieldSelectionChanged();
    void filterChanged();
    void scriptsChanged();
    void fieldLayoutsChanged();
    void outputProfilesChanged();
    // A field to be shown: scrolled to and made current.
    void revealField(int row);
    void feedbackRequested(const QString& title, const QString& summary, const QVariantList& rows,
                           bool retryOffered, bool applyCommitted);
    void folderImagesRequested(const QString& note, const QVariantList& rows);
    void closeRequested();
    void settingsRequested(const QString& page);
    void destinationsRequested();
    void statusMessage(const QString& message);

  private:
    void refilter();
    void noteFieldSelection();
    [[nodiscard]] std::vector<int> selectedFieldRows() const;

    QString title_;
    bench::TaggerSession* session_{nullptr};
    bench::ArtworkSession* artwork_session_{nullptr};
    QuickArtwork* artwork_{nullptr};
    TaggerFieldRows* field_rows_{nullptr};
    QItemSelectionModel* field_selection_{nullptr};
    bench::FieldFilter filter_;
    std::vector<bool> hidden_;
    QString filter_status_;
    int filter_revision_{0};
    int file_selection_revision_{0};
    int file_anchor_{-1};
    QTimer* filter_debounce_{nullptr};
};

} // namespace trackknife::quick
