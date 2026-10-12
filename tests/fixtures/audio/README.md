# Audio fixture provenance

These Base64 files materialize real encoded audio containers during the formats
test. They are text-encoded only so repository tooling can review and reproduce
the exact binary inputs; the test never invokes an `ffmpeg` subprocess.

## Logical ReplayGain chapter fixture

`loudness-chapters-mka.b64` contains two one-second mono chapters at 48 kHz,
with a 997 Hz sine at amplitudes 0.8 and 0.2. This repository-generated audio
distinguishes a chapter scan from an accidental whole-file scan. Offscreen
Properties tests ingest the actual chapters and compare track gains, sample
peaks, and album gain against direct logical-source scans. No runtime FFmpeg
subprocess is used by the tests.

It was generated using FFmpeg n9.0.1 in two steps: encode the floating-point
signal as FLAC in Matroska, then quantize to 16-bit FLAC to keep the fixture
small. The chapter metadata input was:

```text
;FFMETADATA1
album=Logical Gain Album
[CHAPTER]
TIMEBASE=1/1000
START=0
END=1000
title=Loud
[CHAPTER]
TIMEBASE=1/1000
START=1000
END=2000
title=Quiet
```

```sh
ffmpeg -f lavfi \
  -i 'aevalsrc=0.8*sin(2*PI*997*t)*if(lt(t\,1)\,1\,0.25):s=48000:d=2' \
  -i chapters.txt -map_metadata 1 -map_chapters 1 -c:a flac -bitexact source.mka
ffmpeg -i source.mka -map_metadata 0 -map_chapters 0 \
  -c:a flac -sample_fmt s16 -bitexact output.mka
```

The 44,668-byte binary SHA-256 is
`ccd40900b62cb0d51ab2822b7fd606cd57e72cf6b69b47aa899764ecec17eeec`.

## Gapless tone set

The source is exactly 4,800 mono samples (100 ms) of a 997 Hz sine at 48 kHz.
It was generated and encoded with FFmpeg n9.0.1 / libavcodec 63.1.101:

```sh
ffmpeg -f lavfi -i "sine=frequency=997:sample_rate=48000:duration=0.1" \
  -c:a pcm_s16le source.wav
ffmpeg -i source.wav -c:a aac -b:a 128k tone.m4a
ffmpeg -i source.wav -c:a libmp3lame -b:a 128k tone.mp3
ffmpeg -i source.wav -c:a libopus -b:a 96k tone.opus
```

| Encoded fixture | Binary SHA-256 | Relevant delay/padding metadata |
| --- | --- | --- |
| `gapless-tone-aac-m4a.b64` | `9f73d9159487333ba3acd16d12cdf614e9a4951a732cd40f1417c4710aaeef1a` | MP4 edit list; 1,024 initial AAC samples skipped |
| `gapless-tone-mp3.b64` | `096dcc043a0b1cbb10d2d67086c9b399b84b62e72d041aab555a7ab919c0b4a4` | LAME/Xing delay and padding; 1,105 initial and 1,007 final samples skipped |
| `gapless-tone-opus.b64` | `e69f0e640b80074e5774cc4bb17fa89aa2e8300da560df214a0bdf9cbf2a2aa7` | Opus pre-skip of 312 samples plus final granule trim |

Each fixture independently decodes to exactly 4,800 frames with FFmpeg's
documented packet skip-sample semantics. The Trackknife test additionally
requires a non-silent first and last window, preventing encoder priming or
trailing padding from passing merely because it happens to contain zeros.

## Tagged FLAC fixture

`tagged-tone-flac.b64` is 4,410 mono samples (100 ms) of the same 997 Hz sine
at 44.1 kHz with five Vorbis-comment tags. It was generated and encoded with
FFmpeg n9.0.1 / libavcodec 63.1.101 using `-bitexact` for reproducibility:

```sh
ffmpeg -f lavfi -i "sine=frequency=997:sample_rate=44100:duration=0.1" \
  -c:a pcm_s16le source44.wav
ffmpeg -i source44.wav -metadata title="Fixture Tone" \
  -metadata artist="Trackknife Project" \
  -metadata album="Trackbench Fixtures" -metadata date="2026" \
  -metadata track="3" -c:a flac -bitexact tagged-tone.flac
```

