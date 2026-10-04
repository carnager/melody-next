# Trackknife release checklist

This is the acceptance record for an M10/public build. A release is not ready
when only compilation succeeds.

## Source and legal

- The tree passes `git diff --check`, `scripts/check_spdx.sh`, and
  `format-check`.
- `THIRD_PARTY.md`, the GPL-3.0-only license, and dependency versions match the
  actual dynamically linked binary (`ldd` plus package metadata).
- Qt is dynamically linked and replaceable. No modified or statically linked
  Qt copy is distributed.

## Builds and automated verification

- Development, release, ASan/UBSan, and static-analysis configurations build.
- Every functional test passes offscreen; parser/evaluator fuzz smoke and the
  million-row UI benchmark pass their checked budgets.
- A soak run uses `TRACKKNIFE_SOAK_LOG` and shows no sustained resident-memory,
  QObject, underrun, or event-loop-lateness growth during combined playback,
  search, tag, ReplayGain, and conversion work.

## Packaged workflow acceptance

- Install into a clean test account using the package artifacts, not the
  build tree: `trackknife-git`, `melodyd-git`, `melody-agent-git`,
  `melody-cli-git`, `melody-watch-git`. Confirm the desktop entry, file
  opening, and that a second start hands its files to the running window.
- This computer's engine: Trackknife starts it; it keeps playing with the
  window closed. Library folders, Refresh, browsing, search, kept searches,
  lists, Up Next, transport, volume, ReplayGain, gapless preload and natural
  advance.
- An engine elsewhere, over TCP with its password: the same flow, then the
  connection lost and regained from both ends, an engine of an older level
  said as such, and its library panel following changes made by another
  client or reported by `melody-watch`.
- Outputs: the engine's own speakers, an agent on another machine, a UPnP
  renderer, and moving playback between them.
- File work on each engine, on a local disk and on the NAS: open folders,
  files and CUE logical tracks; edit and identify tags, one album and many;
  edit artwork; scan and store ReplayGain; rename and move; Undo this batch;
  convert every shipped preset and verify output. Album folders hold nothing
  but music afterwards, wherever undo copies are kept.
- Run keyboard-only and mouse-only passes. Inspect names, roles, focus order,
  shortcuts, selection state, non-color change markers, scaling, both colour
  schemes, and screen-reader announcements.
- Create a workspace backup while the app is active. Restore it on restart and
  verify profiles, tabs, layouts, library configuration, presets, settings,
  and the retained rollback database.

## Release evidence

Record distribution/version, dependency versions, test output, sanitizer and
fuzz durations, benchmark/soak results, tested servers/audio devices/filesystems,
known limitations, and the exact artifact checksum. Do not turn an unperformed
manual item into a compatibility claim.
