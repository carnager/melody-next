package com.melody.next.speaker

import android.content.Context
import android.os.Handler
import android.os.Looper
import androidx.annotation.MainThread
import androidx.media3.common.AudioAttributes
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.PlaybackException
import androidx.media3.common.Player
import androidx.media3.common.util.UnstableApi
import androidx.media3.exoplayer.ExoPlayer
import org.json.JSONArray
import org.json.JSONObject
import kotlin.math.min
import kotlin.math.pow

/**
 * What the agent needs of a player: the engine's audition calls, and the
 * snapshot it reports. Apart from ExoPlayer so the protocol can be held to
 * account without a device.
 */
interface Audition {
    fun load(source: JSONObject, play: Boolean, positionMs: Long)
    fun queueNext(source: JSONObject, token: Long)
    fun clearNext()
    fun play()
    fun pause()
    fun stop()
    fun seek(seconds: Double)
    fun setVolume(percent: Int)
    fun setReplayGain(params: JSONObject)
    fun setBuffer(capacityMs: Long, startMs: Long)
    /** Playing now: reported four times a second rather than only on change. */
    val playing: Boolean
    /** Anything loaded, playing or paused. */
    val loaded: Boolean
    fun snapshot(): JSONObject
}

/**
 * The engine's audition service, on a phone: what an output agent is from
 * the engine's side (ADR-0228). It plays what it is sent and says how that
 * goes; the queue, the order and what comes next stay the engine's.
 *
 * The numbers it reports are the ones the engine reads: the state as the
 * engine's own audition has it, the position in samples, a count of gapless
 * handovers, and which playback this is.
 */
@UnstableApi
class PhoneAudition(context: Context, private val onChange: () -> Unit) : Audition {
    /** The engine's states, by their number on the wire. */
    enum class State { Empty, Loading, Ready, Buffering, Playing, Paused, Draining, Ended, Failed }

    /** How a source's gain is read: off, the track's, or the album's. */
    enum class Gain { Off, Track, Album }

    private data class Loudness(val trackDb: Double?, val trackPeak: Double?, val albumDb: Double?, val albumPeak: Double?)

    private val main = Handler(Looper.getMainLooper())
    val player: ExoPlayer = ExoPlayer.Builder(context)
        .setAudioAttributes(
            AudioAttributes.Builder().setUsage(C.USAGE_MEDIA).setContentType(C.AUDIO_CONTENT_TYPE_MUSIC).build(),
            /* handleAudioFocus = */ true,
        )
        .setHandleAudioBecomingNoisy(true)
        .setWakeMode(C.WAKE_MODE_NETWORK)
        .build()

    private var failure: String? = null
    private var chainTransitions = 0L
    private var playbackInstance = 0L
    private var occurrenceToken = 0L
    private var nextToken = 0L
    private var volumePercent = 100
    private var gainMode = Gain.Off
    private var preampWithGain = 0.0
    private var preampWithoutGain = 0.0
    private var bufferCapacityMs = 750L
    private var bufferStartMs = 100L
    private val loudness = HashMap<String, Loudness>()
    private var underruns = 0L

    init {
        player.addListener(object : Player.Listener {
            override fun onMediaItemTransition(item: MediaItem?, reason: Int) {
                // The armed continuation took over by itself: a gapless
                // handover, which the engine follows by this count.
                if (reason == Player.MEDIA_ITEM_TRANSITION_REASON_AUTO) {
                    chainTransitions++
                    playbackInstance++
                    occurrenceToken = nextToken
                    nextToken = 0
                    // What finished is gone; only what plays, and what may
                    // be armed after it, stay.
                    while (player.currentMediaItemIndex > 0) player.removeMediaItem(0)
                    applyVolume()
                }
                onChange()
            }

            override fun onPlaybackStateChanged(state: Int) {
                if (state == Player.STATE_BUFFERING && player.playWhenReady && player.currentPosition > 0) underruns++
                onChange()
            }

            override fun onIsPlayingChanged(isPlaying: Boolean) = onChange()
            override fun onPlayWhenReadyChanged(playWhenReady: Boolean, reason: Int) = onChange()
            override fun onPositionDiscontinuity(old: Player.PositionInfo, new: Player.PositionInfo, reason: Int) = onChange()

            override fun onPlayerError(error: PlaybackException) {
                failure = error.cause?.message?.let { "${error.errorCodeName}: $it" } ?: error.errorCodeName
                onChange()
            }
        })
    }