The binary SHA-256 is
`f126f1b8100ad2b80ed1d8fab3e19b6834d2cb7f42c1ec15fc7388f46b04ea41`.
The probe test requires the five tags to survive projection through
`formats::MediaProbe` and the complete decode to publish exactly 4,410
contiguous frames from sample zero with non-silent first and last windows.

## Rich multi-value FLAC fixture

`rich-metadata-flac.b64` reuses the tagged FLAC audio essence and replaces its
comments with 20 values covering repeated artists and credits, album/sort
fields, recording/release-track/release/release-group/artist/album-artist/work/
disc MusicBrainz identities, and a repeated arbitrary `CUSTOM_FIELD`. It was
produced with `metaflac` 1.5.0; each repeated `--set-tag` creates another
ordered Vorbis-comment value:

```sh
base64 -d tagged-tone-flac.b64 > rich-metadata.flac
metaflac --dont-use-padding --remove-all-tags \
  --set-tag='TITLE=Metadata Fixture' \
  --set-tag='ARTIST=First Artist' --set-tag='ARTIST=Second Artist' \
  --set-tag='ALBUM=Rich Metadata' --set-tag='ALBUMARTIST=Album Credit' \
  --set-tag='ARTISTSORT=Artist, First' \
  --set-tag='ALBUMARTISTSORT=Credit, Album' \
  --set-tag='ARTISTS=First Artist' --set-tag='ARTISTS=Second Artist' \
  --set-tag='MUSICBRAINZ_TRACKID=11111111-1111-1111-1111-111111111111' \
  --set-tag='MUSICBRAINZ_RELEASETRACKID=22222222-2222-2222-2222-222222222222' \
  --set-tag='MUSICBRAINZ_ALBUMID=33333333-3333-3333-3333-333333333333' \
  --set-tag='MUSICBRAINZ_RELEASEGROUPID=44444444-4444-4444-4444-444444444444' \
  --set-tag='MUSICBRAINZ_ARTISTID=55555555-5555-5555-5555-555555555555' \
  --set-tag='MUSICBRAINZ_ARTISTID=66666666-6666-6666-6666-666666666666' \
  --set-tag='MUSICBRAINZ_ALBUMARTISTID=77777777-7777-7777-7777-777777777777' \
  --set-tag='MUSICBRAINZ_WORKID=88888888-8888-8888-8888-888888888888' \
  --set-tag='MUSICBRAINZ_DISCID=fixture-disc-id' \
  --set-tag='CUSTOM_FIELD=first custom value' \
  --set-tag='CUSTOM_FIELD=second custom value' rich-metadata.flac
metaflac --dont-use-padding --remove --block-type=PADDING rich-metadata.flac
```

The 2,308-byte binary SHA-256 is
`ca321677795e922747cecca46bf49747fcfde3bb317566224c42866afef6b873`.
The read-adapter regression requires native exposed keys, exact repeated-value
order, the typed MusicBrainz projection, raw-byte paths, and a stable source
revision. The offscreen Trackbench regression additionally requires arbitrary
and MusicBrainz values to survive list-cache restart without persisting the
stale revision as future mutation authority.

## Embedded-artwork FLAC fixture

`art-tone-flac.b64` is the tagged FLAC tone with a 64×64 orange PNG attached
as its display picture, generated with FFmpeg n9.0.1 / libavcodec 63.1.101.
FFmpeg stored the native FLAC picture type as `Other`; artwork inventory tests
must retain that exact type rather than silently reclassifying the first image
as a front cover:

```sh
ffmpeg -f lavfi -i "color=c=orange:size=64x64:duration=0.04:rate=25" \
  -frames:v 1 cover-orange.png
ffmpeg -i tagged-tone.flac -i cover-orange.png -map 0:a -map 1:v \
  -c:a copy -c:v png -disposition:v:0 attached_pic -bitexact art-tone.flac
```

The binary SHA-256 is
`402d86b052891ee6d76c04357215087b5d457f13983afffab656195bbf4ef611`.
The artwork test requires the attached picture to surface as its exact PNG
bytes and the tagless lookup on `tagged-tone-flac.b64` to report a typed
not-found instead of inventing artwork.

