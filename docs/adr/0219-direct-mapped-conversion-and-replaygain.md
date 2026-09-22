# ADR-0219: Direct mapped file tools and a complete ReplayGain surface

Status: Accepted

## Decision

Extend ADR-0203's direct mapped Edit tags workflow to Convert and ReplayGain.
The tools share lexical URI validation and the configured profile/global music
root. They never create or select a local scratch list. Only explicit Load as
local files keeps that behaviour. All server-list and library actions use this
shared dispatch; stock MPD and Melody need no new protocol capability.

Mapped selections identify physical files, not a request to rediscover folders
or implicitly expand chapters/subsongs. Local-list selections retain their
explicit logical identities. Conversion captures actual local metadata and
source revisions off-thread before opening the existing converter factory.
One capture job per window is admitted, with a 20,000-file bound, visible
progress and cancellation. Invalid mappings or failed reads refuse the operation
as a whole instead of silently dropping selected files. Duplicate occurrences
remain distinct and ordinary conversion collision checks still apply.

ReplayGain opens directly with immutable mapped paths and reuses the existing
cancellable capture/measurement/journaled-write workflow. Missing files are
reported during capture. No fabricated persistent list occurrence is necessary
for reconciliation, as already established by ADR-0203. This is explicit local
file access under a configured mapping, not server-side transcoding or remote
filesystem mutation. No Melody deployment is needed.

## ReplayGain presentation

Provide a resizable dialog with explicit Scan mode choices, an album-group
preview, a grouped storage/peak section, phase-labelled progress, and Stop.
Preview groups reads metadata on the capture worker and uses the same pure
album grouping as measurement; it writes nothing. At most 200 groups are
rendered with an explicit remainder count. Custom-expression errors are shown
before scanning. Grouping edits invalidate the displayed preview.

The primary action is Scan and write tags. The header and completion wording
make clear that audio samples are unchanged. This remains the existing direct
scan-and-write workflow, not a new result-review/approval boundary. Cancellation
before writing prevents publication; cancellation during writing retains already
committed targets under the existing journal semantics.

## Verification

Real-file UI tests cover mapped Edit tags/Convert/ReplayGain with no extra list
or active-tab change; complete capture, invalid paths, missing-file refusal and
cancelled conversion preparation; a read-only album-group preview; and a
successful ReplayGain write without any local list occurrence. Local embedded
and sidecar scans exercise the same dialog and writing pipeline.
