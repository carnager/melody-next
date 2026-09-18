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

- Install into a clean test account using the package artifact, not the build
  tree. Confirm the desktop entry and file opening.
- MPD: connect/reconnect, browse, search, queue mutations, scratch/stored lists,
  transport, volume, ReplayGain, and output selection against stock MPD.
- Melody: repeat the MPD flow, select the Trackknife endpoint, then exercise
  daemon/application/network reconnects, streamed and mapped-file playback,
  volume, ReplayGain, gapless preload, and natural advancement.
- Local: open folders/files/CUE logical tracks; play/seek/gapless/device hotplug;
  edit and identify tags; edit artwork; scan/store ReplayGain; rename/move;
  convert every shipped preset and verify output.
- Run keyboard-only and mouse-only passes in both authorities. Inspect names,
  roles, focus order, shortcuts, selection state, non-color change markers,
  scaling, and screen-reader announcements.
- Create a workspace backup while the app is active. Restore it on restart and
  verify profiles, tabs, layouts, local-library configuration, presets,
  settings, and retained rollback database.

## Release evidence

Record distribution/version, dependency versions, test output, sanitizer and
fuzz durations, benchmark/soak results, tested servers/audio devices/filesystems,
known limitations, and the exact artifact checksum. Do not turn an unperformed
manual item into a compatibility claim.
