# ADR-0277: Long file jobs keep the computer awake

## Status

Accepted, 2026-10-11. Builds on ADR-0237 (engines do the file work).

## Context

A long list of files tagged from a Mac, the engine on a server: the Mac
went to sleep and the server never finished. Trackknife hands a large Apply
to the engine in parts of about half a megabyte of plan, each sent when the
last is done; asleep, it sends no more. The engine finishes the part it has
and waits for nothing -- but the rest is never asked for, and the window,
waking to a closed connection, does not learn what was written.

The structural answer is an engine that owns the whole job and a window
that can reattach to it. Until then -- and as a kindness beside it -- the
computer should not fall asleep by itself in the middle of the work.

## Decision

- **A hold per long job.** Tag writes (planning and Apply, in the editor
  and in album batches), ReplayGain measurement and conversion each take a
  hold for as long as they run, and let it go when they end, however they
  end.
- **One platform hold for all of them.** While any hold is taken, idle
  sleep is held off: on macOS an IOKit power assertion
  (`PreventUserIdleSystemSleep`), on Linux a logind inhibitor of `idle`,
  which desktops and idle daemons (hypridle, KDE) respect. Without a system
  bus or logind, nothing happens and the job runs as before.
- **Only idle sleep.** The person can still put the computer to sleep, and
  closing a laptop's lid still does: a job must not keep a laptop awake in
  a bag. The screen may still turn off.

## Consequences

- An unattended job on a computer left alone finishes.
- A lid closed or a sleep chosen still stops it, and a dropped connection
  still loses the parts not sent: the engine-owned job remains to be done.
- Tested: holds counted, one inhibitor however many, listed by logind while
  held and gone after, where logind answers.
