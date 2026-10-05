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
import java.io.IOException
import java.net.InetSocketAddress
import java.net.Socket
import java.net.URI
import javax.net.ssl.SSLSocket
import javax.net.ssl.SSLSocketFactory
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicInteger

/** How an engine is reached: its own port, or a WebSocket through an HTTP proxy. */
enum class Transport(val scheme: String, val defaultPort: Int) {
    TCP("", Endpoint.DEFAULT_PORT),
    WS("ws", 80),
    WSS("wss", 443),
}

/**
 * Where an engine listens, and the password it wants, if any. Reached as a
 * WebSocket ([Transport.WS], [Transport.WSS]) it sits behind an HTTP proxy
 * that forwards the whole site to the engine's stream port (ADR-0271).
 */
data class Endpoint(
    val host: String,
    val port: Int = DEFAULT_PORT,
    val password: String = "",
    val transport: Transport = Transport.TCP,
    val path: String = DEFAULT_PATH,
) {
    companion object {
        const val DEFAULT_PORT = 6603
        const val DEFAULT_PATH = "/protocol"

        /**
         * "host", "host:port" or "[v6]:port", as a person types it; or a
         * ws:// or wss:// address, with a port and path if not the usual.
         */
        fun parse(text: String, password: String = ""): Endpoint? {
            var rest = text.trim()
            var transport = Transport.TCP
            for (candidate in listOf(Transport.WSS, Transport.WS)) {
                val prefix = candidate.scheme + "://"
                if (rest.startsWith(prefix, ignoreCase = true)) {
                    transport = candidate
                    rest = rest.substring(prefix.length)
                }
            }
            var path = DEFAULT_PATH
            if (transport != Transport.TCP) {
                val slash = rest.indexOf('/')
                if (slash >= 0) {
                    path = rest.substring(slash).ifEmpty { DEFAULT_PATH }.let { if (it == "/") DEFAULT_PATH else it }
                    rest = rest.substring(0, slash)
                }
            }
            if (rest.isEmpty()) return null
            val usual = transport.defaultPort
            if (rest.startsWith("[")) {
                val close = rest.indexOf(']')
                if (close < 0) return null
                val host = rest.substring(1, close)
                val port = rest.substring(close + 1).removePrefix(":").toIntOrNull() ?: usual
                return Endpoint(host, port, password, transport, path)
            }
            val colon = rest.lastIndexOf(':')
            // More than one colon without brackets is a bare IPv6 address.
            if (colon < 0 || rest.indexOf(':') != colon) return Endpoint(rest, usual, password, transport, path)
            val port = rest.substring(colon + 1).toIntOrNull() ?: return null
            return Endpoint(rest.substring(0, colon), port, password, transport, path)
        }
    }

    private val authorityHost: String get() = if (host.contains(':')) "[$host]" else host

    override fun toString(): String = when (transport) {
        Transport.TCP -> "$authorityHost:$port"
        else -> "${transport.scheme}://$authorityHost" +
            (if (port == transport.defaultPort) "" else ":$port") +
            (if (path == DEFAULT_PATH) "" else path)
    }

    /**
     * A stream of this engine's, as this phone reaches it: at the host it
     * connected to -- which behind a proxy is not the address the engine sees
     * itself at. On its own port, the engine names its stream port; through
     * a proxy, the streams are the same site's (ADR-0271).
     */
    fun streamUrl(port: Int, query: String): String = when (transport) {
        Transport.TCP -> "http://$authorityHost:$port/stream?$query"
        Transport.WS -> "http://$authorityHost:${this.port}/stream?$query"
        Transport.WSS -> "https://$authorityHost:${this.port}/stream?$query"
    }

    /** An address the engine gave for a stream, taken to this host likewise. */
    fun streamUrl(given: String): String {
        val uri = runCatching { URI(given) }.getOrNull() ?: return given
        if (uri.port < 0 || uri.rawPath != "/stream") return given
        return streamUrl(uri.port, uri.rawQuery ?: "")
    }

    /** The engine's lines: on its own port, or as a WebSocket through a proxy. */
    fun openLines(timeoutMs: Int, factory: SSLSocketFactory? = null): Lines = when (transport) {
        Transport.TCP -> SocketLines(openSocket(timeoutMs))
        else -> {
            val socket = openSocket(timeoutMs, factory)
            try {
                socket.soTimeout = timeoutMs
                WebSocketLines.open(socket, host, port, path, transport.defaultPort).also { socket.soTimeout = 0 }
            } catch (failure: Throwable) {
                runCatching { socket.close() }
                throw failure
            }
        }
    }

    /**
     * A socket to the engine, connected -- for wss, the handshake made and the
     * certificate checked against the host's name, as a browser does.
     */
    fun openSocket(timeoutMs: Int, factory: SSLSocketFactory? = null): Socket {
        // Named: inside apply, `port` would be the unconnected socket's own.
        val address = InetSocketAddress(host, port)
        val plain = Socket().apply {
            tcpNoDelay = true
            keepAlive = true
            connect(address, timeoutMs)
        }
        if (transport != Transport.WSS) return plain
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
    private val lines: Lines,
    scope: CoroutineScope,
) {    private val writing = Mutex()
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
        runCatching { lines.close() }
    }

    private suspend fun send(message: JSONObject) {
        if (closed.isCompleted) throw IOException("not connected")
        withContext(Dispatchers.IO) {
            writing.withLock {
                // JSONObject.toString() never pretty-prints, so the line
                // holds no raw newline -- the one thing the framing forbids.
                lines.write(message.toString())
            }
        }
    }

    private suspend fun readLoop() {
        val why: Throwable = try {
            while (true) {
                val line = lines.read() ?: break
                if (line.isBlank()) continue
                dispatch(JSONObject(line))
            }
            IOException("the engine closed the connection")
        } catch (failure: Throwable) {
            failure
        }
        closed.complete(why)
        runCatching { lines.close() }
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
            val lines = withContext(Dispatchers.IO) { endpoint.openLines(timeoutMs) }
            val connection = EngineConnection(lines, scope)
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
