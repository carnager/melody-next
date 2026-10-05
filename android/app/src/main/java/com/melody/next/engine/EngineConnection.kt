package com.melody.next.engine

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import org.json.JSONObject
import java.io.BufferedReader
import java.io.BufferedWriter
import java.io.IOException
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.InetSocketAddress
import java.net.Socket
import java.net.URI
import javax.net.ssl.SSLSocket
import javax.net.ssl.SSLSocketFactory
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicInteger

/**
 * Where an engine listens, and the password it wants, if any. With [tls] it
 * is reached through a proxy that speaks TLS (ADR-0270) -- the engine itself
 * speaks plain TCP, behind it.
 */
data class Endpoint(
    val host: String,
    val port: Int = DEFAULT_PORT,
    val password: String = "",
    val tls: Boolean = false,
) {
    companion object {
        const val DEFAULT_PORT = 6603
        private const val TLS_PREFIX = "tls://"

        /**
         * "host", "host:port" or "[v6]:port", as a person types it; with
         * "tls://" before it, reached over TLS.
         */
        fun parse(text: String, password: String = ""): Endpoint? {
            var trimmed = text.trim()
            val tls = trimmed.startsWith(TLS_PREFIX, ignoreCase = true)
            if (tls) trimmed = trimmed.substring(TLS_PREFIX.length)
            if (trimmed.isEmpty()) return null
            if (trimmed.startsWith("[")) {
                val close = trimmed.indexOf(']')
                if (close < 0) return null
                val host = trimmed.substring(1, close)
                val port = trimmed.substring(close + 1).removePrefix(":").toIntOrNull() ?: DEFAULT_PORT
                return Endpoint(host, port, password, tls)
            }
            val colon = trimmed.lastIndexOf(':')
            // More than one colon without brackets is a bare IPv6 address.
            if (colon < 0 || trimmed.indexOf(':') != colon) return Endpoint(trimmed, DEFAULT_PORT, password, tls)
            val port = trimmed.substring(colon + 1).toIntOrNull() ?: return null
            return Endpoint(trimmed.substring(0, colon), port, password, tls)
        }
    }

    private val authorityHost: String get() = if (host.contains(':')) "[$host]" else host

    override fun toString(): String = (if (tls) TLS_PREFIX else "") + "$authorityHost:$port"

    /**
     * A stream of this engine's, as this phone reaches it: at the host it
     * connected to -- which behind a proxy is not the address the engine sees
     * itself at -- over HTTPS when the engine is reached over TLS, at the
     * stream port the engine named.
     */
    fun streamUrl(port: Int, query: String): String =
        "${if (tls) "https" else "http"}://$authorityHost:$port/stream?$query"

    /** An address the engine gave for a stream, taken to this host likewise. */
    fun streamUrl(given: String): String {
        val uri = runCatching { URI(given) }.getOrNull() ?: return given
        if (uri.port < 0 || uri.rawPath != "/stream") return given
        return streamUrl(uri.port, uri.rawQuery ?: "")
    }

    /**
     * A socket to the engine, connected -- and with [tls] the handshake made
     * and the certificate checked against the host's name, as a browser does.
     */
    fun openSocket(timeoutMs: Int, factory: SSLSocketFactory? = null): Socket {
        // Named: inside apply, `port` would be the unconnected socket's own.
        val address = InetSocketAddress(host, port)
        val plain = Socket().apply {
            tcpNoDelay = true
            keepAlive = true
            connect(address, timeoutMs)
        }
        if (!tls) return plain
        return try {
            val secured = (factory ?: SSLSocketFactory.getDefault() as SSLSocketFactory)
                .createSocket(plain, host, port, true) as SSLSocket
            secured.sslParameters = secured.sslParameters.apply { endpointIdentificationAlgorithm = "HTTPS" }
            secured.soTimeout = timeoutMs
            secured.startHandshake()
            secured.soTimeout = 0
            secured
        } catch (failure: Throwable) {
            runCatching { plain.close() }
            throw failure
        }
    }
}

/** An error the engine answered with: a stable code, and text for a person. */
class EngineError(val code: String, message: String) : IOException(message)

/** An unsolicited message from the engine -- a state change, a job's progress. */
data class EngineEvent(val name: String, val data: JSONObject)