`external-blue-jpeg.b64` is an 8×6 blue JPEG generated with FFmpeg n9.0.1 /
libavcodec 63.1.101. Its binary SHA-256 is
`f8503798d7a69951fe462ab5843e8342932c64770b2f0cd15295fc5ab848e29c`.
The artwork inventory test materializes it under an exact configured sibling
name and independently requires JPEG MIME and dimensions before replacing it
with malformed bytes for the typed-issue regression.

## Tagged WavPack fixture

`tagged-tone-wavpack.b64` is the same 4,410-sample 44.1 kHz mono tone in a
WavPack container with two APEv2 tags, generated with FFmpeg n9.0.1 /
libavcodec 63.1.101:

```sh
ffmpeg -i source44.wav -metadata title="Fixture Tone" \
  -metadata artist="Trackknife Project" -c:a wavpack -bitexact tagged-tone.wv
```

The binary SHA-256 is
`07b9afb934a54ca0113b056c68a8a7f7731b0c11c12e8d65456e4b53ce249073`.
The probe test requires both tags to survive projection and the complete
decode to publish exactly 4,410 contiguous frames from sample zero with
non-silent first and last windows.

## Tagged AIFF fixture

`tagged-tone-aiff.b64` is the same 4,410-sample 44.1 kHz mono tone stored as
big-endian 24-bit PCM in AIFF. Its five metadata values are carried in an ID3v2.4
chunk. It was generated with FFmpeg n9.0.1 / libavcodec 63.1.101 using `-bitexact`:

```sh
ffmpeg -f lavfi -i "sine=frequency=997:sample_rate=44100:duration=0.1" \
  -metadata title="Fixture Tone" -metadata artist="Trackknife Project" \
  -metadata album="Trackbench Fixtures" -metadata date="2026" \
  -metadata track="3" -c:a pcm_s24be -write_id3v2 1 -id3v2_version 4 \
  -bitexact tagged-tone.aiff
```

The binary SHA-256 is
`8d5d7ffc80b60766bf13349290dfcd448c009dcd30aa6d311126c3d3414143a4`.
The probe test requires the five tags, AIFF container, and `pcm_s24be` stream
shape to survive projection. Complete decode must publish exactly 4,410
contiguous frames, and a sample-bounded seek must exactly equal the same slice
of the complete decode.

## RF64 WAV fixture

`rf64-tone-wav.b64` is 2,400 stereo frames (50 ms) of a 997 Hz tone at 48 kHz,
stored as little-endian 24-bit PCM. FFmpeg was forced to write an RF64 header
for this deliberately small file so the repository can exercise the `RF64` and
mandatory `ds64` chunks without carrying a multi-gigabyte fixture. It also
contains BWF and INFO metadata. It was generated with FFmpeg n9.0.1 /
libavcodec 63.1.101 using `-bitexact`:

```sh
ffmpeg -f lavfi -i "sine=frequency=997:sample_rate=48000:duration=0.05" \
  -ac 2 -metadata title="RF64 Fixture" \
  -metadata artist="Trackknife Project" -c:a pcm_s24le \
  -rf64 always -write_bext 1 -bitexact rf64-tone.wav
```

The binary SHA-256 is
`53d2816ef99a1a278334078d8a43416f4f2ae33ab2ccae4c39d61d182a6ef2ee`.
The test independently checks the `RF64`, `WAVE`, and `ds64` signatures before
probing. It then requires the stereo 24-bit stream and metadata to survive,
exactly 2,400 contiguous frames to decode, and a bounded seek to equal the
corresponding interleaved slice of the complete decode.

## Wave64 float fixture

`wave64-float.b64` is 2,400 stereo frames (50 ms) of a 997 Hz tone at 48 kHz,
stored as little-endian 32-bit floating-point PCM in Sony Wave64. It was
generated with FFmpeg n9.0.1 / libavcodec 63.1.101 using `-bitexact`:

```sh
ffmpeg -f lavfi -i "sine=frequency=997:sample_rate=48000:duration=0.05" \
  -ac 2 -c:a pcm_f32le -bitexact -f w64 wave64-float.w64
```

The binary SHA-256 is
`7b07d75d2550d7837c58bfb3e17309d09e9cc51931566975740f11c67bc895e3`.
The test independently verifies the Wave64 RIFF and WAVE GUIDs, then requires
the float stereo stream shape, exactly 2,400 contiguous decoded frames, and a
bounded seek identical to the corresponding interleaved complete-decode slice.
Trackbench's folder-ingestion allowlist includes `.w64` alongside WAV and RF64.

