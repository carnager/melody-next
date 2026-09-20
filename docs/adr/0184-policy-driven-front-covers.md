# ADR-0184: Policy-driven front covers and journaled folder images

- Status: accepted
- Date: 2026-09-20
- Extends: ADR-0056/0057, ADR-0160, ADR-0183/0185
- Supersedes: ADR-0160's external-image write boundary for the policy destination only

## Decision

**Trackbench decision:** Settings → Covers controls embedding (default on),
writing a front-cover folder image (default off), its filename (default
`cover.jpg`), and the explicit fetch provider (Cover Art Archive). Conversion's
`convert/embed-artwork` preference is independent. Both destinations disabled
blocks artwork publication with an actionable error.

The compact Fields cover shares Artwork's revision-qualified inventory and
selected-file scope. It shows the first front image, or an explicit mixed/missing
state. Drop, paste, Choose file, and Fetch stage a single replacement front per
selected file, preserving other roles. Embedding replaces all existing embedded
front variants; folder-only storage leaves the audio file untouched. Remove
stages embedded-front removal. External image deletion remains unsupported.
Artwork keeps per-file rows, roles, archive browsing, export, and problems.

The storage planner captures policy and destination revisions before Apply.
`ArtworkWritePlanIntent` remains unchanged. A ready source plan adds the
captured folder destination and embed flag. Folder-only front additions need
no qualified embedded writer. The exact source revision is still required.
Prepared container writers reject folder-bearing plans; only the operations
committer can publish the separate file.

A filename must be a plain sibling basename. Its extension follows the encoded
image: `.png` for PNG, `.jpg` for JPEG. No image recompression occurs. Exact
paths and create/replace decisions are shown in a separate review, whose Save
commits that immutable plan. Settings changes cannot retarget the review.
Different images targeting one shared destination block the whole plan; identical
images from several tracks produce one physical publication. An already matching
image is an idempotent success. Other sibling images are untouched.

Folder publication runs off the UI thread. It opens parent directory components
without following symlinks, uses descriptor-relative filesystem operations and
a directory lock, and rejects non-regular or multiply linked replacement
objects. It revalidates donor bytes and destination revision/hash, durably
journals the planned publication, writes and syncs an exclusive same-directory
prepared image, and records its revision. Creation uses atomic no-replace rename;
replacement retains the old inode and uses atomic exchange. Recovery verifies
both identity and image hash, finishes recognizable publications, removes only
recorded prepublication artifacts, and retains ambiguous evidence for
reconciliation. A crash before the prepared revision is recorded conservatively
retains that artifact. Unsupported atomic-publication operations fail safely.

Schema 38 adds the folder-image content kind to the existing operation journal.
Only hashes, raw paths, revisions, and operation evidence are stored; image
blobs are never database records. The migration is transactional; downgrade's
narrower CHECK refuses to discard folder records. Replacements use the existing
retained-backup lifecycle and headless undo; creation has no previous image to
restore. Restart recovery dispatches folder records without projecting them as
media metadata.

A folder image and its media file are separate publications, not a cross-file
transaction. Folder publication precedes the media write; if that later write
fails, the result explicitly says the folder image was saved. Retrying identical
folder bytes is safe. Cancellation stops new work and can leave earlier files
committed, as with other batches. For this wave, folder-cover saves must precede
Rename/Move; the editor blocks combining those actions, and the container writer
rejects bypassing the folder committer. Moving an album's related images as one
transaction remains separate work.

Pasted images are encoded on a worker into content-addressed draft cache files.
Thumbnail decoding is bounded to 32 megapixels and source images to 16 MiB; only
small previews stay in the editor. Settings never trigger an online fetch.

## Verification

Real-file core tests cover folder-only media-byte preservation, journaled create
and replace, exact retained backups and replacement undo, shared-folder
idempotency, conflicting fronts, changed destinations, symlink rejection,
cancellation, interrupted publication and prepublication recovery. UI tests
exercise policy round trips, conversion-policy independence, Fields inventory,
drop/paste staging, explicit destination review, and both combined and folder-only
Apply. Existing embedded artwork and unified tag/artwork tests remain gates.
