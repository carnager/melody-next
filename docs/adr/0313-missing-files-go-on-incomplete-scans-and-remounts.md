# ADR-0313: Missing files go on incomplete scans and remounted shares

## Status

Accepted, 2026-10-24. Changes when ADR-0126's cleanup of missing files runs,
and what it accepts as the recorded filesystem.

## Context

A friend retagged an album, deleted its "CD 2" folder, and pressed Refresh;
the deleted tracks stayed in the library, and playing them said the files
were not there. Two rules kept them:

- **All or nothing.** A scan of a library folder removed missing files only
  if it completed. One file anywhere in the folder that could not be read --
  one still being written by a tagger, one that changed during the scan, one
  with unreadable tags -- kept every deleted file's row, still marked
  available.
- **The recorded device.** A file is confirmed missing when its path is
  absent and its nearest surviving folder is on the device recorded when the
  file was scanned, so an unmounted share -- an empty mountpoint on another
  filesystem -- is not taken for an emptied one. But an NFS or SMB share
  mounted again, after a reboot, is often given another device number: from
  then on nothing deleted on it was ever confirmed, only marked unavailable.

## Decision

1. **An incomplete scan removes what is confirmed missing**, as a complete one
   does: confirmation checks each path itself, so a file that is there keeps
   its row whatever the walk missed, and a folder that could not be read
   leaves its files unconfirmed. Rows not seen and not confirmed missing are
   no longer marked unavailable by an incomplete scan, as before. A cancelled
   scan, or one whose library folder went away during it, removes nothing.
2. **Device moves seen in the scan count.** A file found still there, on
   another device than its stored revision names, shows that device's files
   moved: the scan keeps each such move, and a missing file's surviving
   folder may be on its recorded device or on one the scan saw that device's
   files move to. Where no file of the old device is seen -- the share is
   not mounted -- nothing is confirmed, as before.

Search and playing are unchanged: search already left out unavailable
tracks; the library shows them greyed, and playing skips them.

## Verification

- `local-library`: a deleted folder is removed by a scan made incomplete by
  an unreadable file, while the files there stay available; a cancelled scan
  removes nothing; files stored on another device number, one deleted, the
  rest seen on the new one -- the deleted one removed; files of a device none
  of which is seen stay, unavailable.
