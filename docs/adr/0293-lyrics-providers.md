# ADR-0293: Lyrics providers

## Status

Accepted, 2026-10-17. Builds the provider ADR-0288 designed (its decision
5), first for lyrics, with two of its open questions answered: where it
runs, and whether it keeps running.

## Context

- **Melody reads lyrics where the files are** (ADR-0276): an `.lrc` beside
  the track, ID3 `SYLT`, the lyrics tags. Most libraries have few.
- **Services have them** -- LRCLIB, synced, free, no account -- and which to
  ask is the person's choice, so it belongs in a plugin.
- **ADR-0288 sketched a provider as a program that stays running** and
  answers requests, and left open whether it runs in the window or in the
  engine.

## Decision

1. **A plugin may declare providers**:
   `"providers": [{"id": "lyrics", "provides": "lyrics", "run": ["./lrclib"],
   "timeout": 45}]` -- `provides` is `lyrics` for now, `timeout` 1 to 120
   seconds, 30 by default.
2. **Asked only when Melody has none:** when the engine finds no lyrics for
   what plays, or cannot read them for it (a file outside its library), the
   enabled lyrics providers are asked in turn -- the order Settings lists
   them -- and the first to find lyrics is shown. A provider that fails, or
   finds none, passes to the next; when none finds any, what the engine said
   is said. A newer track stops the asking.
3. **A run for each question, as actions and panels are run** -- not a
   program kept running. Starting one is a few tens of milliseconds beside
   a web request; it needs no lifecycle, no restarts, no protocol of its
   own. A provider keeps what it found itself, as the official one does.
4. **In the window**, as actions and panels are: the engine runs no one
   else's programs. A phone shows only the lyrics in the files.
5. **It is told and answers:**

   ```json
   {"schema": 1, "provide": "lyrics", "provider": "lyrics",
    "engine": {…}, "settings": {…},
    "playing": {"path": "…", "title": "…", "artist": "…", "album": "…",
                "album_artist": "…", "duration_ms": 190000, …}}
   ```

   `{"lyrics": "<LRC or plain text>", "source": "LRCLIB"}`, or
   `{"lyrics": null}` for none. The text is read as an `.lrc` is
   (`parse_lyrics`): timed lines are synced, followed as the music plays.
   The panel says where they came from: "Synced · from LRCLIB".
6. **Nothing is written:** what a provider finds is shown, never saved into
   the files or beside them.
7. **The official one** is LRCLIB (`plugins/lrclib`): looked up by artist,
   title, album and length, else searched for the same length give or take
   two seconds, synced first; kept a month, what it has not a day; asked
   again after a wait when busy (429, 503). It ships with Melody, off until
   enabled.

## Consequences

- Lyrics a service has appear without a file to manage; the files stay as
  they were.
- A provider is as easy to write as a panel: the same input, a smaller
  answer.

## Verification

- `plugins`: providers read from a manifest or refused; answers read --
  lyrics, null, refusals; what a provider is told; the official LRCLIB
  provider answering from what it keeps, asking nothing of the network.
- `bench-main-window`: a file without lyrics, two providers -- the first
  finding none, the second's lines shown, with where they came from.
