# ADR-0165: Capture cached physical sources when an operation starts

- Status: accepted
- Date: 2026-09-13
- Extends: ADR-0164

## Decision

**Trackbench decision:** a local list's enriched cache row without a source
revision must not be passed unchanged into a metadata write plan. The workspace
marks such physical sources as requiring metadata capture. Opening Edit tags or
starting the context-menu ReplayGain operation resolves that marker on the
existing background worker, before building the staged selection.

The shared Qt-free metadata reader captures the current native document and its
revision together, using the existing cancellation and before/after revision
checks. Duplicate paths share the same captured document and revision. A read
failure aborts capture visibly. Decodable sources unsupported by the metadata
reader may capture filesystem identity for sidecar-only storage without claiming
a native tag baseline. Existing captured revisions are never silently advanced;
logical sources retain their separate overlay and source-validation rules.

The marker is transient preparation state, not a persisted schema field. Ordinary
revisionless read-only previews are not implicitly authorized to read files.
Capture completes before drafts or measured gains are applied, so fresh tags
supply grouping and native field identities and unrelated tags remain intact.
Apply still revalidates the captured source; opening search tabs remains database
only except for independent cover loading.

## Verification

Real-file reader tests cover native baseline replacement, repeated paths, missing
sources, cancellation, and retaining existing revision evidence. Workspace tests
run context ReplayGain from imported rows, revisionless cached WAV rows with
sidecar storage, and cached FLAC rows with embedded storage. The FLAC case also
opens Edit tags from an empty cache document and checks fresh revision/document
capture and preservation of unrelated tags after ReplayGain publication.

A MusicBrainz regression reproduces the false draft-conflict rejection when
canonical-only cached names (such as `musicbrainzworkid`) are mistaken for native
properties. Opening Edit tags now restores native names from the file first; the
same provider preview stages successfully and remains undoable.
