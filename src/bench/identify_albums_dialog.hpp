// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "bench/musicbrainz_lookup.hpp"

#include <QDialog>
#include <QPointer>

#include <cstddef>
#include <optional>

class QButtonGroup;
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

// ADR-0261/0262: Identify albums… -- the albums of the files open in a
// tagger, grouped and looked up at once, the clear matches staged, those
// needing a person reviewed, and the albums chosen written as the tagger's
// Actions say.
class IdentifyAlbumsDialog final : public QDialog {
    Q_OBJECT
  public:
    IdentifyAlbumsDialog(TaggerSession& tagger, MusicBrainzLookupService service,
                         QWidget* parent = nullptr);

    [[nodiscard]] AlbumBatchSession* session() const { return session_; }

    // Review an album needing a person, or the next that does.
    void review(std::size_t album);
    void reviewNext();

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
    void showDetail();
    void syncReviewStatus();
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
    QPushButton* actions_;
    QPushButton* write_;
    QWidget* bottom_bar_;
    QStackedWidget* pages_;
    QLabel* review_heading_;
    QLabel* review_place_;
    QLabel* review_status_;
    QPushButton* accept_;
    QListWidget* versions_;
    QWidget* match_holder_;
    TrackMatchSession* match_{nullptr};
    QWidget* match_view_{nullptr};
    std::optional<std::size_t> reviewing_;
    // The review has opened by itself once.
    bool reviewed_{false};
    bool syncing_{false};
};

} // namespace trackknife::bench