/**
 * One connection to an engine, speaking protocol v1 (ADR-0222): one JSON
 * object per line, requests answered by id, events arriving whenever. The
 * engine serves a connection's requests in order, so anything slow -- covers
 * -- goes over a connection of its own.
 */
class EngineConnection private constructor(
    private val socket: Socket,
    scope: CoroutineScope,
) {
    private val reader = BufferedReader(InputStreamReader(socket.getInputStream(), Charsets.UTF_8))
    private val writer = BufferedWriter(OutputStreamWriter(socket.getOutputStream(), Charsets.UTF_8))
    private val writing = Mutex()
    private val ids = AtomicInteger(0)
    private val pending = ConcurrentHashMap<Int, CompletableDeferred<JSONObject>>()
    private val closed = CompletableDeferred<Throwable>()
    private val _events = MutableSharedFlow<EngineEvent>(extraBufferCapacity = 64)

    val events: SharedFlow<EngineEvent> = _events

    /** Completes, with why, when the connection is gone. */
    suspend fun awaitClosed(): Throwable = closed.await()

    val isOpen: Boolean get() = !closed.isCompleted

    init {
        scope.launch(Dispatchers.IO) { readLoop() }
    }

    /** Asks and waits for the answer: the result, or the engine's error thrown. */
    suspend fun call(method: String, params: JSONObject? = null, timeoutMs: Long = 15_000): JSONObject {
        val id = ids.incrementAndGet()
        val answer = CompletableDeferred<JSONObject>()
        pending[id] = answer
        val request = JSONObject().put("id", id).put("method", method)
        if (params != null) request.put("params", params)
        try {
            send(request)
            return withTimeout(timeoutMs) { answer.await() }
        } finally {
            pending.remove(id)
        }
    }

    fun close() {
        runCatching { socket.close() }
    }

    private suspend fun send(message: JSONObject) {
        if (closed.isCompleted) throw IOException("not connected")
        withContext(Dispatchers.IO) {
            writing.withLock {
                // JSONObject.toString() never pretty-prints, so the line
                // holds no raw newline -- the one thing the framing forbids.
                writer.write(message.toString())
                writer.write("\n")
                writer.flush()
            }
        }
    }

    private suspend fun readLoop() {
        val why: Throwable = try {
            while (true) {
                val line = reader.readLine() ?: break
                if (line.isBlank()) continue
                dispatch(JSONObject(line))
            }
            IOException("the engine closed the connection")
        } catch (failure: Throwable) {
            failure
        }
        closed.complete(why)
        runCatching { socket.close() }
        val gone = why as? IOException ?: IOException(why.message, why)
        pending.values.forEach { it.completeExceptionally(gone) }
        pending.clear()
    }

    private fun dispatch(message: JSONObject) {
        if (message.has("event")) {
            _events.tryEmit(
                EngineEvent(message.getString("event"), message.optJSONObject("data") ?: JSONObject())
            )
            return
        }
        val id = message.optInt("id", -1)
        val waiting = pending[id] ?: return
        val error = message.optJSONObject("error")
        if (error != null) {
            waiting.completeExceptionally(
                EngineError(error.optString("code", "unknown"), error.optString("message", "engine error"))
            )
        } else {
            // A void answer is null: an empty object, so a caller always
            // has something to read from.
            waiting.complete(message.optJSONObject("result") ?: JSONObject())
        }
    }

    companion object {
        /**
         * Connects and, when a password is set, authenticates, so what is
         * returned is ready for any request.
         */
        suspend fun open(endpoint: Endpoint, scope: CoroutineScope, timeoutMs: Int = 5_000): EngineConnection {
            val socket = withContext(Dispatchers.IO) { endpoint.openSocket(timeoutMs) }
            val connection = EngineConnection(socket, scope)
            if (endpoint.password.isNotEmpty()) {
                try {
                    val answer = connection.call(
                        "session.authenticate", JSONObject().put("password", endpoint.password)
                    )
                    if (!answer.optBoolean("authenticated", false)) {
                        throw EngineError("unauthorized", "the engine refused the password")
                    }
                } catch (failure: Throwable) {
                    connection.close()
                    throw failure
                }
            }
            return connection
        }
    }
}
