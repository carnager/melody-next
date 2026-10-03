// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QFrame>
#include <QPointer>
#include <QStringList>

#include <functional>

class QCheckBox;
class QComboBox;

namespace trackknife::bench {

class TaggerSession;

// ADR-0238/0262: what Apply does, chosen in one small panel -- Save tags,
// Rename and Move by a naming layout into a destination, ReplayGain, the
// scripts -- over a tagger session, its choices the session's. The tag
// editor and Identify albums… both open it, so both apply alike.
class ApplyActionsPopover final : public QFrame {
    Q_OBJECT
  public:
    // What only the window opening it can do; none, not offered.
    struct Links {
        std::function<void()> loudness_sources;
        std::function<void()> script_editor;
    };
    ApplyActionsPopover(TaggerSession& session, Links links, QWidget* parent);

    // Above `anchor` (a footer button), else below it, inside the screen.
    void showAt(QWidget* anchor);

  private:
    void sync();
    void fillLists();

    QPointer<TaggerSession> session_;
    QCheckBox* save_tags_;
    QCheckBox* rename_;
    QComboBox* layout_;
    QCheckBox* move_;
    QComboBox* destination_;
    QCheckBox* replaygain_;
    QComboBox* grouping_;
    QCheckBox* skip_gain_;
};

// The ReplayGain groupings, as TaggerSession::setReplayGainGrouping counts.
[[nodiscard]] QStringList replayGainGroupingNames();

// Choose folder…: the engine's folder browser, or this computer's file
// dialog, a folder moved into without being saved. Test seam: a
// "trackknife-move-folder" property on `parent` or its window skips it.
void chooseMoveFolder(QWidget* parent, TaggerSession& session);

// A destination list's last entry, which is no destination.
[[nodiscard]] QString chooseFolderItem();

} // namespace trackknife::bench
