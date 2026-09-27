# Sources and research notes

> The MPD and Melody references below informed the retired MPD client
> ([ADR-0224](adr/0224-retire-the-mpd-backend.md)); they matter again only if
> the MPD bridge (Phase 5 of the [unified engine](unified-engine.md)) is built.

Accessed 2026-08-23 unless noted. These links describe observable behavior and
standards; they are not licenses to copy proprietary implementation code or
branding. ADR-0008 superseded the original foobar2000 scripting-compatibility
research; those links remain useful background rather than a language oracle.

Since ADR-0025 this register serves two applications. The MPD, Melody, and
Cantata references inform Trackknife, the pure MPD/Melody client; the
foobar2000, metadata, ReplayGain, playback, and conversion references inform
Trackbench, the separate local-file workstation. The register entries below
predate the split and are kept as-is.

## Source quality

- **Official**: foobar2000 site/FAQ, IETF RFCs, EBU/Xiph specifications, library
  project documentation. Prefer these for claims they cover.
- **Community reference**: Hydrogenaudio Knowledgebase. It is the most complete
  public behavioral reference, but some pages document older versions. Verify
  edge cases against a named current foobar2000 reference build.
- **Reverse-engineered**: classic FPL descriptions. Useful for understanding the
  rich cached-item model, never normative for all FPL/FPLite versions.

## MPD and Melody (accessed 2026-08-24/25)

