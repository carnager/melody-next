# ADR-0309: About takes the artist's albums from a dropwatch

## Status

Accepted, 2026-10-24. Brings back, changed, the dropwatch support ADR-0301
removed from About: dropwatch says which albums the artist has, no longer
which are missing.

## Context

- **About's missing albums are slow for an artist seen the first time.**
  MusicBrainz is asked a page a second, after the artist and album; with
  covers, a new artist takes several seconds before anything is missing.
- **A dropwatch has every watched artist's albums at hand**, MusicBrainz's
  and Discogs's merged and kept fresh by its own schedule: one local
  request instead of several to MusicBrainz.
- **ADR-0301 took dropwatch out for a reason that still holds**: dropwatch
  knows the library only as of its last sync, Melody always.

## Decision

1. **About has a dropwatch address and API token**, beneath Missing albums,
   empty by default. Set, the artist's albums and EPs are dropwatch's list
   for the artist -- found by its MusicBrainz id, else by name -- none it is
   told to ignore.
2. **Only the list.** Which of them are missing is worked out as before,
   against the artist's albums Melody tells the panel (ADR-0301), by
   release group, else by title; dropwatch's own "missing" is not asked.
3. **MusicBrainz stays the fallback**: for an artist dropwatch does not
   watch, and for a dropwatch that cannot be reached -- which is said above
   the missing albums. About never asks dropwatch to watch an artist.

## Verification

- `plugins`: with an address, dropwatch asked for its artists and the
  artist's releases with the token; an ignored one left out, an album the
  library has left out, a Discogs-only one linked to its master; not
  reached, said, and MusicBrainz's list kept a day used instead.
