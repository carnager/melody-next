# ADR-0326: Bandcamp as a cover source

## Status

Accepted, 2026-10-25. Extends ADR-0091/0094 (Cover Art Archive) and
ADR-0316 (Identify fetches covers).

## Context

The Cover Art Archive has no front cover for many newer and independent
releases -- often the ones sold on Bandcamp. MusicBrainz links many
releases, or their albums, to their Bandcamp page; the About plugin
already shows those links.

## Decision

- **A cover source "Cover Art Archive, then Bandcamp"**, the default in
  Cover settings ("Cover Art Archive only" stays a choice).
- **Only where the archive has no front cover**, the release's Bandcamp
  album page is found among its MusicBrainz links -- else among its
  album's -- and the cover it names (its `og:image`) is offered at 1200
  pixels, as a front cover of the listing: fetched by Identify and Fetch
  cover…, shown in the picker as from Bandcamp. Written as every cover is
  (ADR-0317).
- **Never by searching Bandcamp**: a wrong cover is worse than none.
- **An engine fetches only Bandcamp album pages and their images**
  (`https://NAME.bandcamp.com/album/…`, `https://NAME.bcbits.com/img/…`),
  besides MusicBrainz and the archive. An album on a label's own domain
  is not reached.

## Consequences

- One or two MusicBrainz lookups and one page more per album the archive
  has no front for; none for the others.
- It reads a page, not an interface: Bandcamp changing its pages could
  stop it, and covers are then simply not found there.

## Verification

- `musicbrainz-web-service`: album pages and images told from lookalikes;
  a page's cover at 1200 and 350 pixels; a release's links parsed.
- `engine-file-work`: the engine fetches Bandcamp's album pages and covers,
  and refuses other Bandcamp pages and lookalike hosts.
- `bench-main-window`: an album the archive has no cover for gets its
  Bandcamp cover in Identify.
