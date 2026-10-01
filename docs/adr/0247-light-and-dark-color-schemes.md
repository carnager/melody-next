# ADR-0247: Trackknife ships a light and a dark colour scheme

## Status

Accepted, 2026-10-01. Revises ADR-0240's "colours come from the system
palette": that stays the default, no longer the only choice.

## Context

Both windows took their colours from the system palette. Qt gets a dark
palette only when the desktop hands one over -- KDE, qt6ct, GNOME through its
settings portal. On a laptop without that, Qt falls back to its built-in light
palette, and Trackknife came up light on a dark desktop, with no way to ask
for dark.

## Decision

- Settings › General › Appearance › **Colours**: *As the system* (the
  default), *Light*, *Dark* -- `appearance/color-scheme`: `system`, `light`,
  `dark`. Applied at once when Settings is saved, in every window.
- Light and Dark are complete palettes of Trackknife's own, every role in
  every state. Dark is the mockup's: dark grey grounds a shade apart, one blue
  accent, near-white text; Light is its counterpart. Inactive equals Active,
  so an unfocused window keeps its accent (macOS greyed it).
- *As the system* follows the desktop, and the desktop changing between light
  and dark -- except where the desktop says it is dark yet hands Qt a light
  palette: then the dark scheme stands in, so that case needs no choice.
- One implementation (`workspace/color_scheme`) decides and sets the
  application palette. The Qt Quick window puts it on every window as it is
  shown and again when it changes, since a Qt Quick window does not reliably
  follow the application's palette. The widgets window uses Fusion while
  Trackknife's palette is in use -- the desktop's style, Kvantum for one,
  paints its own colours -- and the desktop's style again when it is not.

### Icons and disabled controls (2026-10-01, after a first use)

- The icon theme follows the scheme. A desktop's dark icon theme draws light
  glyphs, light on Trackknife's light ground; the scheme in use picks the
  theme's sibling for its ground -- Papirus-Dark and Papirus-Light, breeze-dark
  and breeze -- among those installed, and the desktop's own again with its
  colours. Qt Quick asks for icons under a revision that changes with the
  theme, so they are drawn again at once.
- Disabled controls fade once. The Trackknife style fades a disabled control
  (`Theme.disabledOpacity`); its windows' palettes carry no separate disabled
  greys, which under that fade were unreadable. The widgets window, which
  shows disabled through the palette alone, has disabled text dark enough to
  read in Light and light enough in Dark.

## Consequences

- The window looks the same on any desktop when Light or Dark is chosen, and
  dark wherever the desktop is.
- Choosing Light or Dark gives up a desktop style's own look in the widgets
  window, by design: that look is what ignored the choice.

## Validation

`tests/bench_main_window_test.cpp` (`colorSchemesAreChosenAndApplied`): both
palettes complete, Dark chosen in Settings applies its palette and Fusion,
Light after it, and the system's colours back.