    // --- What the engine asks ------------------------------------------------

    @MainThread
    override fun load(source: JSONObject, play: Boolean, positionMs: Long) {
        val item = item(source)
        failure = null
        playbackInstance++
        occurrenceToken = 0
        nextToken = 0
        player.setMediaItem(item, positionMs.coerceAtLeast(0))
        player.playWhenReady = play
        player.prepare()
        applyVolume()
    }

    @MainThread
    override fun queueNext(source: JSONObject, token: Long) {
        val item = item(source)
        clearNext()
        if (player.mediaItemCount == 0) throw AgentError("unsupported", "nothing plays to continue from")
        player.addMediaItem(item)
        nextToken = token
    }

    @MainThread
    override fun clearNext() {
        while (player.mediaItemCount > player.currentMediaItemIndex + 1) {
            player.removeMediaItem(player.mediaItemCount - 1)
        }
        nextToken = 0
    }

    @MainThread
    override fun play() {
        if (player.playbackState == Player.STATE_IDLE && player.mediaItemCount > 0) player.prepare()
        player.play()
    }

    @MainThread
    override fun pause() = player.pause()

    @MainThread
    override fun stop() {
        player.stop()
        player.clearMediaItems()
        failure = null
        occurrenceToken = 0
        nextToken = 0
    }

    @MainThread
    override fun seek(seconds: Double) = player.seekTo((seconds * 1000).toLong().coerceAtLeast(0))

    @MainThread
    override fun setVolume(percent: Int) {
        volumePercent = percent.coerceIn(0, 100)
        applyVolume()
    }

    @MainThread
    override fun setReplayGain(params: JSONObject) {
        if (params.has("mode")) {
            gainMode = Gain.entries.getOrNull(params.getInt("mode"))
                ?: throw AgentError("invalid_argument", "mode is off, track or album")
        }
        if (params.has("preamp_with_gain_db")) preampWithGain = params.getDouble("preamp_with_gain_db")
        if (params.has("preamp_without_gain_db")) preampWithoutGain = params.getDouble("preamp_without_gain_db")
        applyVolume()
    }

    @MainThread
    override fun setBuffer(capacityMs: Long, startMs: Long) {
        // Kept to report back; ExoPlayer sizes its own buffer.
        bufferCapacityMs = capacityMs
        bufferStartMs = startMs
    }

    override val playing: Boolean get() = state() == State.Playing
    override val loaded: Boolean get() = state() != State.Empty

    fun release() = main.post { player.release() }

    // --- What it reports -------------------------------------------------------

    @MainThread
    fun state(): State = when {
        failure != null -> State.Failed
        player.mediaItemCount == 0 -> State.Empty
        else -> when (player.playbackState) {
            Player.STATE_IDLE -> State.Empty
            Player.STATE_BUFFERING -> if (player.currentPosition > 0) State.Buffering else State.Loading
            Player.STATE_ENDED -> State.Ended
            else -> if (player.playWhenReady) State.Playing else State.Paused
        }
    }

