# Third-party dependency notes

Trackknife source is GPL-3.0-only. Dependencies retain their own licenses and
must be inventoried before binary distribution.

## Current build and test dependencies

| Dependency | Use | Linking/status | License note |
| --- | --- | --- | --- |
| Qt 6 Core/Gui/Widgets/Concurrent/DBus/Network/Test | Product UI, desktop integration, network providers, concurrency adapters, and tests | Dynamic system libraries | LGPLv3 option. Trackknife does not modify Qt, uses no static Qt linkage, and permits users to replace the system Qt libraries; packages must preserve Qt copyright/license notices and provide the corresponding Qt source offer required by their distributor. |
| utf8proc 2.9+ | Core UTF-8 validation and Unicode mapping | Dynamic system library | MIT plus Unicode data notice; compatible with GPLv3. |
| libmpdclient 2.22+ | MPD command/protocol adapter (M2) | Dynamic system library | BSD-2/3-Clause source notices; compatible with GPLv3. Exact packaged notice is retained when linked. |
| SQLite 3.37+ | Transactional profiles, workspace, and list-document state | Dynamic system library | Public domain; compatible with GPLv3. |
| FFmpeg libavformat 60+, libavcodec 60+, libavutil 58+, libswresample 4+ | M4 local media probe/decode/sample conversion boundary | Dynamic system libraries | FFmpeg is LGPL-2.1-or-later by default and may include GPL components depending on the distribution build; dynamically linked packaged configurations must remain GPLv3-compatible and retain FFmpeg notices. |
| libopenmpt 0.7+ | M4 tracker-module subsong identity, names, and duration | Dynamic system library; FFmpeg remains the decoder | BSD-3-Clause; compatible with GPLv3. The OpenMPT/libopenmpt copyright and license notice must accompany distribution. |
| TagLib 2.0+ | M5 generic ordered metadata property reader | Dynamic system library; write capabilities remain disabled | LGPL-2.1-only or MPL-1.1; the LGPL licensing path is compatible with GPLv3. TagLib notices and source/relinking obligations must accompany distribution as applicable. |
| PipeWire 0.3.50+ | M4 direct local audio output and real-time stream callback | Dynamic system library | MIT; compatible with GPLv3. PipeWire and SPA notices remain with their distribution packages. |
| libebur128 1.2+ | EBU R128 integrated loudness and peak analysis | Dynamic system library | MIT; compatible with GPLv3. Copyright and license notices remain with the distribution package. |
| nlohmann/json 3.11+ | Test-only title-format corpus loader | Header-only, test target only | MIT; compatible with GPLv3. |
| Chromaprint `fpcalc` | Optional AcoustID fingerprint subprocess | Separate optional executable, never linked | LGPL-2.1-or-later; Trackknife works without it and invokes an independently installed executable. |

Binary releases use only dynamic system-library linkage. Packagers must ship
this inventory and Trackknife's GPL-3.0-only license beside the binary; system
dependency notices may remain in their owning packages when the package manager
guarantees their installation.
