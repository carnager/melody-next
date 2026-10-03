# Release measurements

The 1.0 criteria in `MILESTONES.md` ask for a library of about 66,000 tracks
on a NAS engine to be measured end to end, with no step stalling the window,
and the numbers kept with the release.

## 2026-10-03, ahead of 1.0

The setup: gemenon, a NAS running the packaged `melodyd` (protocol 1, level
1) over 66,841 tracks of FLAC on its own disks; caprica, the desktop running
Trackknife built from the same `main`, reaching gemenon over the LAN.

### The engine

Measured on gemenon against its own socket, so without the network.

| What | Time | Answer |
| --- | --- | --- |
| The library's top level (artists) | 46 ms | 0.05 MB |
| Search `ALL`, the first 200 rows | 168 ms | 0.03 MB |
| Search `ALL`, every path | 204 ms | 7.7 MB |
| Word search "love" | 489 ms | 0.3 MB |
| Random album / ten random artists (ADR-0258) | 1.6 s / 1.4 s | -- |
| Keep a search of everything as a list (`list.from_query`) | 1.8 s | -- |
| Open that list (`list.get`) | 0.47 s | 24 MB |
| Describe its first 2,000 rows (ADR-0259) | 91 ms | 2.4 MB |
| Describe the remaining rows, in parts | 3.0 s | -- |
| Delete the list | 0.41 s | -- |
| Read tags of 256 random files from disk | 15.7 ms a file | -- |
| The same 256 again, cached by the OS | 0.1 ms a file | -- |
| ReplayGain scan of a random album, true peak (10 tracks) | 3.6 s | -- |

Reading every file's tags from disk would take about 17 minutes; the tagger
does not wait for it, opening on the library's tags (ADR-0257).

### The window

On caprica, the first start after the window stopped keeping its own copy
of the lists (ADR-0259), which moved its tabs to the new store -- among them
a kept search of all 66,841 tracks, a tab cache of 103 MB.

| What | How it went |
| --- | --- |
| Starting, the tabs moved to the new store | fast |
| Keeping a search of `ALL` as a tab | the window stayed responsive |
| Playing from that tab | at once |
| The tagger on all 66,841 rows | opened at once |
| ReplayGain of an album | worked |
| Converting an album | worked |
| Starting again, the tabs read from their caches | a slight delay |

The slight delay of the second start is the 103 MB cache of the kept search
being read, off the window's thread: about a second in a debug build
(ADR-0259). Compressing the cache is the remedy if it grows to matter.

### Builds

The release preset with makepkg's flags (`-O3`, LTO, `_FORTIFY_SOURCE=3`,
`_GLIBCXX_ASSERTIONS`) on GCC 16.2.1 passes the suite, 101 of 101, as the
debug build does.
