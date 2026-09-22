# ADR-0218: Explicit opt-in permanent conversion gain

Status: Accepted

## Decision

The user reported confusing the converter's Apply album ReplayGain option with
output analysis/tagging. This control changes output PCM using existing values;
it does not perform a scan. Source files are not overwritten.

Superseding ADR-0173's gain-setting restoration, every new conversion dialog and
every preset selection starts with permanent gain off. Legacy last-used and
saved-job gain fields are ignored, not reinterpreted, and newly saved jobs omit
them. Encoder, naming and other job settings retain their existing semantics.

Label the control Permanent volume adjustment and explicitly name permanent
volume changes in both choices. Show a bold inline warning whenever enabled.
Every gain-enabled Convert requires a second confirmation whose default and
Escape action are Cancel. Cancellation starts no worker and publishes no files.
The warning distinguishes changing audio samples from calculating gain tags,
and explains that deleting tags cannot reverse a volume change. Recreate outputs
from original sources when the processing was unwanted.

Foobar2000's converter separately documents ReplayGain in Processing and
ReplayGain-scan output files as albums:
<https://wiki.hydrogenaudio.org/index.php?title=Foobar2000%3AConverter>.
The latter remains a separate missing feature here, not a claim of this fix.

This is shared local-file conversion UI, including explicitly mapped Melody
selections. Remote-only tracks remain unavailable for local conversion; Melody
playback ReplayGain settings and server files are not changed.

## Validation

Real-file conversion UI tests verify old settings cannot enable gain, warning
visibility, Cancel as the default with no output, explicit confirmation followed
by the measured PCM volume change, and reopening with gain off. Core converter
semantics are unchanged.
