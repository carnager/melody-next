// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace trackknife::bench {

// ADR-0247, for the widgets window: Trackknife's own colours need a style
// that paints with the palette -- Fusion -- where the desktop's style
// (Kvantum, say) paints its own. Applies the scheme chosen now and keeps
// the style in step with every scheme applied after: Fusion under
// Trackknife's palettes, the desktop's style under the desktop's colours.
void followColorSchemes();

} // namespace trackknife::bench
