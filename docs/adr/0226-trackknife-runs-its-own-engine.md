# ADR-0226: Trackknife runs its own engine

- Status: accepted
- Date: 2026-09-23
- Extends: ADR-0220, ADR-0223
- Amended by: ADR-0227, which replaces "one engine per workspace" below

## Decision

**The engine owns everything.** Catalogue, ratings, history, lists, playback,
and every file change (tags, covers, convert, ReplayGain, moves) belong to the
engine, because every client (a phone included) must see the same ratings and
history, and because the reason for the engine is editing files on a NAS from
the machine the engine runs on, next to the files. Trackknife is a client that
also runs by itself: it is a remote that starts its own engine.

The model is MPD with direct file access: the engine plays where it runs, and
sound anywhere else comes from an output agent it streams to (Phase 4).
Trackknife is not a player. A desktop that wants the music on its own
speakers while the engine is elsewhere runs an agent, and Trackknife starts
its own agent for that case. This answers the question Phase 4 left open.

~~**One engine per workspace.**~~ *Withdrawn by ADR-0227: this computer's
engine is always there, and a remote one is added beside it, not instead of
it. Connecting to a remote engine had stopped local playback, which was never
the intent.*

**This computer's engine is started when needed.** Trackknife connects to
`$XDG_RUNTIME_DIR/melodyd.sock`. If nothing answers, it starts `melodyd`
detached, in its own session, with output going to `melodyd.log` in the data
directory, and waits up to ten seconds for it to listen. The engine outlives
the window: music keeps playing after it closes, and the next start
reconnects. If the engine stops while the window is open, the playback
connection's reconnect starts it again.

The program is `$TRACKKNIFE_ENGINE`, else the `melodyd` beside the
executable (installed, or the build tree's `src/daemon`). `PATH` is never
searched: another program of the same name there is not this engine. The Go
`melodyd` was one, and a test started it.

## The database

`melodyd` opens one file, `lists.sqlite`, for the catalogue and the workspace
store. They always shared a schema. Its default state directory is the one
Trackknife always used, `$XDG_DATA_HOME/trackknife/trackknife`. The first
engine therefore adopts the existing library, ratings and history in place,
with no import step. The separate `library.sqlite3` and `workspace.sqlite3` of
earlier engine builds are no longer read.

An engine holds an exclusive lock on its state directory (`engine.lock`) and
another on its socket (`<socket>.lock`) for its lifetime. Two engines on one
database would both play and both answer for the same ratings, and two
started at once for one socket could each find it stale and take it from the
other. Of two workspaces starting an engine at the same moment, one engine
wins the lock, the other exits, and both clients connect to the winner.

Starting an engine is switched on by the application's `main()` and is off
otherwise. A test must not leave a daemon running, nor reach the one serving
the real database. An earlier guard relied on tests enabling Qt's test mode,
and one that didn't started an engine.

## Transition

**Trackknife has no player.** The in-process audition service, the client's
playback order, gapless bookkeeping, listening credit, resume checkpoint and
Last.fm sampler of its own are gone. The window renders the engine's state and
sends it commands. With no engine connected, nothing plays and the transport
says so. The "restore playback on startup" setting went with the resume
checkpoint: the engine outlives the window and restores its own queue, paused,
when it restarts.

**Output and buffer are the engine's.** `playback.set_output`,
`playback.refresh_outputs` and `playback.set_buffer` choose the engine
machine's sink and buffer. The state document reports the devices, the default
sink, availability, suspension, underruns and whether a buffer change is
pending. The engine persists the choice (`playback.output.v1`); the window
mirrors it into its settings only so the dialog shows the engine's value.

**Asks are held apart from the list.** A track sent to Up Next from outside
the playing list is held in a pool of asks, not appended to the queue. It is
playable and requestable, but it is not a list entry: it takes no part in the
order or in consume, and a client mirroring the queue does not show it as a
row. Each Up Next ask has its own identity, so the same track asked for twice
plays twice. An ask that ends by itself returns to where the list was, as
pressing Next already did.

The in-process catalogue remains as a fallback when no engine could be
started, and for tests. It goes next, once those tests run against an engine.
The file-changing work moves into the engine after that. Until it does, the
tagger writes files from the client, which is correct only for this
computer's engine. Relocating a file re-sends the list so the engine's queue
names the new path; the playing track keeps its open file.

**The engine is `melodyd`.** It was built as `tkengine` so that an existing
install of the Go `melodyd` would not be replaced by accident. The one that
remained (in `~/.local/bin`) has been retired, so the engine takes its planned
name now rather than after Phase 6: the binary, `melodyd.sock`,
`melodyd.service` and `melodyd.log`. The launcher never searches `PATH`
(above), so a same-named program elsewhere is never started in its place.
