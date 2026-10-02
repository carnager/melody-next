// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QList>

namespace trackknife::bench {

// ADR-0251: how large Trackknife draws itself, on top of what the desktop
// says its screens need -- a Retina or 5K Mac's 2x, a Linux desktop's 1.25
// or 1.5 -- which Qt applies by itself. 1.0 is as the desktop says; a desktop
// that says too little (X11 without a DPI setting) or a user who wants it
// larger chooses another. Settings › General › Appearance, as stored.
inline constexpr auto interface_scale_key = "appearance/interface-scale";

// The sizes offered, as factors: 1.0, "As the system", first -- what an
// unset choice shows -- then smallest to largest.
[[nodiscard]] QList<double> interfaceScales();

// What is chosen, from the settings file, before the application exists:
// 1.0 when nothing or something unusable is stored.
[[nodiscard]] double chosenInterfaceScale();

// Before QApplication is made: the chosen size as QT_SCALE_FACTOR, unless
// the environment already sets one -- which then wins, as Qt's own control.
// A change takes effect at the next start.
void applyInterfaceScale(int argc, char** argv);

} // namespace trackknife::bench