- [Official MPD protocol specification](https://mpd.readthedocs.io/en/stable/protocol.html)
  - Normative for greeting/response framing, command lists, idle subsystems,
    queue song IDs and playlist versions, browse/search, stored playlists,
    outputs, binary artwork, and server errors.
  - The specification recommends song-ID commands over position commands when
    multiple clients may edit the live queue.
- [Official libmpdclient site](https://www.musicpd.org/libs/libmpdclient/)
- [Official libmpdclient API](https://www.musicpd.org/doc/libmpdclient/)
  - Provides maintained synchronous command parsing plus low-level asynchronous
    protocol I/O. Trackknife initially uses complete blocking operations only
    on bounded background session workers.
- Local Melody reference: `../melody/README.md`,
  `../melody/melodyd/mpd.go`, and `../melody/melodyd/mpd_commands.go`.
  - Melody uses the standard MPD command surface for library, live queue,
    transport, and outputs. Its advertised additions include `switchoutput` and
    output primary/online/stream-format/bitrate fields.
  - A clean temporary Melody daemon on 2026-08-25 accepted Trackknife's probe,
    advertised 92 commands and 7 tag types, and exposed `switchoutput`. The
    empty fixture intentionally had no queue items or registered output agent.

## Cantata interaction reference (accessed 2026-08-25)

- [Archived Cantata source repository](https://github.com/CDrummond/cantata)
  - Cantata 2.5.0 was its final release. Its documented grouped play queue and
    Qt Widgets implementation inform interaction structure, not copied source,
    branding, or a compatibility promise. Its cached artist/album/track library
    hierarchy also informed ADR-0019's interaction target; Trackknife instead
    uses bounded lazy MPD branch queries to preserve its authority boundary.
- [Cantata interface screenshot](https://audio-file.org/wp-content/uploads/2022/03/cantata.png)
  - Reference for the compact transport/artwork/metadata header, album-grouped
    queue hierarchy, current-track accent, and queue-summary status strip.

## foobar2000 overview and formats

- [foobar2000 overview and current main features](https://www.foobar2000.org/?page=Overview)
- [foobar2000 for Windows downloads](https://www.foobar2000.org/windows)
  - Listed 2.25.10 as the latest stable version on 2026-08-23; this is a
    candidate conformance profile, not yet an adopted reference environment.
- [foobar2000 native audio format list](https://www.foobar2000.org/formats)
- [foobar2000 official FAQ](https://www.foobar2000.org/FAQ)
  - Of particular relevance: FPL is intentionally internal/non-editable and is
    designed to carry necessary track information and load quickly.
- [foobar2000 component repository](https://www.foobar2000.org/components)

## Title formatting and queries

- [MusicBrainz Picard scripting documentation](https://picard-docs.musicbrainz.org/en/v2.13/extending/scripting.html)
- [MusicBrainz Picard source](https://github.com/metabrainz/picard)
  - Picard's open `%field%` / `$function(...)` language informed the familiar
    surface syntax selected by ADR-0008. Trackknife does not copy its parser or
    promise Picard script compatibility.

- [Title Formatting Reference](https://wiki.hydrogenaudio.org/index.php?title=Foobar2000:Title_Formatting_Reference)
- [Title Formatting Introduction](https://wiki.hydrogenaudio.org/index.php?title=Foobar2000:Title_Formatting)
- [Title Formatting Examples](https://wiki.hydrogenaudio.org/index.php?title=Foobar2000:Titleformat_Examples)
- [Query syntax](https://wiki.hydrogenaudio.org/index.php?title=Foobar2000:Query_syntax)
- [Album List title-format behavior](https://wiki.hydrogenaudio.org/index.php?title=Foobar2000:Titleformat_Album_List)
- [Official foobar2000 SDK](https://www.foobar2000.org/SDK)
  - The 2025-03-07 public source distribution exposes a UTF-8 comparison helper
    that uses one-code-point lowercase comparison rather than full case folding,
    plus helpers that count decoded UTF-8 characters and convert character
    counts to byte offsets. These support implementation hypotheses; they are
    not black-box results from the title-format engine.

- Local reference installation: foobar2000 package 2.25.10-1, with the bundled
  syntax reference at `/usr/share/foobar2000/doc/titleformat_help.html`.
  Independently restated documentation cases may cite it, but the document
  itself should be archived only if its redistribution license permits that.
  Documentation-derived cases still do not replace black-box observations.

## Metadata and file tools

- [Properties and multi-file metadata editing](https://wiki.hydrogenaudio.org/index.php?title=Foobar2000:Properties)
- [Automatically Fill Values](https://wiki.hydrogenaudio.org/index.php?title=Foobar2000:Properties/Automatically_Fill_Values)
- [Masstagger actions and ordered scripts](https://wiki.hydrogenaudio.org/index.php?title=Foobar2000:Components/Masstagger_%28foo_masstag%29)
- [File Operations](https://wiki.hydrogenaudio.org/index.php?title=Foobar2000:File_Operations_%28foo_fileops%29)
- [ID3 field/frame mapping](https://wiki.hydrogenaudio.org/index.php?title=Foobar2000:ID3_Tag_Mapping)
- [Display/album-art source preferences](https://wiki.hydrogenaudio.org/index.php?title=Foobar2000:Preferences:Display)
- [Xiph Vorbis comment specification](https://xiph.org/vorbis/doc/v-comment.html)
- [Xiph VorbisComment field conventions and pictures](https://wiki.xiph.org/VorbisComment)
- [TagLib format/property mapping](https://taglib.org/api/p_propertymapping.html)

## ReplayGain and loudness

- [foobar2000 ReplayGain scanner preferences](https://wiki.hydrogenaudio.org/index.php?title=Foobar2000:Preferences:ReplayGain_Scanner)
- [foobar2000 playback ReplayGain modes](https://wiki.hydrogenaudio.org/index.php?title=Foobar2000:Preferences:Playback)
- [Hydrogenaudio ReplayGain overview and format storage notes](https://wiki.hydrogenaudio.org/index.php?title=ReplayGain)
- [Revised ReplayGain specification](https://wiki.hydrogenaudio.org/index.php?title=ReplayGain_2.0_specification)
- [EBU R 128-2023](https://tech.ebu.ch/docs/r/r128.pdf)
- [IETF RFC 7845: Ogg Opus, output gain and R128 tags](https://datatracker.ietf.org/doc/rfc7845/)
- [FLAC tool ReplayGain application behavior](https://www.xiph.org/flac/documentation_tools_flac.html)

Important caveat: “EBU R128 analysis” in ReplayGain tools often means BS.1770-
family measurement at a `-18 LUFS` ReplayGain target, not the broadcast
recommendation's `-23 LUFS` target. Persist the algorithm and reference used.

## Playback, library, playlists, and conversion

- [Converter workflow and pipeline options](https://wiki.hydrogenaudio.org/index.php?title=Foobar2000:Converter)
- [Playback and ReplayGain preferences](https://wiki.hydrogenaudio.org/index.php?title=Foobar2000:Preferences:Playback)
- [Output buffering](https://wiki.hydrogenaudio.org/index.php?title=Foobar2000:Preferences:Output)
- [DSP manager and built-in DSP examples](https://wiki.hydrogenaudio.org/index.php?title=Foobar2000:Preferences:Playback:DSP_Manager)
- [Media Library preferences](https://wiki.hydrogenaudio.org/index.php?title=Foobar2000:Preferences:Media_Library)
- [Autoplaylists](https://wiki.hydrogenaudio.org/index.php?title=Foobar2000:Autoplaylist)
- [Faceted library browsing](https://wiki.hydrogenaudio.org/index.php?title=Foobar2000:Components/Facets_%28foo_facets%29)
- [PipeWire registry API](https://docs.pipewire.org/group__pw__registry.html)
- [PipeWire metadata API](https://docs.pipewire.org/group__pw__metadata.html)
- [PipeWire stream autoconnect](https://docs.pipewire.org/page_streams.html)
- [WirePlumber session management](https://pipewire.pages.freedesktop.org/wireplumber/design/understanding_session_management.html)
  - Accessed 2026-08-29 for ADR-0030. Registry global/remove events define the
    live topology boundary; the default sink is metadata, and the session
    manager owns dynamic stream relinking when graph/default conditions change.
- [FFmpeg `AVChapter` structure](https://ffmpeg.org/doxygen/trunk/structAVChapter.html)
- [FFmpeg rational rescaling API](https://ffmpeg.org/doxygen/trunk/group__lavu__math.html)
  - Accessed 2026-08-29 for ADR-0031. FFmpeg defines chapter start/end values in
    each chapter's explicit time base; `av_rescale_q_rnd` provides the bounded
    integer rational conversion used to project absolute sample boundaries.
- [Official libopenmpt C API](https://lib.openmpt.org/doc/group__libopenmpt__c.html)
- [Official OpenMPT/libopenmpt repository](https://github.com/OpenMPT/openmpt)
  - Accessed 2026-08-29 for ADR-0032. The stable C API exposes bounded subsong
    count, selection, names, and duration, including hidden songs. The project
    is BSD-3-Clause licensed, which is compatible with Trackknife's GPLv3
    distribution; its notices remain required.

## FPL and rich cached playlist items

- [Official foobar2000 FAQ: FPL design rationale](https://www.foobar2000.org/FAQ)
- [fplreader reverse-engineered classic FPL description](https://github.com/yellowcrescent/fplreader)

The historical parser describes records containing source path, subsong,
duration, size, arbitrary metadata pairs, and four ReplayGain floats. This
supports the architectural idea that playlists can retain rich cached item
snapshots. It does not establish the current binary format or make playlists the
canonical store for embedded metadata.

## Selected and candidate implementation libraries

ADRs record which libraries are selected; a link here alone is not adoption.

- [FFmpeg documentation](https://ffmpeg.org/documentation.html)
- [FFmpeg codec multithreading notes](https://github.com/FFmpeg/FFmpeg/blob/master/doc/multithreading.txt)
- [libopenmpt documentation](https://lib.openmpt.org/libopenmpt/documentation/)
- [TagLib API and supported formats](https://taglib.org/api/)
- [GTK 4 Rust bindings listed by GTK](https://www.gtk.org/docs/language-bindings/rust/)
- [gtk4-rs documentation](https://gtk-rs.org/gtk4-rs/stable/latest/docs/gtk4/)
- [Qt main windows, dock panels, tabs, and layout persistence](https://doc.qt.io/qt-6/qmainwindow.html)
- [Qt model/view programming and incremental fetching](https://doc.qt.io/qt-6/model-view-programming.html)
- [libebur128 repository](https://github.com/jiixyj/libebur128)
- [SQLite documentation](https://sqlite.org/docs.html)
- [PipeWire documentation](https://docs.pipewire.org/)
- [libmpdclient documentation](https://www.musicpd.org/doc/libmpdclient/)

Before adoption, pin versions/licenses, audit maintenance, prototype the hardest
requirements, and write fixtures. A library's list of supported formats is not
a guarantee of lossless unknown-metadata preservation.

## Research still required

- Current FPLite/FPL import feasibility if migration is prioritized.
- Per-container tag/artwork/ReplayGain round-trip matrix using real fixtures.
- Opus normalization interoperability policy across RFC-compliant and common
  ReplayGain players.
- Gapless behavior and sample-boundary fixtures across chosen decode backends.
- GUI toolkit performance with very large virtualized tables and bulk editors.
- Linux output backend behavior for exclusive/direct paths, device changes, and
  gapless transitions.
