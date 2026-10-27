# ADR-0341: melody-cli says which engine, and helps one command at a time

## Status

Accepted, 2026-10-27.

## Context

melody-cli reaches an engine by `--server`, `$MELODY_SERVER`, this
computer's socket or one found nearby, and nothing said which one answered.
A shell exporting `MELODY_SERVER` sent every command to the home server
while the user thought they asked this computer's engine; `status` ended in
"on caprica" -- the speakers, read as the engine. There was no plain way to
say "this computer's engine" past the variable. And the help was one page of
some 140 lines; `play --help` said only "say album or track".

## Decision

- **`status` names the engine that answered** beside the speakers it plays
  on: `· engine gemenon · speakers caprica`.
- **`engines` marks the one in use** with a last column, `in use` (and
  `in_use` with `--json`), so the tab-separated columns before it stay as
  they were.
- **`--local`** reaches this computer's engine whatever `$MELODY_SERVER`
  says; with `--server` or `--engine` as well it is refused, as asking for
  two engines.
- **Help is an overview and a page per command.** `melody-cli`, `--help`
  and `help` print every command on one line, grouped -- playing, the
  queue, the library, playlists, your music's files, sharing, engines and
  setup -- with how the engine is chosen. `help COMMAND` and `COMMAND
  --help` print every entry the command's name appears in, in full, with
  examples (`play` resumes, plays an album, plays a list). `convert --help`
  keeps its own page of every option. Both come from one table.
- An unknown command says so and where the commands are listed.

## Verification

- `melody-cli`: the overview names the commands and how the engine is
  chosen; `help queue` and `play --help` give the command in full; an
  unknown command fails and says where the list is; `status` names the
  engine; `--local` reaches this computer's engine past `$MELODY_SERVER`,
  fails plainly when none runs, and refuses `--server` beside it.
