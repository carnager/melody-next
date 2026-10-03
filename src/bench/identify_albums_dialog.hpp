// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "bench/musicbrainz_lookup.hpp"
#include "workspace/tagger_session.hpp"

#include <QDialog>
#include <QPointer>

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

class QButtonGroup;
class QLabel;
class QStackedWidget;
class QProgressBar;
class QPushButton;
class QTextBrowser;
class QTreeWidget;

namespace trackknife::bench {

class AlbumBatchSession;
class TrackMatchSession;

// ADR-0261/0262: Identify albums… -- the albums of the files open in a
// tagger, grouped and looked up at once, the clear matches staged, those
// needing a person reviewed, and the albums chosen written as the tagger's
// Actions say.
class IdentifyAlbumsDialog final : public QDialog {
    Q_OBJECT
  public:
    // Over the files open in a tag editor, and its draft.
    IdentifyAlbumsDialog(TaggerSession& tagger, MusicBrainzLookupService service,
                         QWidget* parent = nullptr);
    // By itself (Tools): a tagger of its own over the tracks, without the
    // editor's window.
    IdentifyAlbumsDialog(std::size_t track_count, MetadataPropertiesSourceReader reader,
                         std::span<const std::string_view> fields, TaggerServices services,
                         QWidget* parent = nullptr);

    // None until the files are read, opened by itself.
    [[nodiscard]] AlbumBatchSession* session() const { return session_; }
    [[nodiscard]] TaggerSession* tagger() const { return tagger_; }

    // Review an album needing a person, or the next that does.
    void review(std::size_t album);
    void reviewNext();

  protected:
    void closeEvent(QCloseEvent* event) override;

  public slots:
    void reject() override;

  private:
    explicit IdentifyAlbumsDialog(QWidget* parent);
    void begin(MusicBrainzLookupService service);
    // Whether closing may go on: not while writing, nor, when nothing else
    // holds them, over staged albums the person keeps.
    [[nodiscard]] bool mayClose();
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
    AlbumBatchSession* session_{nullptr};
    bool owns_tagger_{false};
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
    QTreeWidget* versions_;
    QWidget* match_holder_;
    TrackMatchSession* match_{nullptr};
    QWidget* match_view_{nullptr};
    std::optional<std::size_t> reviewing_;
    // The review has opened by itself once.
    bool reviewed_{false};
    bool syncing_{false};
};

} // namespace trackknife::bench
