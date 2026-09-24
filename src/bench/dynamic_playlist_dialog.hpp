// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "bench/dynamic_playlist_service.hpp"
#include <QDialog>

class QComboBox;
class QLineEdit;
class QSpinBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QFormLayout;
class QTimer;
namespace trackknife::ui {
class QueueTableView;
}
namespace trackknife::bench {
class DynamicPlaylistDialog final : public QDialog {
    Q_OBJECT
  public:
    // Which library a refresh searches: this computer's, or the remote's.
    using LibrarySearch = std::function<void(bool remote, query::CompiledTkq,
                                             core::CancellationToken,
                                             DynamicPlaylistService::Completion)>;
    // `remote_label` names the remote's library; empty when there is none.
    DynamicPlaylistDialog(QString profile, QString remote_label, LibrarySearch search,
                          QWidget* parent = nullptr);
    ~DynamicPlaylistDialog() override;
    ui::QueueTableView* view() const { return view_; }
    // The library the results come from, and so the engine they play on.
    bool remote() const;
    void followLibrary(bool remote);
    void libraryChanged();
    void invalidateAuthority();
    bool authorityValid() const { return authority_valid_; }
    const DynamicPlaylistService::Tracks& tracks() const { return tracks_; }
    QString playlistName() const;
    void playCurrent();
  signals:
    void playRequested(int row);
    void resultsChanged();
    void snapshotRequested(const QString& name, const DynamicPlaylistService::Tracks& tracks);
    void libraryChosen(bool remote);

  private:
    DynamicPlaylistDefinition definition() const;
    void loadSelection();
    void refresh(bool preserve_results = false);
    void updateFields();
    void refill(const QString& selected = {});
    void discardResults();
    QString profile_;
    QVector<DynamicPlaylistDefinition> definitions_;
    bool auto_refresh_{false};
    bool loading_{false};
    bool busy_{false};
    bool refresh_pending_{false};
    bool authority_valid_{true};
    DynamicPlaylistService* service_;
    DynamicPlaylistService::Tracks tracks_;
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
    QFormLayout* form_;
    QLabel* status_;
    QPushButton* refresh_;
    QPushButton* open_;
    QTimer* refresh_timer_;
    ui::QueueTableView* view_;
    LocalListModel* local_model_{nullptr};
};
} // namespace trackknife::bench
