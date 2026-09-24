package com.melody.next.speaker

import androidx.media3.common.util.UnstableApi
import com.melody.next.engine.Endpoint
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
import org.json.JSONObject
import java.io.BufferedReader
import java.io.BufferedWriter
import java.io.IOException
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.InetSocketAddress
import java.net.Socket
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
) {
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
    @Volatile private var writer: BufferedWriter? = null
    private val writing = Mutex()
    @Volatile private var changed = true

    fun start(endpoint: Endpoint) {
        session?.cancel()
        session = scope.launch(Dispatchers.IO) { run(endpoint) }
    }

    fun stop() {
        session?.cancel()
        session = null
        _status.value = Status.Off
        scope.launch(playerThread) { audition.stop() }
    }

    /** Something changed in playback: reported at the next chance. */
    fun noteChange() {
        changed = true
    }

    private suspend fun run(endpoint: Endpoint) {
        var problem = ""
        while (scope.isActive) {
            _status.value = Status.Connecting(endpoint, problem)
            val socket = Socket()
            try {
                socket.tcpNoDelay = true
                socket.keepAlive = true
                socket.connect(InetSocketAddress(endpoint.host, endpoint.port), 5_000)
                val reader = BufferedReader(InputStreamReader(socket.getInputStream(), Charsets.UTF_8))
                val out = BufferedWriter(OutputStreamWriter(socket.getOutputStream(), Charsets.UTF_8))
                writer = out
                if (endpoint.password.isNotEmpty()) {
                    ask(reader, 1, "session.authenticate", JSONObject().put("password", endpoint.password))
                }
                ask(
                    reader, 2, "agent.register",
                    JSONObject().put("name", name).put("instance", instance).put("files", false).put("protocol", 1),
                )
                _status.value = Status.Registered(endpoint)
                problem = ""
                changed = true
                val reporting = scope.launch(playerThread) { report() }
                try {
                    serve(reader)
                } finally {
                    reporting.cancel()
                }
                problem = "the engine went away"
                // What played belonged to that connection.
                withContext(playerThread) { if (audition.loaded) audition.pause() }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                problem = failure.message ?: failure.javaClass.simpleName
            } finally {
                writer = null
                runCatching { socket.close() }
            }
            delay(retryMs)
        }
    }

    /** A request of our own, before the connection turns round. */
    private suspend fun ask(reader: BufferedReader, id: Int, method: String, params: JSONObject) {
        send(JSONObject().put("id", id).put("method", method).put("params", params))
        while (true) {
            val line = withContext(Dispatchers.IO) { reader.readLine() } ?: throw IOException("the engine closed the connection")
            val message = JSONObject(line)
            if (message.optInt("id", -1) != id) continue
            message.optJSONObject("error")?.let { throw IOException(it.optString("message", "refused")) }
            return
        }
    }

    /** From here the engine asks; every request is answered, in order. */
    private suspend fun serve(reader: BufferedReader) {
        while (true) {
            val line = withContext(Dispatchers.IO) { reader.readLine() } ?: return
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

    private fun handle(method: String, params: JSONObject): JSONObject {
        when (method) {
            "audition.load" -> audition.load(
                params.optJSONObject("source") ?: throw AgentError("invalid_argument", "a source is required"),
                params.optBoolean("play", true),
                params.optLong("position_ms", 0),
            )
            "audition.queue_next" -> audition.queueNext(
                params.optJSONObject("source") ?: throw AgentError("invalid_argument", "a source is required"),
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
            val snapshot = audition.snapshot()
            val playing = audition.playing
            val now = System.currentTimeMillis()
            val text = snapshot.toString()
            if (changed || (playing && now - lastSent >= 250) || (!playing && text != last)) {
                changed = false
                last = text
                lastSent = now
                runCatching { send(JSONObject().put("event", "audition.changed").put("data", snapshot)) }
            }
            delay(100)
        }
    }

    private suspend fun send(message: JSONObject) {
        val out = writer ?: throw IOException("not connected")
        withContext(Dispatchers.IO) {
            writing.withLock {
                out.write(message.toString())
                out.write("\n")
                out.flush()
            }
        }
    }
}
