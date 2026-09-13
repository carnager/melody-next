# ADR-0161: Retry unfinished metadata files from feedback

- Status: accepted
- Date: 2026-09-13
- Extends: ADR-0160

## Decision

**Trackbench decision:** partial tag/artwork saves offer **Retry failed / stopped
files** in the feedback window. The retry contains only explicitly failed or
cancelled physical sources without a commit result. Successful sources are
excluded, including successes from earlier retries. No automatic retry occurs.

The retry retains the original immutable text/artwork plan, occurrence indexes,
source revisions, image fingerprints, and replacement evidence. The existing
commit executor rechecks the source and replacement inputs before publication;
it does not silently adopt external edits. Unfinished journal operations for the
same raw path block a new metadata save until recovery resolves them.

Feedback reports failures as needing attention, rather than promising that
ambiguous recovery failures left files untouched. Retrying dismisses feedback
without triggering the editor's normal close-after-save behavior. The close
button reads Close editor when earlier files were saved.

This first retry control covers physical tag/artwork saves. Rename/move batches,
CUE sheets, and loudness sidecars retain their existing feedback workflows;
rebuilding their interdependent plans needs separate qualification. Users must
reopen and review files whose source revision has changed. Retry does not merge
external edits or perform recovery automatically.

## Verification

Real-file GUI tests cover partial tag-and-cover publication, failed and stopped
sources, preservation of the successful file without a second attempt, eventual
successful retry, and rejection after external modification. Core tests verify
that an unfinished journal record prevents another publication, followed by
recovery and undo of the original operation.
