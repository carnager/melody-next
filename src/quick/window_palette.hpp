// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QObject>

namespace trackknife::quick {

// Qt Quick windows take their colours from their own palette, not reliably
// from the application's: each is given the application's -- the colour
// scheme chosen (ADR-0247) -- when it is shown, and again whenever that
// changes, by Settings or by the desktop.
class WindowPalettes final : public QObject {
    Q_OBJECT

  public:
    explicit WindowPalettes(QObject* parent = nullptr);
    // Every window open now, afresh.
    void recolorAll();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
};

} // namespace trackknife::quick
