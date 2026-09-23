# ADR-0225: Cover size limits

- Status: accepted
- Date: 2026-09-23
- Extends: ADR-0184

## Decision

Settings → Covers has two limits on the longer edge of a newly written front
cover, in pixels: one for the embedded cover and one for the folder image.
Both default to no limit. A cover over its limit is scaled down (smooth
filtering, aspect kept) and saved as JPEG at quality 90, or PNG when it uses
transparency. A cover within its limit is written byte for byte, as before.

Each destination is converted from the original, never from the other
destination's copy, so a small embedded thumbnail does not become a blurry
folder image.

Only new writes are converted. Covers already in files are not touched, and
there is no bulk "shrink existing covers" action.

## Where it happens

The storage planner (`plan_artwork_storage`) converts the replacement before it
inspects it, through an `ArtworkImageFitter` the caller supplies. The planner
stays free of an image library, and the tagger's fitter uses Qt. Converted
covers are content-named drafts in the cover-draft cache, referenced by the
plan like any other replacement file, so the existing revision and
fingerprint checks apply to them unchanged.

Converting before inspection is what lets an oversized cover through at all.
A replacement input may be at most 16 MiB, but when a limit applies the
original may be up to 64 MiB (`maximum_fittable_artwork_bytes`), because
shrinking it is the point. A policy with a limit but no fitter is refused
rather than silently ignored.

## Why

Scans of 4000+ pixels and 15+ MB are common from archive sources. Embedding one
into every track of an album multiplies it by the track count and slows every
reader that decodes covers, while a folder image is read once and can
reasonably be larger. Hence the two separate limits.
