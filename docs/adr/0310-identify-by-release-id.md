# ADR-0310: Identify looks up a release named by its id

## Status

Accepted, 2026-10-24. Extends the single-album Identify of ADR-0090 and
ADR-0162. The first of the steps ADR-0311 proposes towards one Identify.

## Context

- **Identify only searched.** The files' own MusicBrainz release id -- an
  album tagged before, opened again -- was not used: the person searched
  for the album the files already name, and picked it out of 25 versions.
- **A release could not be named.** When the search does not find the
  version wanted -- a pressing, a deluxe edition -- the person finds it on
  musicbrainz.org, and had no way to give Identify what they found.
  Identify albums looks a tagged id up already (ADR-0261); Identify did
  not.

## Decision

1. **The tagged release first.** Where every file chosen carries the same
   MusicBrainz release id, Identify looks it up as it opens and shows it as
   the one candidate, "Tagged release". Searching replaces it as before.
   Where the files differ or one has none, nothing is looked up.
2. **A link or id where the search is typed.** The Album field -- or the
   Artist field -- takes a release's link (`musicbrainz.org/release/<id>`,
   any page of it, `beta.` too) or a bare id: Search then looks that release
   up instead of searching, shown as "Release named". A release group's
   link is not a release and is searched for as text; one field, no mode
   to switch.
3. **Matched as a found one is.** The release looked up is a candidate like
   any other: "Match this version…" opens the matcher of ADR-0162.

## Verification

- `musicbrainz-web-service`: an id and a link found, lowercased, with a
  page and a query after it; a release group's link, a longer id, another
  host's link refused.
- `bench-main-window`: Identify on files tagged with a release looks it up
  without a search; a link typed as the album looks that release up; the
  one matched is staged.
