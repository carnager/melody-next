package com.melody.next.engine

import android.os.SystemClock
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import org.json.JSONArray
import org.json.JSONObject
import java.io.IOException

sealed interface ConnectionState {
    data object Idle : ConnectionState
    data class Connecting(val endpoint: Endpoint, val lastError: String = "") : ConnectionState
    data class Connected(val endpoint: Endpoint, val name: String) : ConnectionState
    /** Refused -- a wrong password -- and not retried until changed. */
    data class Refused(val endpoint: Endpoint, val reason: String) : ConnectionState
}

data class LibraryPage(val entries: List<LibraryEntry>, val more: Boolean)

/**
 * The phone's view of one engine. The engine owns the queue, Up Next and
 * playback; this mirrors them -- from the state it is sent and the events it
 * hears -- and turns what the user does into requests. Nothing is decided
 * here that the engine decides.
 */
class EngineClient(
    private val scope: CoroutineScope,
    private val clock: () -> Long = { SystemClock.elapsedRealtime() },
) {
    private val _connection = MutableStateFlow<ConnectionState>(ConnectionState.Idle)
    private val _state = MutableStateFlow(PlaybackState())
    private val _queue = MutableStateFlow<List<QueueEntry>>(emptyList())
    private val _upNext = MutableStateFlow<List<QueueEntry>>(emptyList())
    private val _outputs = MutableStateFlow<List<Output>>(emptyList())
    private val _problems = MutableSharedFlow<String>(extraBufferCapacity = 8)
    private val _ratings = MutableSharedFlow<RatingChange>(extraBufferCapacity = 16)

    val connection: StateFlow<ConnectionState> = _connection
    val state: StateFlow<PlaybackState> = _state
    val queue: StateFlow<List<QueueEntry>> = _queue
    val upNext: StateFlow<List<QueueEntry>> = _upNext
    val outputs: StateFlow<List<Output>> = _outputs
    /** Something asked for that failed, said for a person. */
    val problems: SharedFlow<String> = _problems
    /** A rating set on the engine, by any client: this one, Trackknife, a script. */
    val ratings: SharedFlow<RatingChange> = _ratings

    @Volatile private var control: EngineConnection? = null
    @Volatile private var covers: EngineConnection? = null
    private val coversOpening = Mutex()
    private var session: Job? = null
    private var endpoint: Endpoint? = null
    private var queueRevision = -1L

    /** Connects, and keeps reconnecting until told otherwise. */
    fun connect(to: Endpoint) {
        session?.cancel()
        endpoint = to
        covers?.close()
        covers = null
        session = scope.launch { run(to) }
    }

    fun disconnect() {
        session?.cancel()
        session = null
        control?.close()
        covers?.close()
        control = null
        covers = null
        _connection.value = ConnectionState.Idle
    }

    /** Asks again at once: back in the foreground, or the network changed. */
    fun reconnectNow() {
        val current = endpoint ?: return
        if (control?.isOpen != true) connect(current)
    }

    private suspend fun run(to: Endpoint) {
        var lastError = ""
        while (true) {
            _connection.value = ConnectionState.Connecting(to, lastError)
            try {
                val opened = EngineConnection.open(to, scope)
                control = opened
                val listening = scope.launch { opened.events.collect { handle(it) } }
                try {
                    val name = opened.call("engine.info").optString("name").ifEmpty { to.host }
                    queueRevision = -1
                    adoptState(opened.call("playback.state"))
                    refreshOutputs()
                    _connection.value = ConnectionState.Connected(to, name)
                    lastError = ""
                    val polling = scope.launch { keepTime(opened) }
                    val why = opened.awaitClosed()
                    polling.cancel()
                    lastError = why.message ?: "connection lost"
                } finally {
                    listening.cancel()
                    opened.close()
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                lastError = failure.message ?: failure.javaClass.simpleName
                // A wrong password will not get better by asking again.
                if (failure is EngineError && failure.code == "unauthorized") {
                    _connection.value = ConnectionState.Refused(to, lastError)
                    return
                }
            }
            control = null
            delay(RETRY_MS)
        }
    }

    /**
     * The engine does not send position -- it moves continuously (ADR-0222).
     * The clock here counts on from the last state; this corrects it now and
     * then, and notices a track ending on its own.
     */
    private suspend fun keepTime(connection: EngineConnection) {
        while (connection.isOpen) {
            delay(if (_state.value.playing) 5_000 else 15_000)
            runCatching { adoptState(connection.call("playback.state")) }
        }
    }

    private suspend fun handle(event: EngineEvent) {
        when (event.name) {
            "playback.changed" -> adoptState(event.data)
            "outputs.changed" -> _outputs.value = event.data.optJSONArray("outputs").objects().map(Output::from)
            "catalogue.rating_changed" -> event.data.optString("hash").takeIf { it.isNotEmpty() }?.let { hash ->
                _ratings.tryEmit(RatingChange(hash, event.data.optInt("rating")))
            }
        }
    }

    private suspend fun adoptState(json: JSONObject) {
        if (!json.has("status")) return
        val next = PlaybackState.from(json, clock())
        _state.value = next
        // The queue is fetched when it changed, anyone's change: the
        // revision says so, including another client's edit.
        if (next.queueRevision != queueRevision) {
            queueRevision = next.queueRevision
            refreshQueue()
        }
    }

    private suspend fun refreshQueue() {
        val connection = control ?: return
        runCatching {
            _queue.value = connection.call("playback.queue").optJSONArray("entries").objects().map(::QueueEntry)
            _upNext.value = connection.call("playback.requests").optJSONArray("entries").objects().map(::QueueEntry)
        }
    }

    private suspend fun refreshOutputs() {
        val connection = control ?: return
        runCatching {
            _outputs.value = connection.call("outputs.list").optJSONArray("outputs").objects().map(Output::from)
        }
    }

    /** The engine's address as this phone reaches it: where its stream port is too. */
    fun engineHost(): String? = (_connection.value as? ConnectionState.Connected)?.endpoint?.host

    /** A request whose answer is wanted; throws when not connected or refused. */
    suspend fun call(method: String, params: JSONObject? = null): JSONObject {
        val connection = control ?: throw IOException("not connected to an engine")
        return connection.call(method, params)
    }

    /**
     * A request made for its effect. A playback answer is the new state,
     * adopted at once rather than waiting for the event; a refusal is said.
     */
    fun command(method: String, params: JSONObject? = null, after: suspend () -> Unit = {}) {
        scope.launch {
            try {
                adoptState(call(method, params))
                after()
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                _problems.tryEmit(failure.message ?: "the engine refused")
            }
        }
    }

    // --- Transport ---------------------------------------------------------

    fun playEntry(entry: String) = command("playback.play", JSONObject().put("entry", entry))

    fun togglePlay() {
        val current = _state.value
        when {
            current.playing -> command("playback.pause")
            current.status == "paused" -> command("playback.resume")
            current.entry.isNotEmpty() && _queue.value.any { it.entry == current.entry } -> playEntry(current.entry)
            _queue.value.isNotEmpty() -> playEntry(_queue.value.first().entry)
        }
    }

    fun next() = command("playback.next")
    fun previous() = command("playback.previous")
    fun stop() = command("playback.stop")
    fun seek(positionMs: Long) = command("playback.seek", JSONObject().put("position_ms", positionMs.coerceAtLeast(0)))
    fun setVolume(percent: Int) = command("playback.set_volume", JSONObject().put("percent", percent.coerceIn(0, 100)))

    fun setModes(change: JSONObject) = command("playback.set_modes", change)
    fun toggleRepeat() = setModes(JSONObject().put("repeat", !_state.value.modes.repeat))
    fun toggleRandom() = setModes(JSONObject().put("random", !_state.value.modes.random))
    fun cycleSingle() = setModes(JSONObject().put("single", (_state.value.modes.single + 1) % 3))
    fun cycleConsume() = setModes(JSONObject().put("consume", (_state.value.modes.consume + 1) % 3))

    fun selectOutput(id: String) = command("outputs.select", JSONObject().put("id", id), after = { refreshOutputs() })

    // --- The queue and Up Next ---------------------------------------------

    private fun entries(list: List<QueueEntry>) = JSONArray().apply { list.forEach { put(it.json) } }

    /**
     * Replaces the queue with these and plays one -- the first, or the track
     * tapped, with the rest of its album around it.
     */
    fun play(entries: List<QueueEntry>, startAt: Int = 0) {
        val first = entries.getOrNull(startAt) ?: return
        scope.launch {
            try {
                call("playback.replace_queue", JSONObject().put("entries", entries(entries)))
                adoptState(call("playback.play", JSONObject().put("entry", first.entry)))
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                _problems.tryEmit(failure.message ?: "could not play")
            }
        }
    }

    /** After what is there, which keeps its identities: what plays, plays on. */
    fun append(entries: List<QueueEntry>) = replaceQueue(_queue.value + entries)

    /** Up Next: handed to the engine to hold, then asked for, first or last. */
    fun request(entries: List<QueueEntry>, first: Boolean) {
        if (entries.isEmpty()) return
        scope.launch {
            try {
                call("playback.enqueue", JSONObject().put("entries", entries(entries)))
                val waiting = _upNext.value.map { it.entry }
                val asked = entries.map { it.entry }
                val order = if (first) asked + waiting else waiting + asked
                adoptState(call("playback.set_requests", JSONObject().put("entries", JSONArray(order))))
                refreshQueue()
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                _problems.tryEmit(failure.message ?: "could not add to Up Next")
            }
        }
    }

    fun replaceQueue(entries: List<QueueEntry>) =
        command("playback.replace_queue", JSONObject().put("entries", entries(entries)), after = { refreshQueue() })

    fun removeFromQueue(entry: String) = replaceQueue(_queue.value.filterNot { it.entry == entry })

    fun moveInQueue(from: Int, to: Int) {
        val list = _queue.value.toMutableList()
        if (from !in list.indices || to !in list.indices || from == to) return
        list.add(to, list.removeAt(from))
        _queue.value = list
        replaceQueue(list)
    }

    fun clearQueue() {
        scope.launch {
            try {
                call("playback.stop")
                adoptState(call("playback.replace_queue", JSONObject().put("entries", JSONArray())))
                refreshQueue()
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                _problems.tryEmit(failure.message ?: "could not clear the queue")
            }
        }
    }

    fun removeRequest(entry: String) {
        val order = _upNext.value.map { it.entry }.filterNot { it == entry }
        command("playback.set_requests", JSONObject().put("entries", JSONArray(order)), after = { refreshQueue() })
    }

    fun clearRequests() = command("playback.clear_requests", after = { refreshQueue() })

    // --- The library -------------------------------------------------------

    suspend fun query(
        kind: EntryKind,
        text: String = "",
        artist: String? = null,
        albumKey: String? = null,
        path: String? = null,
        newestFirst: Boolean = false,
        offset: Int = 0,
        limit: Int = 500,
    ): LibraryPage {
        val params = JSONObject()
            .put("kind", kind.wire)
            .put("text", text)
            .put("offset", offset)
            .put("limit", limit)
            .put("newest_first", newestFirst)
        artist?.let { params.put("artist", it) }
        albumKey?.let { params.put("album_key", it) }
        path?.let { params.put("path", it) }
        val answer = call("catalogue.query", params)
        return LibraryPage(answer.optJSONArray("entries").objects().map(LibraryEntry::from), answer.optBoolean("more"))
    }

    suspend fun tracksOf(album: LibraryEntry): List<LibraryEntry> =
        query(EntryKind.Track, albumKey = album.key, limit = 5_000).entries

    /** What the library knows of a file: its title, rating, rating key. */
    suspend fun track(path: String): LibraryEntry? =
        runCatching { query(EntryKind.Track, path = path, limit = 1).entries.firstOrNull() }.getOrNull()

    suspend fun setRating(hash: String, album: Boolean, rating: Int) {
        call("catalogue.set_rating", JSONObject().put("hash", hash).put("album", album).put("rating", rating.coerceIn(0, 10)))
    }

    /**
     * A cover, scaled by the engine to `size` pixels. Over a connection of
     * its own: a cover is the one slow answer, and the control connection
     * serves requests in order.
     */
    suspend fun cover(albumKey: String? = null, path: String? = null, size: Int): ByteArray? {
        val params = JSONObject().put("size", size)
        when {
            albumKey != null -> params.put("album_key", albumKey)
            path != null -> params.put("path", path)
            else -> return null
        }
        val connection = coverConnection() ?: throw IOException("not connected to an engine")
        val answer = connection.call("catalogue.artwork", params, timeoutMs = 30_000)
        if (answer.isNull("image")) return null
        return RawPath.decode(answer.getString("image")).takeIf { it.isNotEmpty() }
    }

    private suspend fun coverConnection(): EngineConnection? {
        covers?.takeIf { it.isOpen }?.let { return it }
        val to = endpoint ?: return null
        if (_connection.value !is ConnectionState.Connected) return null
        return coversOpening.withLock {
            covers?.takeIf { it.isOpen } ?: EngineConnection.open(to, scope).also { covers = it }
        }
    }

    companion object {
        private const val RETRY_MS = 2_000L
    }
}
