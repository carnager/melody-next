// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QQuickImageProvider>

namespace trackknife::quick {

class QuickWorkspace;

// "image://cover/<album key>": an album's cover as the workspace has it --
// the same images the widgets window draws. Asked for on the GUI thread
// (the Images using it are not asynchronous), where the workspace lives.
class CoverProvider final : public QQuickImageProvider {
  public:
    explicit CoverProvider(QuickWorkspace& workspace);
    QImage requestImage(const QString& id, QSize* size, const QSize& requested) override;

  private:
    QuickWorkspace& workspace_;
};

// "image://icon/<name>": the icons the widgets window uses, drawn by the
// same QIcon machinery -- a freedesktop theme name, falling back through
// "|"-separated names, or "sp:<StandardPixmap>" for the style's own, as
// in "media-playback-start|sp:SP_MediaPlay", or "tk:album-shuffle" for the
// window's own. "?disabled" draws the disabled mode, "?oneshot" marks a
// mode's one-shot with a dot.
class IconProvider final : public QQuickImageProvider {
  public:
    IconProvider();
    QPixmap requestPixmap(const QString& id, QSize* size, const QSize& requested) override;
};

} // namespace trackknife::quick
