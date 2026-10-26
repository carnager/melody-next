# ADR-0332: Covers of other releases of an album, and albums in the file list

## Status

Accepted, 2026-10-26. Extends ADR-0091/0094 (Cover Art Archive),
ADR-0316 (Identify fetches covers) and ADR-0326 (Bandcamp covers).

## Context

The Cover Art Archive often has no cover for one release of an album
while another release of it -- a reissue, another country's pressing --
has one. And a cover is staged for the files selected: with the files of
several albums in the tagger (after Identify, all of them are selected),
there was no quick way to select one album's files to choose its cover,
by file or from the archive, or to tag one album by hand.

## Decision

- **A release without a front cover of its own gets its album's.** Each
  source is asked only while none has been found, in this order: the
  release's own Bandcamp page, the archive's cover for its release group
  (`coverartarchive.org/release-group/ID`: the front cover of the release
  MusicBrainz shows for the album), then the release group's Bandcamp
  page. Bandcamp only when Cover settings allow it; the release group's
  cover regardless. In the picker it is "Cover Art Archive · another
  release"; Identify's Fetch covers takes it as it takes any front.
- **An engine answers a release group's listing as a release's**: JSON,
  cached, a 404 an empty listing.
- **The tagger's file list shows the files under their albums** when
  they hold more than one -- grouped as Identify groups them (ADR-0261),
  by the tags the files carry, each album's title and artist above its
  first file. Clicking a title selects that album's files; with Ctrl, it
  adds them. One album: no titles, as before.
- **No album chooser in the cover's menu.** Choose file…, Fetch cover and
  a dropped image stay "for the files selected"; the file list is where
  an album is selected.

## Consequences

- One request more (cached) per album whose release has no front cover.
- An album's files that are not next to each other in the list get their
  title once, above the first; clicking it still selects them all.

## Verification

- `musicbrainz-web-service`: a release group's listing URL, ids checked.
- `engine-file-work`: a release group's kept error page is not served as
  its listing.
- `bench-main-window`: Identify fetches an album's cover from another
  release of it, asked only for the album without Bandcamp; the file list
  titles two albums, a title selecting its album's files, Ctrl adding.
