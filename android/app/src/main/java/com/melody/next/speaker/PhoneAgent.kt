package com.melody.next.speaker

import androidx.media3.common.util.UnstableApi
import com.melody.next.engine.Endpoint
import com.melody.next.engine.Lines
import com.melody.next.engine.Nearby
import com.melody.next.engine.sameAddress
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.IOException
import java.util.UUID

/**
 * This phone as an output agent (ADR-0228): it connects to the engine, says
 * who it is, and from then on the connection runs the other way -- the
 * engine asks, this answers, and reports what it is doing so the engine
 * can follow the track to its end.
 */
@UnstableApi
class PhoneAgent(
    private val scope: CoroutineScope,
    private val audition: Audition,
    private val name: String,
    // Where the player lives: ExoPlayer wants the thread it was made on.
    private val playerThread: CoroutineDispatcher = Dispatchers.Main,
    private val retryMs: Long = 2_000,
    /**
     * True while the connection is being made again after it dropped with
     * music playing here: the engine resumes on this phone once it is back,
     * and until then the app must stay in front, or the system cuts it off
     * the network and it never is back.
     */
    val reconnecting: MutableStateFlow<Boolean> = MutableStateFlow(false),
    private val giveUpMs: Long = 10 * 60_000,
    /** ADR-0272: engines on this network, to reach the engine the nearer way. */
    private val nearby: Nearby = Nearby(),
) {
    /**
     * What this phone wants streamed: the original, or Opus at a bit rate
     * (0 for the original). Told at registration and in every report, so
     * a change -- off Wi-Fi -- applies to the next track the engine sends.
     */
    @Volatile var bitrateKbps: Int = 0
        set(value) {
            field = value
            changed = true
        }

    private fun wish(): JSONObject =
        if (bitrateKbps > 0) JSONObject().put("format", "opus").put("bitrate", bitrateKbps)
        else JSONObject().put("format", "original")
    sealed interface Status {
        data object Off : Status
        data class Connecting(val endpoint: Endpoint, val problem: String = "") : Status
        data class Registered(val endpoint: Endpoint) : Status
    }

    private val _status = MutableStateFlow<Status>(Status.Off)
    val status: StateFlow<Status> = _status

    /** Per process: the engine tells a restarted agent from a second one by it. */
    private val instance = UUID.randomUUID().toString()
    private var session: Job? = null
    @Volatile private var writer: Lines? = null
    private val writing = Mutex()
    @Volatile private var changed = true

    // The engine as this phone reached it, for where its streams are.
    @Volatile private var reached: Endpoint? = null
    @Volatile private var saved: Endpoint? = null

    /** Plays for the engine at `endpoint` -- or nearer, found here by the id `id` gives. */
    fun start(endpoint: Endpoint, id: () -> String? = { null }) {
        session?.cancel()
        saved = endpoint
        session = scope.launch(Dispatchers.IO) { run(endpoint, id) }
    }

    /**
     * The phone changed networks: a connection made the nearer way is made
     * again -- left home, the engine is not there any more (ADR-0272).
     */
    fun networkChanged() {
        val wanted = saved ?: return
        val via = reached ?: return
        if (!via.sameAddress(wanted)) writer?.let { runCatching { it.close() } }
    }

    fun stop() {
        session?.cancel()
        session = null
        stopWaiting()
        _status.value = Status.Off
        scope.launch(playerThread) { audition.stop() }
    }

    /** Something changed in playback: reported at the next chance. */
    fun noteChange() {
        changed = true
    }

    private var giveUp: Job? = null

    private fun stopWaiting() {
        giveUp?.cancel()
        giveUp = null
        reconnecting.value = false
    }

    /** Waits for the engine to come back -- for a while, not for ever. */
    private fun waitForReturn() {
        reconnecting.value = true
        giveUp?.cancel()
        giveUp = scope.launch {
            delay(giveUpMs)
            reconnecting.value = false
        }
    }

    private suspend fun run(endpoint: Endpoint, id: () -> String?) {
        var problem = ""
        while (scope.isActive) {
            var registered = false
            var switching = false
            var nearer: Job? = null
            _status.value = Status.Connecting(endpoint, problem)
            var lines: Lines? = null
            try {
                val (via, opened) = nearby.reach(endpoint, id(), 5_000) { at, timeoutMs ->
                    withContext(Dispatchers.IO) { at.openLines(timeoutMs) }
                }
                lines = opened
                reached = via
                val reader = opened
                writer = opened
                if (via.password.isNotEmpty()) {
                    ask(reader, 1, "session.authenticate", JSONObject().put("password", via.password))
                }
                ask(
                    reader, 2, "agent.register",
                    JSONObject().put("name", name).put("instance", instance).put("files", false).put("protocol", 1)
                        .put("decodes", JSONArray(DECODES)).put("stream", wish()),
                )
                _status.value = Status.Registered(endpoint)
                registered = true
                // Back: the engine takes up what played, where it was.
                stopWaiting()
                problem = ""
                changed = true
                // Reached the long way, and the engine turns up here: come
                // home -- once nothing plays here, not cutting a track off.
                if (via === endpoint) {
                    nearer = scope.launch {
                        nearby.awaitNearer(endpoint, id)
                        while (withContext(playerThread) { audition.playing }) delay(retryMs)
                        switching = true
                        runCatching { opened.close() }
                    }
                }
                val reporting = scope.launch(playerThread) { report() }
                try {
                    serve(reader)
                } finally {
                    reporting.cancel()
                }
                problem = "the engine went away"
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                problem = failure.message ?: failure.javaClass.simpleName
            } finally {
                nearer?.cancel()
                writer = null
                runCatching { lines?.close() }
            }
            if (switching) continue
            // Gone, cleanly or not -- a network switch usually breaks the
            // connection rather than closing it. What played belonged to it;
            // if something was, the engine is waited for.
            if (registered) {
                val wasPlaying = withContext(playerThread) {
                    audition.playing.also { if (audition.loaded) audition.pause() }
                }
                if (wasPlaying) waitForReturn()
            }
            delay(retryMs)
        }
    }

    /** A request of our own, before the connection turns round. */
    private suspend fun ask(reader: Lines, id: Int, method: String, params: JSONObject) {
        send(JSONObject().put("id", id).put("method", method).put("params", params))
        while (true) {
            val line = withContext(Dispatchers.IO) { reader.read() } ?: throw IOException("the engine closed the connection")
            val message = JSONObject(line)
            if (message.optInt("id", -1) != id) continue
            message.optJSONObject("error")?.let { throw IOException(it.optString("message", "refused")) }
            return
        }
    }

    /** From here the engine asks; every request is answered, in order. */
    private suspend fun serve(reader: Lines) {
        while (true) {
            val line = withContext(Dispatchers.IO) { reader.read() } ?: return
            if (line.isBlank()) continue
            val request = JSONObject(line)
            if (!request.has("method")) continue
            val id = if (request.has("id")) request.get("id") else null
            val answer = try {
                val result = withContext(playerThread) {
                    handle(request.getString("method"), request.optJSONObject("params") ?: JSONObject())
                }
                changed = true
                JSONObject().put("result", result)
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (refused: AgentError) {
                JSONObject().put("error", JSONObject().put("code", refused.code).put("message", refused.message))
            } catch (failure: Exception) {
                JSONObject().put("error", JSONObject().put("code", "backend").put("message", failure.message ?: "failed"))
            }
            // A notification (no id) is answered by nobody.
            if (id != null) send(answer.put("id", id))
        }
    }

    /**
     * ADR-0270: a stream as this phone reaches the engine -- the engine names
     * its own address, which behind a proxy is not one this phone can reach,
     * and says http where the proxy speaks TLS.
     */
    private fun reachable(source: JSONObject?): JSONObject {
        source ?: throw AgentError("invalid_argument", "a source is required")
        val url = source.optString("url", "")
        val via = reached
        if (url.isNotEmpty() && via != null) source.put("url", via.streamUrl(url))
        return source
    }

    private fun handle(method: String, params: JSONObject): JSONObject {
        when (method) {
            "audition.load" -> audition.load(
                reachable(params.optJSONObject("source")),
                params.optBoolean("play", true),
                params.optLong("position_ms", 0),
            )
            "audition.queue_next" -> audition.queueNext(
                reachable(params.optJSONObject("source")),
                params.optLong("token", 0),
            )
            "audition.clear_next" -> audition.clearNext()
            "audition.play" -> audition.play()
            "audition.pause" -> audition.pause()
            "audition.stop" -> audition.stop()
            "audition.seek" -> audition.seek(params.optDouble("seconds", 0.0))
            "audition.volume" -> audition.setVolume(params.optInt("percent", 100))
            "audition.replay_gain" -> audition.setReplayGain(params)
            "audition.buffer" -> audition.setBuffer(params.optLong("capacity_ms", 750), params.optLong("start_threshold_ms", 100))
            // One output here, the phone's own: nothing to choose between.
            "audition.refresh_outputs", "audition.output" -> Unit
            else -> throw AgentError("unsupported", "$method is not something this phone does")
        }
        return JSONObject()
    }

    /**
     * `audition.changed`: on every change, and four times a second while
     * playing, so the engine's clock and its end-of-track follow this one.
     */
    private suspend fun report() {
        var last = ""
        var lastSent = 0L
        while (true) {
            val snapshot = audition.snapshot().put("stream", wish())
            val playing = audition.playing
            val now = System.currentTimeMillis()
            val text = snapshot.toString()
            if (changed || (playing && now - lastSent >= 250) || (!playing && text != last)) {
                changed = false
                last = text
                lastSent = now
                // Not sent: the connection is gone, whatever reading says --
                // closed, so it is made again rather than waited on.
                runCatching { send(JSONObject().put("event", "audition.changed").put("data", snapshot)) }
                    .onFailure { writer?.let { gone -> scope.launch(Dispatchers.IO) { runCatching { gone.close() } } } }
            }
            delay(100)
        }
    }

    private suspend fun send(message: JSONObject) {
        val out = writer ?: throw IOException("not connected")
        withContext(Dispatchers.IO) {
            writing.withLock { out.write(message.toString()) }
        }
    }

    companion object {
        /**
         * What Android plays from a stream on every phone, by FFmpeg's names:
         * the engine converts anything else -- WavPack, APE, a tracker file.
         */
        val DECODES = listOf(
            "flac", "mp3", "aac", "opus", "vorbis",
            "pcm_s16le", "pcm_s24le", "pcm_u8", "pcm_f32le",
        )
    }
}
