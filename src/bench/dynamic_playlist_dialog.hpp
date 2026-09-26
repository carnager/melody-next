// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "bench/engine_key.hpp"

#include "bench/dynamic_playlist_service.hpp"
#include <QDialog>
#include <vector>

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
    // Which library a refresh searches: that of the engine chosen.
    using LibrarySearch =
        std::function<void(const EngineKey& engine, query::CompiledTkq, core::CancellationToken,
                           DynamicPlaylistService::Completion)>;
    // A library to choose, by its engine and the name it is shown by.
    struct Library {
        EngineKey engine;
        QString name;
    };
    // `libraries`: the engines to search, this computer's first; none, only
    // this computer's.
    DynamicPlaylistDialog(QString profile, std::vector<Library> libraries, LibrarySearch search,
                          QWidget* parent = nullptr);
    ~DynamicPlaylistDialog() override;
    ui::QueueTableView* view() const { return view_; }
    // The library the results come from, and so the engine they play on.
    EngineKey engine() const;
    void followLibrary(const EngineKey& engine);
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
    void libraryChosen(const trackknife::ui::EngineKey& engine);

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
