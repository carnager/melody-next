package com.melody.next.offline

import android.content.Context
import android.net.Uri
import androidx.media3.common.AudioAttributes
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.MediaMetadata
import androidx.media3.common.Player
import androidx.media3.common.util.UnstableApi
import androidx.media3.exoplayer.ExoPlayer
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/** What the offline player is doing, for the screens. */
data class OfflineState(
    val album: OfflineAlbum? = null,
    val index: Int = 0,
    val playing: Boolean = false,
) {
    val track: OfflineTrack? get() = album?.tracks?.getOrNull(index)
    val active: Boolean get() = album != null
}

/**
 * Albums kept on the phone, played on it -- with no engine in reach. The
 * engine's queue and modes are the engine's; this is a player of its own,
 * one album at a time, and says so on screen.
 */
@UnstableApi
class OfflinePlayer(context: Context) {
    val player: ExoPlayer = ExoPlayer.Builder(context)
        .setAudioAttributes(
            AudioAttributes.Builder().setUsage(C.USAGE_MEDIA).setContentType(C.AUDIO_CONTENT_TYPE_MUSIC).build(),
            /* handleAudioFocus = */ true,
        )
        .setHandleAudioBecomingNoisy(true)
        .build()

    private val _state = MutableStateFlow(OfflineState())
    val state: StateFlow<OfflineState> = _state

    init {
        player.addListener(object : Player.Listener {
            override fun onEvents(player: Player, events: Player.Events) {
                _state.value = _state.value.copy(index = player.currentMediaItemIndex, playing = player.isPlaying)
            }
        })
    }

    fun play(album: OfflineAlbum, startAt: Int = 0) {
        val kept = album.tracks.filter { it.file.isFile }
        if (kept.isEmpty()) return
        val items = kept.map { track ->
            MediaItem.Builder()
                .setUri(Uri.fromFile(track.file))
                .setMediaId(track.entry.key)
                .setMediaMetadata(
                    MediaMetadata.Builder()
                        .setTitle(track.entry.title.ifEmpty { track.entry.label })
                        .setArtist(track.entry.artist)
                        .setAlbumTitle(album.album.album)
                        .setAlbumArtist(album.album.artist)
                        .setArtworkUri(album.cover.takeIf { it.isFile }?.let(Uri::fromFile))
                        .build(),
                )
                .build()
        }
        _state.value = OfflineState(album.copy(tracks = kept), startAt.coerceIn(0, kept.lastIndex), true)
        player.setMediaItems(items, startAt.coerceIn(0, kept.lastIndex), 0)
        player.prepare()
        player.play()
    }

    fun toggle() = if (player.isPlaying) player.pause() else player.play()
    fun next() = player.seekToNextMediaItem()
    fun previous() = player.seekToPrevious()
    fun seek(positionMs: Long) = player.seekTo(positionMs)

    /** Done with it: the engine's controls come back. */
    fun close() {
        player.stop()
        player.clearMediaItems()
        _state.value = OfflineState()
    }
}