## Container chapter fixture

`container-chapters-mka.b64` is 9,600 mono samples (200 ms) of a 997 Hz tone at
48 kHz, encoded as FLAC in Matroska. The container has two adjacent 100 ms
chapters, global album-like title/artist/date metadata, per-chapter titles, and
an artist override on the first chapter. It was generated with FFmpeg n9.0.1 /
libavcodec 63.1.101 from this metadata source:

```text
;FFMETADATA1
title=Chapter Album
artist=Album Artist
date=2026
[CHAPTER]
TIMEBASE=1/1000
START=0
END=100
title=First chapter
artist=First Artist
[CHAPTER]
TIMEBASE=1/1000
START=100
END=200
title=Second chapter
```

```sh
ffmpeg -f lavfi -i "sine=frequency=997:sample_rate=48000:duration=0.2" \
  -f ffmetadata -i chapters.ffmeta -map 0:a:0 -map_metadata 1 \
  -map_chapters 1 -c:a flac container-chapters.mka
```

The binary SHA-256 is
`d9a323eadaab30280aa7f27dee5f7917877c29063e66d838302c81e2e326e01a`.
The tests require the chapter boundaries to project to adjacent sample ranges
`[0, 4800)` and `[4800, 9600)`, preserve scoped metadata, decode to the exact
whole-file PCM concatenation, expand as two Trackbench rows, and survive list
persistence.

`partial-chapters-mka.b64` uses the same 9,600-sample source but carries only
the first `[0, 4800)` chapter. Its binary SHA-256 is
`8cfc56bd92a3da2907ecbd8b0ff27fa0771bab211b4754955e552a52fe347ad8`.
The negative regression requires this navigation-only table to remain an
ordinary whole-file probe result rather than hiding the unchaptered second
half.

## Codec-native tracker subsong fixture

`two-subsongs-mod.b64` is an original repository-authored 3,164-byte ProTracker
MOD (`M.K.`) named `Trackknife subsongs`; it contains no third-party musical
material. Its 31-sample header defines one 32-byte signed square-like waveform.
The two order entries reference separate patterns with different note periods,
and each pattern jumps back to its own order at row 4. libopenmpt consequently
reports the second otherwise unreachable order as a hidden subsong.

