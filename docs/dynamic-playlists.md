# Dynamic playlists

Choose **File → Dynamic playlists**. The same editor works with this
computer's library or a server's, and each library keeps its own saved
definitions.

Choose **Library rules**, enter a name and a `tkq-1` rule, then **Refresh**:

- `genre HAS rock AND rating GREATER 6` — rock rated above three stars.
- `rating MISSING` — unrated tracks.
- `genre HAS jazz SORT BY %album%` — jazz ordered by album.

Ratings use 0–10, so 8 means four stars. Rules use the complete
[query language](query-language.md) on either library, because the engine
evaluates them the same way. A rule matching more than 20,000 tracks asks you
to narrow it; results are never a silent sample of a cut-off answer.

Set a maximum of 1–500 tracks and optionally shuffle. **Save definition** keeps
the rule/source. Saved rules refresh when selected and remain current while the
window is open. Local rules update after library scans, rating edits, and
committed tag/path changes. Changes arriving during a query trigger a fresh
evaluation. **Stop** cancels the current work and automatic refresh.
**Open snapshot in new tab** creates an ordinary playlist you can play or edit;
that snapshot stays stable as the dynamic definition changes. Results and
snapshot tabs show one line per track, without album grouping. Server results
also offer **Add to queue**. Refresh never replaces your playing queue.

Results support row/multi-selection, keyboard navigation, Enter/double-click
playback, ratings, Go to artist/album, local or explicitly mapped file tools,
Last.fm, Up Next, and copying/dragging into ordinary lists. Up Next shortcuts
act on the result selection, not the tab behind the editor. Outward drag is
copy-only; dropping into or manually reordering/removing dynamic results is
not a definition edit. Choose **Open editable snapshot…** for those operations.

Play captures the results as a playback list on the engine the library belongs
to. Later refreshes never rewrite it. Selection, current row, and scroll anchors survive refresh by raw source
identity and duplicate ordinal. Playing markers follow the source, including
duplicate occurrences within the captured playback context. Editing metadata
captures the selected inputs so a result refresh cannot retarget the operation.
Closed dynamic editors do not keep refreshing; persistent live autoplaylist tabs
remain future work.

History rules may also order results, for example
`ALL SORT DESCENDING HISTORY(playcount)`. Turn off **Shuffle results on refresh**
to retain query ordering. See [history ordering](query-language.md#history-ordering-adr-0216).

For Last.fm, add an API key under **Settings → Metadata services** using its
registration link. Choose similar tracks, a user's loved/top tracks, or a tag.
Enter the seed artist/title, username, or tag, then Refresh. These details are
sent to Last.fm when you request a refresh. Account authorization, scrobbling,
and Love/Unlove are configured separately under **Settings → Last.fm**; see
[Last.fm accounts](lastfm.md).

The response is matched to music already in the selected library. Missing
tracks are counted, duplicate matches are removed, and differing editions use
a deterministic path choice. Matching currently requires the same artist and
title apart from case, normalized Unicode, and whitespace; it does not guess
alternate credits or editions. Each refresh reads up to 500 provider candidates, independently of your
playlist length, and randomly selects from the library matches. Tracks outside
the previous result are preferred; previous tracks fill any remaining slots.
The previous selection is remembered across restarts, separately for each
definition and library.

**Shuffle selected tracks** also randomizes the order. With it unchecked, the
fresh selection keeps Last.fm’s ranking order. If your library has too few
matches to vary the selection, the result reports that all available matches
are included. Missing matches can leave fewer tracks than requested.

Local results come entirely from the index. Use Library Refresh first if new
files are not indexed. Closing the dynamic editor leaves saved definitions and
opened snapshots intact. Changing between local and MPD authority closes it;
reopen from File for the new library.
