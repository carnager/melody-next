# ADR-0346: melody-pick, Quick album and Quick track on their own

## Status

Accepted, 2026-10-31.

## Context

Quick album and Quick track (Ctrl+Shift+A, Ctrl+Shift+T) live in the window:
they search the library and put what is chosen in a tab. For a key binding
outside Melody there is `melody-rofi`, built on `melody-cli` -- but rofi
needs X11 or a Wayland compositor, so on macOS there was nothing. A flag on
`melody` itself was considered and set aside: it would work only through the
running window (one Melody per workspace, ADR-0267), bring the window's focus
and start-up questions with it, and fail where `melody-rofi` works with no
window open.

## Decision

- **`melody-pick [albums|tracks]`**, a program of its own in the `melody`
  package, shows the window's picker -- the same widget, rows, keys and
  colour scheme -- as a frameless window in the middle of the screen. It
  closes on Escape, when the focus leaves it, or once a choice is queued; a
  choice that fails is said in its status line, and it stays open.
- **The engine is the one `melody-cli` would use**, from the same options
  and environment: `--server` (`$MELODY_SERVER`), `--engine NAME`, `--local`,
  `--password`/`--password-file` (`$MELODY_PASSWORD`, `$MELODY_PASSWORD_FILE`);
  without any, this computer's engine when it runs, else the one nearby.
- **A choice goes to that engine**, as `melody-cli` puts it: Enter adds it
  after what is queued, Shift+Enter replaces the queue and plays, Ctrl+Enter
  plays it next, Ctrl+Shift+Enter puts it last in Up Next. There are no tabs,
  so Alt+Enter (a new tab in the window) does nothing and is not listed. A
  window that is open shows the change, as it shows `melody-cli`'s
  (ADR-0269).
- **One implementation.** Which engine, the tracks for a picked key and how
  they are queued moved out of `melody-cli` into `melody_cli_engine`
  (`src/cli/engine_access`), which both programs link; `melody-cli`'s
  behaviour is unchanged.
- `melody-pick` reads the window's settings for its colours and size and
  writes nothing.

## Consequences

- macOS gets a keyboard picker; bind `melody-pick --engine gemenon` (or
  `--server`) to a key with the system's or a launcher's shortcuts.
- `melody-rofi` stays for those who prefer rofi.
- On macOS this computer's engine usually runs while Melody does, so without
  options the picker searches it; name the engine that holds the library.
