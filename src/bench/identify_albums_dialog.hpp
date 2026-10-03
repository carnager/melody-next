// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "bench/musicbrainz_lookup.hpp"

#include <QDialog>
#include <QPointer>

#include <cstddef>
#include <optional>
#include <vector>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QStackedWidget;
class QProgressBar;
class QPushButton;
class QTextBrowser;
class QTreeWidget;

namespace trackknife::bench {

class AlbumBatchSession;
class TaggerSession;
class TrackMatchSession;

// ADR-0261: Identify albums… -- the albums of the files open in a tagger,
// grouped, looked up in the background, and the clear matches staged.
class IdentifyAlbumsDialog final : public QDialog {
    Q_OBJECT
  public:
    IdentifyAlbumsDialog(TaggerSession& tagger, MusicBrainzLookupService service,
                         QWidget* parent = nullptr);

    [[nodiscard]] AlbumBatchSession* session() const { return session_; }

    // Step 3: review an album needing a person, or the next that does.
    void review(std::size_t album);
    void reviewNext();
    // Step 4: the staged albums, to be written.
    void showApply();

  protected:
    void closeEvent(QCloseEvent* event) override;

  public slots:
    void reject() override;

  private:
    void sync();
    void showVersion(int version);
    void accept();
    void skip();
    void backToList();
    void syncApply();
    void write();
    [[nodiscard]] std::vector<std::size_t> albumsToWrite() const;
    void showDetail();
    [[nodiscard]] int selectedAlbum() const;
    [[nodiscard]] bool shown(int album) const;

    QPointer<TaggerSession> tagger_;
    AlbumBatchSession* session_;
    QLabel* heading_;
    QProgressBar* progress_;
    QLabel* progress_text_;
    QPushButton* stop_;
    QWidget* filters_;
    QButtonGroup* filter_group_;
    QTreeWidget* list_;
    QTextBrowser* detail_;
    QPushButton* split_;
    QPushButton* merge_;
    QLabel* summary_;
    QPushButton* look_up_;
    QPushButton* review_next_;
    QWidget* bottom_bar_;
    QStackedWidget* pages_;
    QLabel* review_heading_;
    QListWidget* versions_;
    QWidget* match_holder_;
    TrackMatchSession* match_{nullptr};
    QWidget* match_view_{nullptr};
    std::optional<std::size_t> reviewing_;
    QPushButton* apply_;
    QLabel* apply_heading_;
    QTreeWidget* apply_list_;
    QCheckBox* rename_;
    QComboBox* rename_preset_;
    QLabel* rename_pattern_;
    QCheckBox* move_;
    QComboBox* move_preset_;
    QLabel* move_pattern_;
    QCheckBox* replaygain_;
    QComboBox* replaygain_mode_;
    QCheckBox* replaygain_skip_;
    QLabel* replaygain_hint_;
    QLabel* apply_note_;
    QProgressBar* write_progress_;
    QPushButton* write_stop_;
    QPushButton* apply_back_;
    QPushButton* write_;
    // The albums the last write was given, shown with how each went.
    std::vector<std::size_t> written_;
    bool syncing_{false};
};

} // namespace trackknife::bench
