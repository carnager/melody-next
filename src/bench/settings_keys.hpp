// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>

namespace trackknife::bench {

// The QSettings keys the Settings dialog writes and the rest of the
// workspace reads. Kept apart from the dialog so that what reads them --
// the engine launcher, the remote engines, either window -- needs no widgets.
struct SettingsKeys {
    // What to give the remote engine: its own password when one is set,
    // otherwise this computer's -- one password everywhere, as melodyd
    // falls back to its own for the engines it plays for.
    [[nodiscard]] static QString remoteEnginePassword();

    static constexpr auto acoustid_client_key = "musicbrainz/acoustid-client-key";
    // ADR-0237 stage 2: engines also write track ratings into the files.
    static constexpr auto ratings_in_tags_key = "library/ratings-in-tags";
    // The scale other players' plain RATING tags are read on: off, 5, 10, 100.
    static constexpr auto rating_tag_scale_key = "library/rating-tag-scale";
    // ADR-0220: empty means the library is opened in this process, which is
    // what it has always done. A socket path routes it through an engine
    // instead, so pointing at one is a deliberate act and the default is
    // unchanged behaviour.
    static constexpr auto library_engine_socket_key = "library/engine-socket";
    // ADR-0227: this computer's engine, when it is one already running rather
    // than the one the workspace starts. Not shown in Settings: it is for
    // tests and for developing the engine, which run their own.
    static constexpr auto library_local_engine_socket_key = "library/local-engine-socket";
    // ADR-0223: the remote engine's password, when it differs from
    // engine_password_key; read it through remoteEnginePassword(). The key
    // keeps its old name so a saved value survives.
    static constexpr auto library_engine_token_key = "library/engine-token";
    // ADR-0234: the id the configured remote gave, once reached -- which its
    // lists are kept under. Forgotten when the address changes.
    static constexpr auto library_engine_id_key = "library/engine-id";
    // ADR-0227: where this computer sees the remote engine's music. Both
    // empty: the same paths on both machines.
    static constexpr auto library_remote_folder_key = "library/remote-folder";
    static constexpr auto library_remote_mount_key = "library/remote-mount";
    // ADR-0226/0228: how this computer's engine is shared on the network.
    // Whether this computer's library has a tab: someone whose music is all
    // on the remote engine may not want it.
    static constexpr auto library_show_local_key = "library/show-local";
    static constexpr auto engine_share_key = "engine/share";
    static constexpr auto engine_listen_key = "engine/listen";
    static constexpr auto engine_listen_default = "0.0.0.0:6600";
    static constexpr auto engine_stream_port_key = "engine/stream-port";
    static constexpr int engine_stream_port_default = 6601;
    // The password of the engines on this network: this computer's, and the
    // remote's unless library_engine_token_key says otherwise.
    static constexpr auto engine_password_key = "engine/password";
    static constexpr auto engine_music_root_key = "engine/music-root";
    // ADR-0228: the remote engine may play on this computer's speakers
    // through this computer's engine -- no melody-agent needed here.
    static constexpr auto engine_play_for_remote_key = "engine/play-for-remote";
    // ADR-0239: what engines elsewhere stream to those speakers, in kbps of
    // Opus, 0 the original files: when nearby, and when reached through a
    // VPN or a router.
    static constexpr auto engine_stream_nearby_key = "engine/stream-nearby-kbps";
    static constexpr int engine_stream_nearby_default = 0;
    static constexpr auto engine_stream_away_key = "engine/stream-away-kbps";
    static constexpr int engine_stream_away_default = 128;
    static constexpr auto replaygain_sidecar_only_key = "replaygain/sidecar-only";
    static constexpr auto replaygain_true_peak_key = "replaygain/true-peak";
    static constexpr auto artwork_embed_key = "artwork/embed";
    static constexpr auto artwork_folder_image_key = "artwork/write-folder-image";
    static constexpr auto artwork_folder_image_name_key = "artwork/folder-image-name";
    static constexpr auto artwork_fetch_source_key = "artwork/fetch-source";
    // Longest edge of a newly written cover, in pixels; 0 is no limit.
    static constexpr auto artwork_max_embedded_edge_key = "artwork/max-embedded-edge";
    static constexpr auto artwork_max_folder_edge_key = "artwork/max-folder-edge";
};

} // namespace trackknife::bench
