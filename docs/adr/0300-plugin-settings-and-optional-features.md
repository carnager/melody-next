# ADR-0300: Plugin settings in a dialog, and optional features

## Status

Accepted, 2026-10-20. Changes where the settings of ADR-0288 are set, and
adds two keys to a setting in `plugin.json`. Decision 4, About's dropwatch
support, is superseded by ADR-0301: About works out the missing albums
itself.

## Context

- **Every plugin's settings stood open on the Plugins page**, under its
  checkbox: a page that grows with each plugin, most of it fields set once.
- **A plugin could not have a part that is optional.** About and dropwatch's
  missing albums belong together -- the artist playing, and what of theirs
  is not in the library -- but most people have no dropwatch, and its
  address and token would stand in About's settings as though it needed
  them.

## Decision

1. **A plugin with settings has a Settings… button** on the Plugins page.
   It opens a dialog of the plugin's settings; OK puts what was set into
   the draft, which Settings' Save writes with the rest, and Cancel leaves
   it as it was. A setting's help is said beneath it.
2. **`requires`: a setting can belong to an optional feature** -- a toggle
   listed before it, named by its key. It stands beneath the toggle,
   indented, greyed until the toggle is ticked. The program is told every
   setting as before; reading the toggle is its part.
3. **`link`: a setting can name a page about it**, `http` or `https`, shown
   as a link beneath it: for a feature, where to get what it works with.
4. **About gains Dropwatch support**, off by default: ticked, beneath the
   artist and album, the artist's albums dropwatch knows of that are not in
   the library -- the artist found by its MusicBrainz id, which dropwatch's
   are. It replaces the separate dropwatch About in `melody-plugins`.

A `requires` that names no earlier toggle, or a `link` that is no web
address, makes the plugin unusable, as any key out of place does.

## Verification

- `plugins`: `requires` and `link` read, and refused when they name no
  earlier toggle or no web address; the official About with Dropwatch
  support asks a dropwatch for the artist by its MusicBrainz id, with the
  token, and draws the missing albums oldest first; without it, asks
  nothing.
- `bench-main-window`: the Plugins page has a Settings… button and no
  fields; the dialog greys a setting until its feature is ticked and links
  the feature's page; Cancel keeps nothing, OK and Save keep all.
