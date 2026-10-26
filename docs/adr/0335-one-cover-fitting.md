# ADR-0335: One cover fitting; lossy conversions fit their covers

## Status

Accepted, 2026-10-26. Changes ADR-0131 (conversion artwork carriage) for
lossy output; ADR-0317's rules kept, its Qt implementation replaced.

## Context

- A conversion embedded the source's cover byte for byte. FLAC albums
  carry scans -- a 3000×3000 JPEG of 1.5 MB -- and every Opus track made
  from them carried it: a quarter of a 6 MB file. A lossy copy is made to
  be small.
- Covers were fitted by two implementations: the tagger's, in Melody with
  Qt (ADR-0317), and the engine's, with FFmpeg, for phones and Together.
  A third use -- conversion -- would have been a third place to choose.
- An engine's conversion (`convert.apply`) knew nothing of Cover settings.

## Decision

- **One fitting**, in the formats library with FFmpeg: decoding JPEG, PNG,
  WebP, GIF, BMP and TIFF; refusing more than 32 megapixels; turning a
  photo upright by its EXIF orientation; transparency over white; scaled
  smoothly, aspect kept. Two rules over it:
  - **for writing** (ADR-0317): a JPEG at quality 90 no larger than the
    limit, or of its size with no limit; a JPEG that fits as it is;
  - **for showing** (phones, Together): within a size and a byte budget.
  The tagger's cover writes and fetches, and conversions, use the first;
  Qt's image code for covers is gone.
- **Lossy conversion output carries its cover within the embedded-cover
  limit** of the asker's Cover settings (1000 px by default) by the rule
  for writing; lossless output, and lossy with No limit, as it is.
- `convert.apply` takes the limit as `artwork_edge` (protocol level 25),
  `melody-cli convert` as `--cover-size` (default 1000).
- Where conversions run is ADR-0336's: they move into the engine on the
  asker's computer.

## Verification

- `artwork-fitting`: ADR-0317's cases through the one fitting -- a scan to
  1000×925, a cover that fits left alone, transparency over white, BMP,
  PNG, WebP and TIFF -- and the eight EXIF orientations shown as Qt shows
  them.
- `audio-convert`: an Opus with a 16-pixel limit carries a JPEG of at most
  16 pixels, its picture type kept; with no limit, and as FLAC, the
  source's cover byte for byte.
- `cover-fitting`: the rule for showing, from its new library.
