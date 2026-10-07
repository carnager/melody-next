// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "bench/engine_key.hpp"

#include "bench/dynamic_playlist_service.hpp"
#include "workspace/dynamic_playlist_session.hpp"
#include <QDialog>
#include <QSet>
#include <vector>

class QComboBox;
class QLineEdit;
class QSpinBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QFormLayout;
namespace trackknife::ui {
class QueueTableView;
}
namespace trackknife::bench {
class DynamicPlaylistDialog final : public QDialog {
    Q_OBJECT
  public:
    using LibrarySearch = DynamicPlaylistSession::LibrarySearch;
    using Library = DynamicPlaylistSession::Library;
    // `libraries`: the engines to search, this computer's first; none, only
    // this computer's.
    DynamicPlaylistDialog(QString profile, std::vector<Library> libraries, LibrarySearch search,
                          QWidget* parent = nullptr);
    ~DynamicPlaylistDialog() override;
    ui::QueueTableView* view() const { return view_; }
    // The library the results come from, and so the engine they play on.
    EngineKey engine() const { return session_->engine(); }
    void followLibrary(const EngineKey& engine) { session_->followLibrary(engine); }
    void libraryChanged() { session_->libraryChanged(); }
    void invalidateAuthority() { session_->invalidateAuthority(); }
    bool authorityValid() const { return session_->authorityValid(); }
    const DynamicPlaylistService::Tracks& tracks() const { return session_->tracks(); }
    QString playlistName() const { return session_->playlistName(); }
    void playCurrent();
  signals:
    void playRequested(int row);
    void resultsChanged();
    void snapshotRequested(const QString& name, const DynamicPlaylistService::Tracks& tracks);
    void libraryChosen(const trackknife::ui::EngineKey& engine);
    void definitionsSaved();

  protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    void sync();
    void syncCatalog();
    void keepPlace();
    void restorePlace();

    DynamicPlaylistSession* session_;
    // Where the results were before a refresh: the rows selected, the one
    // current and the one at the top, by identity.
    QSet<QByteArray> kept_selected_;
    QByteArray kept_current_;
    QByteArray kept_top_;
    int kept_offset_{0};
    int kept_horizontal_{0};
    QComboBox* library_;
    QComboBox* catalog_;
    QComboBox* source_;
    QLineEdit* name_;
    QLineEdit* query_;
    QLineEdit* artist_;
    QLineEdit* track_;
    QLineEdit* user_;
    QLineEdit* tag_;
    QSpinBox* limit_;
    QCheckBox* shuffle_;
    QLineEdit* group_by_;
    QSpinBox* groups_;
    QSpinBox* per_group_;
    QFormLayout* form_;
    QLabel* status_;
    QPushButton* refresh_;
    QPushButton* save_;
    QPushButton* remove_;
    QPushButton* open_;
    ui::QueueTableView* view_;
};
} // namespace trackknife::bench
