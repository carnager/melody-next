# ADR-0297: Favourites

## Status

Accepted, 2026-10-19. Builds on ADR-0296: a favourite is a track in a
playlist, and the playlist a file.

## Context

- **Marking a track while it plays is the most common thing done to a
  library** after playing it. A rating says how good; a favourite says
  "this one", and is wanted as a list to play.
- **Playlists are now files every player reads** (ADR-0296), so a list of
  favourites is useful outside Melody too -- on MPD, on a phone.
- **Last.fm has loves**, which some keep as their favourites.

## Decision

1. **A favourite is a track in the saved playlist Favorites** of the engine
   holding it, made with the first favourite -- a file in its playlist
   folder like every saved list. A playlist named so by hand is the same
   one: renamed, it is no longer Favorites.
2. **The engine answers it** (`favorites.toggle`, `favorites.state`, level
   20), so the window, melody-cli (`fav`, `unfav`) and the phone do one
   thing. A track is in Favorites once; taken out, every entry of it goes.
3. **In the window, a heart beside the rating** of what plays, and
   Playback › Favourite (Ctrl+Shift+H): it says whether what plays is one
   and turns it over. The track it acts on is the one it shows, not
   whatever plays by the time the click arrives.
4. **Loved on Last.fm, when asked**: Settings › Last.fm › "Favourite tracks
   are also loved on Last.fm", off by default. Made a favourite, the track
   is loved with this window's sign-in; taken out, unloved.

## Consequences

- Favorites can be played, shuffled, exported and synced as any playlist.
- A love made elsewhere does not make a favourite: Last.fm is told, not
  asked.

## Verification

- `engine-playlist-folder`: the first favourite makes the playlist and its
  file; said; made again, once; toggled, out.
- `bench-main-window`: the heart for what plays, in and out of Favorites,
  hidden with nothing playing.
