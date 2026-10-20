# ADR-0301: A panel is told the playing track's tags, and the artist's albums

## Status

Accepted, 2026-10-20. Extends what a panel of ADR-0291 is given. About's
missing albums, from dropwatch since ADR-0300, are worked out by About
itself.

## Context

- **A panel was told seven fields** of what plays -- path, title, artist,
  album, album artist, date, track number. About had to find the artist
  and album on MusicBrainz by name -- a search, slower than anything else it
  does, and wrong where two artists share a name -- although the files
  usually carry their MusicBrainz ids.
- **A panel knew nothing of the library.** Which of an artist's albums are
  missing was dropwatch's to say, and dropwatch knows the library only as
  of its last sync: an album just added stayed missing until the next.
- **A plugin could reach the engine itself**, through melody-cli. That
  would make each plugin find the engine, hold its password where it is
  elsewhere, and be tested against one; and it would be a second way to the
  library beside the one Melody has.

## Decision

1. **A panel is told every tag of the playing track** the library has:
   `playing.tags`, each field named as the query language names it -- a
   field Melody knows folded, lowercase without spaces, `_` or `-`
   (`musicbrainzalbumartistid`), a tag it does not know as it is spelled,
   lowercase (`custom_field`) -- to a list of its values as written.
   Asked of the engine playing it, when the panel is asked; a file outside
   the library has none.
2. **A panel that says `"library": ["artist-albums"]` is told the playing
   artist's albums** in the library: `artist_albums`, one for each release
   group -- or album title and album artist, where there is none -- with
   its title, album artist, date, folder, track count and the tags every
   track of it shares. The artist is the album artist, else the artist; by
   MusicBrainz id too where the track has one. Asked only for panels that
   say so; at most 2,000 tracks are read.
3. **Melody asks; the plugin does not.** The library is queried by the
   window, through the engine, as for lyrics; a plugin is a program of its
   input, tested without an engine.
4. **About works out the missing albums itself**: the artist's albums and
   EPs on MusicBrainz -- not compilations, live albums or other secondary
   kinds -- that are not among `artist_albums`, matched by release group,
   else by title; on Discogs, with a token, for an artist MusicBrainz does
   not know. It looks the artist and album up by the ids in the tags where
   they are there. Dropwatch support goes; dropwatch's own plugin is for
   dropwatch.

## Verification

- `plugins`: `library` read and refused; the artist's query, names with
  quotes in them; the albums made of tracks, by release group and by title,
  their shared tags; the input with and without them; About's missing
  albums from a MusicBrainz answer and a library, kept a day, without asking
  anything where the tags name the artist.
- `bench-main-window`: a panel told the playing track's tags, and the
  artist's albums from the engine's library when it asks.
