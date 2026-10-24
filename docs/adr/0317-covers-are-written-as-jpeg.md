# ADR-0317: Covers are written as JPEG, at most 1000 px

## Status

Accepted, 2026-10-25. Changes ADR-0184's storage limits: their default, and
what is converted.

## Context

- The Cover Art Archive keeps scans: 4000×4000 JPEGs of nearly 9 MB, PNGs
  of several. Written as they are, they made files megabytes larger; for
  "The Real Thing" by Faith No More a cover picked stayed empty.
- The size limits for embedded covers and folder images were off by
  default, and a cover within a limit was written in whatever format it
  came: PNG, or one that keeps transparency.
- A cover from a file could only be a PNG or a JPEG.

## Decision

1. **What is written is a JPEG**, at quality 90, no larger than the limit
   on its longer side; transparency, where there is any, over white.
2. **The limits default to 1000 px** for embedded covers and folder images
   alike; set to No limit, a cover keeps its size but is still a JPEG.
3. **Anything Qt's image plugins read comes in**: JPEG, PNG, WebP, BMP,
   GIF, TIFF. A cover fetched from the archive, or added or replaced from a
   file, is converted when it comes -- off the UI thread, a JPEG shrunk as
   it is read -- and again at writing where a limit is smaller, or an
   embedded picture copied is no JPEG.

## Verification

- `artwork-fitting`: a BMP, a PNG and, where Qt reads it, a WebP become
  JPEGs within the edge, or of their size with no limit; what is no image
  is refused; transparency over white.
- `bench-main-window`: the archive's PNG fetched and written as a JPEG; a
  pasted image written as `folder.jpg`; the limits 1000 px until changed.
