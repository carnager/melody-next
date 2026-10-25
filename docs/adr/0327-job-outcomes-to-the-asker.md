# ADR-0327: A job's outcome goes to the client that asked

## Status

Accepted, 2026-10-25. Amends ADR-0222 (jobs report through events).

## Context

A job reports `job.finished` with its outcome, and every event went to
every client. A cover fetched for the tagger (`musicbrainz.fetch`, a job) is
its image's bytes: a 3.9 MB Cover Art Archive original became a 5.2 MB
event, sent to every client of the engine -- the tagger's window, the phone,
a desktop shell's watcher. Past 4 MB of unread events a client is let go
(the limit that protects the engine from a client that stopped reading), so
one event past it let them all go: the tagger saw "the engine closed the
connection" and fetched no cover, though the archive had one.

## Decision

- **The outcome is the asker's.** A server marks which connection each
  request it dispatches comes from; a job remembers the connection that
  started it. Its `job.finished` reaches that connection whole, as an
  answer to its own request would -- outside the limit on unread events --
  and every other client hears `job.finished` without the outcome.
- A job started by the engine itself, with no client asking, is told to
  every client as before.
- Progress events and every other event are unchanged.

## Verification

- `engine-server`: a 6 MB outcome reaches the client that started the job;
  another client hears it finished, without it; both stay connected.