The binary SHA-256 is
`3ceed6acd7a73296ff343588ba64ee414f11c82f1690fd7191b978fceb834324`.
libopenmpt 0.8.8 reports two independently selectable 600 ms songs. FFmpeg n9.0.1
selects them through the `libopenmpt` demuxer's `subsong` option at 48 kHz
stereo. The regression requires distinct non-silent selected decode, exact
`[0, 28800)` logical ranges (excluding the demuxer's generated fade tail), one
gapless playback-ring crossing, Trackbench folder expansion, and SQLite
restoration of both selections.

## Vorbis timeline fixture

`vorbis-positive-start.b64` is 4,410 mono samples (100 ms) of the same 997 Hz
sine at 44.1 kHz. It was encoded by vorbis-tools `oggenc` 1.4.3 with a fixed Ogg
serial number:

```sh
ffmpeg -f lavfi -i "sine=frequency=997:sample_rate=44100:duration=0.1" \
  -c:a pcm_s16le source.wav
oggenc -s 12345 -q 4 -o vorbis-positive-start.ogg source.wav
```

The binary SHA-256 is
`fb683e56206f5f0f66bb3199a72630e5d1195de325870eaba1df004c52b5ac28`.
FFmpeg exposes its first decoded frame at timestamp 128 even though the logical
audition starts at sample zero; other valid Vorbis encoders can also expose
overlapping or gapped best-effort frame timestamps. The regression requires the
4,282 decodable PCM frames to be published on one contiguous logical timeline
starting at zero.

## Tagged MP3 fixture

`tagged-tone-mp3.b64` is a 0.1 s 44.1 kHz mono 997 Hz sine in an MP3
container with a leading ID3v2.4 tag, generated with FFmpeg n9.0.1 /
libmp3lame:

```sh
ffmpeg -f lavfi -i "sine=frequency=997:duration=0.1:sample_rate=44100" \
  -metadata title="Fixture Tone" -metadata artist="Trackknife Project" \
  -c:a libmp3lame -b:a 128k -write_id3v2 1 -id3v2_version 4 -bitexact \
  tagged-tone.mp3
```

The binary SHA-256 is
`05a2df905bb6d0398e7af6bb9451effc80f6252d9fb1006431b762aa3172bb9d`.
The MP3 writer qualification requires both tags to survive projection and
the MPEG audio region to stay byte-identical through a prepared-copy tag
write.

## Tagged Ogg Vorbis and Opus fixtures

`tagged-tone-vorbis.b64` (0.1 s 44.1 kHz mono 997 Hz sine, libvorbis q3) and
`tagged-tone-opus.b64` (the same tone at 48 kHz, libopus 64k) carry stream
tags `title=Fixture Tone` and `artist=Trackknife Project`, generated with
FFmpeg n9.0.1:

```sh
ffmpeg -f lavfi -i "sine=frequency=997:duration=0.1:sample_rate=44100" \
  -metadata:s:a:0 title="Fixture Tone" -metadata:s:a:0 artist="Trackknife Project" \
  -c:a libvorbis -q:a 3 -bitexact tagged-tone.ogg
ffmpeg -f lavfi -i "sine=frequency=997:duration=0.1:sample_rate=48000" \
  -metadata:s:a:0 title="Fixture Tone" -metadata:s:a:0 artist="Trackknife Project" \
  -c:a libopus -b:a 64k -bitexact tagged-tone.opus
```

Binary SHA-256:
`9cdff810ef048cc51d455e8467d1df92868426e9b6de01cbe0a2e4ce67bae65d` (ogg),
`89a2e4bdf0382fcd3b5bc9b6ef3afd395b735a1749deeca6738967e53bcef0ae` (opus).
The Ogg writer qualification requires the reread tags to match the plan
exactly and every logical packet except the comment packet to stay
byte-identical through a prepared-copy tag write.

## Tagged MP4/M4A fixture

`tagged-tone-m4a.b64` is a 2 s 44.1 kHz stereo 997 Hz sine encoded as AAC
in an MP4 container (`ftyp free mdat moov` layout, `moov` trailing) with
the standard atoms `©nam`, `©ART`, `©alb`, and `trkn`, generated with
FFmpeg n9.0.1:

```sh
ffmpeg -f lavfi -i "sine=frequency=997:duration=2" -ac 2 \
  -metadata title="Fixture Tone" -metadata artist="Trackknife Project" \
  -metadata album="Container Fixtures" -metadata track="3" \
  -c:a aac -b:a 128k -bitexact tagged-tone.m4a
```

The binary SHA-256 is
`a66fd41662cd3ce5d6b79e3397a6416940801c78f2cf72eb37532fed07ffa04a`.
The MP4 writer qualification requires the reread tags to match the plan
exactly, the `ftyp` and `mdat` boxes to stay byte-identical, and the
decoded PCM to be unchanged through a prepared-copy tag write; freeform
`----:com.apple.iTunes:NAME` items are created by the writer under test,
not stored in the fixture.

`loudness-tone-opus.b64` is a 0.6 s 48 kHz mono 997 Hz sine at half
amplitude encoded with libopus at 64 kb/s (long enough for one gated
loudness block), tagged title/artist, generated with FFmpeg n9.0.1:

```sh
ffmpeg -f lavfi -i "sine=frequency=997:sample_rate=48000:duration=0.6" \
  -af "volume=0.5" -c:a libopus -b:a 64k -bitexact \
  -metadata title="Loudness tone" -metadata artist="Trackknife Fixtures" \
  loudness-tone.opus
```

The binary SHA-256 is
`eefe0e497467a1fbed30f2a73e1c2003185b8c6ead20fff237e11429f8bb690f`.
ADR-0149 tests scan it to assert R128 Q7.8 proposals and write
`R128_TRACK_GAIN`/`R128_ALBUM_GAIN` comments onto materialized copies to
assert the decoder's -23 to -18 LUFS reference lift.

`empty-field-flac.b64` is `tagged-tone-flac.b64` with one Vorbis comment
whose value is empty, as other taggers leave them (ADR-0279). TagLib cannot
write an empty comment, so it was made with metaflac:

```sh
base64 -d tagged-tone-flac.b64 > empty-field.flac
metaflac --set-tag="EMPTYFIELD=" empty-field.flac
base64 -w 76 empty-field.flac > empty-field-flac.b64
```
