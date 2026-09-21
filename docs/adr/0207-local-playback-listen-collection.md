# ADR-0207: Local playback listen collection

## Status

Accepted, 2026-09-21. Supersedes ADR-0204's proposed metadata-derived identity
for newly collected history. User-facing statistics, resume, and album shuffle
remain separate follow-ups.

## Decision

**Trackknife decision:** collect local listening facts without requiring tags,
a library scan, or a Last.fm account. Observe only the Local Queue player,
even while an MPD tab is selected. The Melody endpoint and MPD transport never
write local listening history. Nothing is written to audio files or sent online.

Reuse the pure `ListenAccounting` threshold: duration must exceed 30 seconds;
credit advancing playback for half its duration or four minutes, whichever is
less. Pauses, unavailable/suspended output, large seeks, stalls, and observation
gaps over five seconds do not earn credit. Position samples are conservative,
not proof that someone heard the audio. Unknown-duration sources do not count.
Each audio-worker playback instance qualifies once; repeats and gapless
handoffs receive separate instances. Last played records qualification time.

Schema 40 maps revision-qualified physical observations to a repository-owned
source ID. An observation hashes a length-prefixed raw path and the complete
filesystem revision; this is not an audio fingerprint. The track key combines
that source ID with explicit decoder selection and sample range. Thus CUE
ranges, chapters, alternate streams, and subsongs remain distinct. Identical
list occurrences share statistics; separate physical copies do not.

Verified metadata refresh and rename/move transactions link old and published
observations to the same source ID, alongside existing list/cache/index updates.
Both observations remain resolvable for delayed playback samples and recovery.
Links are established even before a source has history. Undo can reuse an old
observation; conflicting established identities fail rather than silently merge
counts. Reusing a pathname with a different revision does not inherit history.
Unverified external file changes start a new identity; automatic external
reconciliation and history import/export are not implemented.

Qualified events cross the existing serialized persistence worker, with at most
16 pending writes. Database work never runs in the GUI or audio callback.
An occurrence UUID, identity, timestamp, and count increment commit atomically.
An identical retry is a no-op; a changed replay is rejected. The UI currently
does not retry failures and shows an error, including admission overflow.
Normal shutdown drains accepted work; a crash before commit may lose that
listen. Partial listening credit is not restored across process restarts.
Occurrence evidence is retained, not automatically pruned.

Qualification is not end-of-track completion and does not clear resume state.
Schema-39 rows remain intact but are not guessed into the new identity space;
that version had no runtime collector. No persisted `tkfmt-1` or `tkq-1`
expression changes meaning. Resume collection/restoration and displayed or
queryable statistics are not included in this slice.

## Verification

Repository tests exercise restart/connection replay, changed-event rejection,
raw paths, replacement isolation, logical selections, metadata refresh, and
multi-step relocation with delayed observations. Migration round trips and
nonempty downgrade refusal protect identity and occurrence data. A workspace
test feeds deterministic player snapshots over a real fixture source through
the actual persistence worker, covering pauses, seeks, repeat occurrences,
suspended output, brief clips, and separate logical tracks. Existing pure
accounting tests cover long tracks and sparse samples.

Validation (2026-09-21): the full development build and all 69 CTest suites
pass, including offscreen workspace tests. Changed-file formatting, SPDX,
and diff checks pass. Live listening was not manually verified in this pass.