    /** The snapshot, as the engine's audition would render it (`audition_wire`). */
    @MainThread
    override fun snapshot(): JSONObject {
        val state = state()
        // Positions travel in samples: at the stream's rate when it is known,
        // else in milliseconds as samples at 1 kHz, which the engine reads the same.
        val rate = player.audioFormat?.sampleRate?.takeIf { it > 0 } ?: 1000
        val channels = player.audioFormat?.channelCount?.takeIf { it > 0 } ?: 2
        val positionMs = player.currentPosition.coerceAtLeast(0)
        val durationMs = player.duration.takeIf { it != C.TIME_UNSET && it > 0 }
        val json = JSONObject()
            .put("state", state.ordinal)
            .put("position_sample", positionMs * rate / 1000)
            .put("end_sample", durationMs?.let { it * rate / 1000 } ?: JSONObject.NULL)
            .put("next_armed", player.mediaItemCount > player.currentMediaItemIndex + 1)
            .put("chain_transitions", chainTransitions)
            .put("playback_instance", playbackInstance)
            .put("occurrence_token", occurrenceToken)
            .put("next_occurrence_token", nextToken)
            .put("volume_percent", volumePercent)
            .put("replay_gain_mode", gainMode.ordinal)
            .put("preamp_with_gain_db", preampWithGain)
            .put("preamp_without_gain_db", preampWithoutGain)
            .put("configured_buffer", JSONObject().put("capacity_ms", bufferCapacityMs).put("start_threshold_ms", bufferStartMs))
            .put("underruns", underruns)
            .put("output_target", JSONObject.NULL)
            .put("default_output", JSONObject.NULL)
            .put("output_available", true)
            .put("output_suspended", false)
            .put("devices", JSONArray())
        if (state != State.Empty) {
            json.put("format", JSONObject().put("sample_rate", rate).put("channels", channels).put("layout", if (channels == 1) "mono" else "stereo"))
        }
        failure?.let { json.put("error", it) }
        return json
    }

    // --- Inside -----------------------------------------------------------------

    private fun item(source: JSONObject): MediaItem {
        val url = source.optString("url", "")
        if (url.isEmpty()) {
            // This phone has no copy of the music; the engine streams it.
            throw AgentError("unsupported", "this phone plays streams only")
        }
        val selection = source.optJSONObject("selection")
        if (selection != null && (!selection.isNull("stream_index") || !selection.isNull("subsong_index"))) {
            throw AgentError("unsupported", "this phone cannot choose a stream or subsong in a file")
        }
        if (source.optJSONObject("segment") != null) {
            throw AgentError("unsupported", "this phone cannot play part of a file (a CUE track) yet")
        }
        val gain = source.optJSONObject("replay_gain")
        loudness[url] = Loudness(
            gain?.optDoubleOrNull("track_gain_db"), gain?.optDoubleOrNull("track_peak"),
            gain?.optDoubleOrNull("album_gain_db"), gain?.optDoubleOrNull("album_peak"),
        )
        return MediaItem.Builder().setUri(url).setMediaId(url).build()
    }

    /**
     * Volume and ReplayGain as one level: the gain the mode picks (falling
     * back to the other kind), its pre-amp, held under the peak so it does
     * not clip -- and at most full scale, which is all a phone's player
     * can do.
     */
    private fun applyVolume() {
        val id = player.currentMediaItem?.mediaId
        val level = loudness[id]
        val (db, peak) = when (gainMode) {
            Gain.Off -> null to null
            Gain.Track -> (level?.trackDb ?: level?.albumDb) to (level?.trackPeak ?: level?.albumPeak)
            Gain.Album -> (level?.albumDb ?: level?.trackDb) to (level?.albumPeak ?: level?.trackPeak)
        }
        var linear = when {
            gainMode == Gain.Off -> 1.0
            db != null -> 10.0.pow((db + preampWithGain) / 20.0)
            else -> 10.0.pow(preampWithoutGain / 20.0)
        }
        if (peak != null && peak > 0) linear = min(linear, 1.0 / peak)
        player.volume = (min(linear, 1.0) * volumePercent / 100.0).toFloat()
    }

    private fun JSONObject.optDoubleOrNull(name: String): Double? = if (isNull(name) || !has(name)) null else optDouble(name)
}

/** A refusal with a code the engine understands (`core::ErrorCode`'s names). */
class AgentError(val code: String, message: String) : Exception(message)
