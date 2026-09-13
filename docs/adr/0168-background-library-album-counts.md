# ADR-0168: Shared library presentation and background album counts

- Status: Accepted
- Date: 2026-09-13

## Context

The local library appeared roomier than MPD despite sharing its row renderer:
artist rows lacked secondary text, and local expansion animation was disabled.
The requested artist subtitle is an album count available before expansion.

## Decision

Library trees share MPD's existing row metrics, 32-pixel icon setting,
18-pixel indentation, and animated expansion through the common view. Local
artist counts appear on the secondary line as **1 album** / **N albums**;
album rows retain artist and track information. Local lazy expansion uses the
shared pending-expansion mechanism so loaded children, rather than a temporary
placeholder, are revealed by the animation.

Local artist pages count distinct indexed album keys in their existing bounded,
cancellable background SQLite query. This needs no schema change or file scan.

After MPD roots arrive, the session's query worker issues one grouped tag query:
`list Album group AlbumArtist group Date group MUSICBRAINZ_ALBUMID` (using
`Artist` when the root uses that tag). It transfers release identifiers, titles,
and dates rather than full tracks. Release IDs define identity where present;
otherwise album title and date define identity within the artist. Same-named
editions remain distinct; one release spanning dates/title variants counts once.
Custom non-artist roots retain their existing behavior.

MPD root rows show **Counting albums…** until results arrive. Unsupported or
failed counts are visibly unavailable, never reported as zero. Tokens reject
stale results after reload; pending count queries are cancelled on reload and
cleared on disconnect. Queries retain at most 500,000 response pairs / 32 MiB
and 100,000 distinct artist/release identities. Exceeding a bound fails the
whole count result rather than publishing partial counts. Loaded branches can
still supply their existing count if the background query is unavailable.

## Evidence and validation

The [MPD protocol list command](https://mpd.readthedocs.io/en/stable/protocol.html#the-music-database)
supports repeated tag grouping. A temporary stock MPD 0.24 database verified
the nested response order: last group first, then inner groups and album values.
Protocol tests preserve that response and cover edition separation and release
ID deduplication. Tree-model tests cover counts before expansion, singular/plural,
stale results, and failure. Local index tests cover distinct album counts;
workspace tests compare local/MPD tree presentation settings and exercise browsing.
