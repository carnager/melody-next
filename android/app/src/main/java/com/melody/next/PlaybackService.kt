package com.melody.next

import android.app.PendingIntent
import android.content.Intent
import android.os.Looper
import android.os.SystemClock
import androidx.media3.common.MediaItem
import androidx.media3.common.MediaMetadata
import androidx.media3.common.Player
import androidx.media3.common.SimpleBasePlayer
import androidx.media3.common.util.UnstableApi
import androidx.media3.session.MediaSession
import androidx.media3.session.MediaSessionService
import com.google.common.util.concurrent.Futures
import com.google.common.util.concurrent.ListenableFuture
import com.melody.next.engine.EngineClient
import com.melody.next.engine.PlaybackState
import com.melody.next.engine.QueueEntry
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.launch

/**
 * The engine, as Android's media controls see a player: the notification,
 * the lock screen, a headset's buttons. It plays nothing on this phone; what
 * it is asked, it asks the engine.
 */
@UnstableApi
private class EnginePlayer(private val client: EngineClient, private val covers: Covers) :
    SimpleBasePlayer(Looper.getMainLooper()) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var state = PlaybackState()
    private var current: QueueEntry? = null
    private var hasBefore = false
    private var hasAfter = false
    private var artwork: Pair<String, ByteArray>? = null
    private var fetching: Job? = null

    init {
        scope.launch {
            combine(client.state, client.queue, client.upNext) { state, queue, upNext -> Triple(state, queue, upNext) }
                .collect { (next, queue, upNext) ->
                    state = next
                    current = queue.firstOrNull { it.entry == next.entry } ?: upNext.firstOrNull { it.entry == next.entry }
                    val row = queue.indexOfFirst { it.entry == next.entry }
                    // The engine decides what is next -- Up Next, random,
                    // repeat -- so these only say whether skipping means
                    // anything, which is what enables the buttons.
                    hasBefore = row > 0 || next.modes.repeat
                    hasAfter = upNext.isNotEmpty() || (row >= 0 && row < queue.lastIndex) || next.modes.repeat ||
                        (next.modes.random && queue.size > 1)
                    fetchArtwork()
                    invalidateState()
                }
        }
    }

    private fun fetchArtwork() {
        val entry = current ?: return
        if (artwork?.first == entry.albumGroup) return
        fetching?.cancel()
        fetching = scope.launch {
            val bytes = covers.bytes(CoverKey(path = entry.path, group = entry.albumGroup), 512) ?: return@launch
            artwork = entry.albumGroup to bytes
            invalidateState()
        }
    }

    override fun getState(): State {
        val entry = current
        val commands = Player.Commands.Builder().addAll(
            COMMAND_PLAY_PAUSE, COMMAND_STOP, COMMAND_GET_CURRENT_MEDIA_ITEM, COMMAND_GET_METADATA,
            COMMAND_GET_TIMELINE, COMMAND_SEEK_IN_CURRENT_MEDIA_ITEM,
        )
        if (hasAfter) commands.addAll(COMMAND_SEEK_TO_NEXT, COMMAND_SEEK_TO_NEXT_MEDIA_ITEM)
        if (entry != null) commands.addAll(COMMAND_SEEK_TO_PREVIOUS, COMMAND_SEEK_TO_PREVIOUS_MEDIA_ITEM)
        val builder = State.Builder()
            .setAvailableCommands(commands.build())
            .setPlayWhenReady(state.playing, PLAY_WHEN_READY_CHANGE_REASON_REMOTE)
        if (entry == null) {
            return builder.setPlaybackState(STATE_IDLE).setPlaylist(emptyList()).build()
        }
        val metadata = MediaMetadata.Builder()
            .setTitle(entry.title)
            .setArtist(entry.artist)
            .setAlbumTitle(entry.album)
            .setAlbumArtist(entry.albumArtist)
        artwork?.takeIf { it.first == entry.albumGroup }?.let {
            metadata.setArtworkData(it.second, MediaMetadata.PICTURE_TYPE_FRONT_COVER)
        }
        val duration = state.durationMs.takeIf { it > 0 } ?: entry.durationMs
        val item = MediaItemData.Builder(entry.entry)
            .setMediaItem(MediaItem.Builder().setMediaId(entry.entry).setMediaMetadata(metadata.build()).build())
            .setDurationUs(if (duration > 0) duration * 1_000 else androidx.media3.common.C.TIME_UNSET)
            .setIsSeekable(duration > 0)
            .build()
        // Neighbours stand in for "there is something before, after": the
        // controls skip by index, and the engine is asked instead.
        val playlist = buildList {
            if (hasBefore) add(MediaItemData.Builder("before").build())
            add(item)
            if (hasAfter) add(MediaItemData.Builder("after").build())
        }
        val shown = state
        return builder
            .setPlaybackState(STATE_READY)
            .setPlaylist(playlist)
            .setCurrentMediaItemIndex(if (hasBefore) 1 else 0)
            .setContentPositionMs { shown.positionAt(SystemClock.elapsedRealtime()) }
            .build()
    }

    override fun handleSetPlayWhenReady(playWhenReady: Boolean): ListenableFuture<*> {
        if (playWhenReady != state.playing) client.togglePlay()
        return Futures.immediateVoidFuture()
    }

    override fun handleSeek(mediaItemIndex: Int, positionMs: Long, seekCommand: Int): ListenableFuture<*> {
        when (seekCommand) {
            COMMAND_SEEK_TO_NEXT, COMMAND_SEEK_TO_NEXT_MEDIA_ITEM -> client.next()
            COMMAND_SEEK_TO_PREVIOUS, COMMAND_SEEK_TO_PREVIOUS_MEDIA_ITEM -> client.previous()
            else -> client.seek(positionMs.coerceAtLeast(0))
        }
        return Futures.immediateVoidFuture()
    }

    override fun handleStop(): ListenableFuture<*> {
        client.stop()
        return Futures.immediateVoidFuture()
    }

    override fun handleRelease(): ListenableFuture<*> {
        scope.cancel()
        return Futures.immediateVoidFuture()
    }
}

@UnstableApi
class PlaybackService : MediaSessionService() {
    private var session: MediaSession? = null

    override fun onCreate() {
        super.onCreate()
        val app = application as MelodyApp
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        session = MediaSession.Builder(this, EnginePlayer(app.client, app.covers))
            .setSessionActivity(open)
            .build()
            // Nothing binds to this service -- no controller asks for the
            // session -- so it is handed over here; without it Media3 never
            // takes charge of the notification.
            .also(::addSession)
    }

    override fun onGetSession(controllerInfo: MediaSession.ControllerInfo): MediaSession? = session

    override fun onTaskRemoved(rootIntent: Intent?) {
        // Swiped away while the engine is not playing: nothing to control.
        val player = session?.player
        if (player == null || !player.playWhenReady) stopSelf()
    }

    override fun onDestroy() {
        session?.run {
            player.release()
            release()
        }
        session = null
        super.onDestroy()
    }
}
