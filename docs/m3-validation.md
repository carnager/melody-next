<!-- SPDX-License-Identifier: GPL-3.0-only -->

# M3 validation

M3 is **Complete**. The automated gates and the live checks below passed
against Melody and stock MPD on 2026-08-26.

## Automated evidence

The 2026-08-26 gate passed:

- all 19 development tests, including the scriptable MPD protocol server and
  native workspace tests;
- all 19 AddressSanitizer/UndefinedBehaviorSanitizer tests;
- the warnings-as-errors GCC build, Clang static-analysis build, SPDX check,
  and clang-format check;
- the one-million-row offscreen interaction benchmark, with every measured p95
  inside its documented budget. Exact results are in
  `../benchmarks/results/2026-08-26-m3-offscreen-smoke.md`.
- a read-only probe of the live Melody server at `gemenon:6600`: protocol
  0.23.5, 92 commands, 7 tag types, 18 queue occurrences, 4 outputs, and the
  advertised exclusive-output extension all projected successfully.
- a regression test that disconnects both session sockets while the command
  worker is idle and proves that the idle worker wakes it for an autonomous,
  generation-guarded reconnect without a user command.

## Live acceptance pass

Build and launch with:

```sh
cmake --preset dev
cmake --build --preset dev
./build/dev/src/bench/trackknife
```

On both Melody and stock MPD:

1. Connect from an untouched profile, restart Melody, and confirm automatic
   reconnect. Create a second profile and switch to it with one Server-menu
   action.
2. Browse Folders, Albums, Artists, Genres, and Playlists. Open focused rows
   with Enter and with double-click. Confirm album/artist indexes do not turn
   into the folder listing after a status update.
3. Use live search from the field and results list. Exercise append, add-next,
   replace, `Shift+Enter` result tabs, and continued result paging.
4. Play, pause, seek, change volume and modes, and edit the live queue by mouse
   and keyboard: multi-select, add-next/end, reorder, priority, remove, crop,
   and clear.
5. While Melody stays open, change playback and reorder/delete the queue in
   another client. Melody must converge without replaying a rejected edit.
6. Create, load, edit, reorder, rename, clear, and delete a server playlist when
   the daemon advertises the corresponding commands.
7. Create two scratch/named lists, copy and move duplicate tracks between them,
   reorder and pin tabs, close/restart, and confirm order, duplicates, dirty
   markers, pin state, and column widths survive.

On Melody additionally confirm the Outputs popup shows online/primary detail
and that one-click exclusive switching selects the requested endpoint without
breaking the standard additive-output behavior used with stock MPD.

## Results

- Melody version: **MPD protocol 0.23.5; hands-on pass green on 2026-08-26**
- Stock MPD version: **0.24.14; hands-on pass green on 2026-08-26**
- Defects found: **one fixed**. Restarting a daemon closed both sockets, but if
  no command was queued only the `idle` worker noticed; the sleeping command
  worker could therefore wait indefinitely instead of reconnecting. The idle
  failure path now requests an authoritative refresh, which wakes the command
  connection so it observes the failure and enters the normal bounded
  reconnect path. The socket-disconnect regression above covers the defect.
- Compatibility notes: Melody correctly capability-gated stored-playlist item
  edit/reorder/clear/rename because that server advertised `save`, `load`, and
  `rm`, but not the corresponding edit commands. Stock MPD exercised the full
  advertised stored-playlist lifecycle. Melody's exclusive output switch and
  stock MPD's additive output toggles both passed.
- Shared-server cleanup: Melody's original 18-track order, paused current song
  and elapsed time, volume, playback modes, and `caprica` primary output were
  restored exactly; all temporary validation playlists were removed.
