# ADR-0342: melody-cli -- one word for one thing, and shell completion

## Status

Accepted, 2026-10-27. Follows ADR-0341.

## Context

After ADR-0341 the rest of the usability review of melody-cli was open:

- `next` skipped alone and put something first in Up Next with words after
  it; `queue` put it last in Up Next -- not into the queue, which `add`
  does. Three words for where something goes, one of them meaning two
  things.
- `lists` named the saved playlists and `playlists` said where they are
  kept: two words that sound the same.
- `rate` was `rating` by another name. `love` (Last.fm) and `fav` (Melody's
  Favorites playlist) sat side by side without saying whose heart each is.
- `cover FILE [SIZE]` could only write a file.
- `config` is this computer's melodyd.conf, also when `--server` named
  another engine, which read as configuring that one.
- There was no shell completion, with some thirty commands and names --
  speakers, lists, layouts -- only the engine knows.

melody-cli has hardly any users besides Melody's own DMS plugin and rofi
script, both in this repository, so compatibility is not kept (**Melody
decision**).

## Decision

- **`next` only skips.** With words it is refused, pointing to `insert`.
- **`insert album|track WORDS`** puts it first in Up Next, to play after
  this track (as `mpc insert` does); **`upnext`** puts it last in Up Next.
  `add` appends to the queue as before; `queue` is gone.
- **`lists folder [DIR|default [--style S]]`** is the playlist folder;
  `playlists` is gone.
- **`rate` is gone**; `rating` and `albumrating` stay.
- **`lastfm love|unlove`** replaces `love` and `unlove`, so Last.fm's heart
  is named as Last.fm's; `fav` and `unfav` are Melody's Favorites.
- **`cover [--size PIXELS] [FILE|-]`**: without a file, or with `-`, the image
  goes to standard output -- refused when that is a terminal.
- **`config` refuses `--server` and `--engine`** on the line: it cannot
  reach another computer's file. `$MELODY_SERVER` alone does not refuse it,
  since a shell exporting it would otherwise lose `config`; the help says
  whose file it is.
- **Completion for bash and zsh**, installed with melody-cli where each
  shell looks (`/usr/share/bash-completion/completions/melody-cli`,
  `/usr/share/zsh/site-functions/_melody-cli`). Commands, their words and
  options are in the scripts; what only the engine knows is asked of it
  through a hidden `melody-cli __complete WHAT` -- `commands` (each with a
  few words for zsh to show), `engines`, `outputs`, `lists`, `layouts`,
  `presets`, and `config-keys` from the melodyd.conf skeleton -- with the
  `--server`/`--engine`/`--local`/password options of the line passed along.
  Names with spaces complete as one word. Nothing found is no output, never
  a prompt.

The DMS plugin and the rofi script use the new words.

## Consequences

- Scripts using `queue`, `next WORDS`, `playlists`, `rate`, `love` or
  `cover FILE SIZE` must change; the error for `next WORDS` names the new
  word, the others are unknown commands that point to `help`.
- The command list lives once in `help.cpp` with a summary each, which both
  `__complete commands` and the test that every command has a help page
  read.
- Fish completion is not written; the `__complete` interface is what one
  would use.
