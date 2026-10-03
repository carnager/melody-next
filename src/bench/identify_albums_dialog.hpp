// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "bench/musicbrainz_lookup.hpp"

#include <QDialog>

class QButtonGroup;
class QLabel;
class QProgressBar;
class QPushButton;
class QTextBrowser;
class QTreeWidget;

namespace trackknife::bench {

class AlbumBatchSession;
class TaggerSession;

// ADR-0261: Identify albums… -- the albums of the files open in a tagger,
// grouped, looked up in the background, and the clear matches staged.
class IdentifyAlbumsDialog final : public QDialog {
    Q_OBJECT
  public:
    IdentifyAlbumsDialog(TaggerSession& tagger, MusicBrainzLookupService service,
                         QWidget* parent = nullptr);

    [[nodiscard]] AlbumBatchSession* session() const { return session_; }

  private:
    void sync();
    void showDetail();
    [[nodiscard]] int selectedAlbum() const;
    [[nodiscard]] bool shown(int album) const;

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
    bool syncing_{false};
};

} // namespace trackknife::bench
