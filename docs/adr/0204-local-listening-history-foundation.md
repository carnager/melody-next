# ADR-0204: Content-identified local listening history foundation

## Status

Accepted, 2026-09-21. This is the persistence foundation for roadmap area 7;
playback observation and user-facing statistics remain in progress.

ADR-0207 now implements observation and supersedes the metadata-derived key
proposal below with revision-qualified, publication-linked source identity.

## Decision

Persist local play counts, last-played time, and resume position against the
same metadata-derived track hash used by local ratings. This is a metadata
identity, not a fingerprint of the audio. Well-tagged tracks keep the same key
across path changes; files missing title/album tags use filename/folder
fallbacks and need explicit identity reconciliation before playback integration
can satisfy the relocation contract. Logical tracks also need distinct identity
qualification. Changing identity-bearing tags changes the key, matching the
existing rating contract. This first slice does not begin collecting history.

Schema 39 adds `local_listening_history`. Playback will submit explicit,
timestamped observations through the serialized persistence boundary. Resume
updates are monotonic, so a delayed worker result cannot rewind a newer saved
position. Equal timestamps are treated as repeated observations. Recording a
qualified play increments the count and advances `last_played_ms`; it clears
resume only if its observation is at least as recent as the stored resume.
Each record call counts once, so callers must deliver it once and must not
retry an ambiguous write. Durable occurrence deduplication is a prerequisite
for any future retrying playback integration.

This table records facts; it does not decide whether a listen counts. The
playback layer must separately specify and test its completion threshold before
it begins writing plays. Restart restoration must remain opt-in and must load a
paused position without starting audio. MPD/Melody history needs its own
authority decision: this local table must not pretend to own server listening
state.

## Validation

Repository tests cover initial absence, monotonic resume updates, accumulated
plays, completion clearing resume, invalid identities, reopening, schema backup,
and migration round trips. The development downgrade refuses to discard a
nonempty history table.
