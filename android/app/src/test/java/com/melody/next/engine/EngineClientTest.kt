package com.melody.next.engine

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.json.JSONArray
import org.json.JSONObject
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.PrintWriter
import java.net.ServerSocket
import java.net.Socket
import java.util.Base64
import java.util.concurrent.CopyOnWriteArrayList
import kotlin.concurrent.thread

/**
 * A stand-in engine on a local socket: answers what it is scripted to,
 * records what it was asked, and can push events -- enough to hold the
 * client to protocol v1 without a real engine.
 */
private class FakeEngine(private val password: String = "", port: Int = 0) : AutoCloseable {
    val server = ServerSocket().apply { reuseAddress = true; bind(java.net.InetSocketAddress("127.0.0.1", port)) }
    private val sockets = CopyOnWriteArrayList<Socket>()
    val asked = CopyOnWriteArrayList<JSONObject>()
    val clients = CopyOnWriteArrayList<PrintWriter>()
    var state = JSONObject().put("status", "stopped").put("queue_revision", 1).put("queue_size", 1)
    var queue = JSONArray().put(entry("a", "Alpha"))

    init {
        thread(isDaemon = true) {
            while (!server.isClosed) {
                val socket = runCatching { server.accept() }.getOrNull() ?: break
                thread(isDaemon = true) { serve(socket) }
            }
        }
    }

    val endpoint get() = Endpoint("127.0.0.1", server.localPort, password)

    fun push(event: String, data: JSONObject) = clients.forEach {
        it.println(JSONObject().put("event", event).put("data", data)); it.flush()
    }

    private fun serve(socket: Socket) {
        sockets += socket
        val reader = BufferedReader(InputStreamReader(socket.getInputStream()))
        val writer = PrintWriter(socket.getOutputStream(), true)
        clients += writer
        var authenticated = password.isEmpty()
        while (true) {
            val line = runCatching { reader.readLine() }.getOrNull() ?: break
            val request = JSONObject(line)
            asked += request
            val id = request.getInt("id")
            val method = request.getString("method")
            val params = request.optJSONObject("params") ?: JSONObject()
            val answer = JSONObject().put("id", id)
            when {
                method == "session.authenticate" ->
                    if (params.optString("password") == password) {
                        authenticated = true
                        answer.put("result", JSONObject().put("authenticated", true))
                    } else {
                        answer.put("error", JSONObject().put("code", "unauthorized").put("message", "wrong password"))
                    }
                !authenticated -> answer.put("error", JSONObject().put("code", "unauthorized").put("message", "authenticate first"))
                method == "engine.info" -> answer.put("result", JSONObject().put("name", "fake").put("protocol", 1))
                method == "playback.state" -> answer.put("result", state)
                method == "playback.queue" -> answer.put("result", JSONObject().put("entries", queue))
                method == "playback.requests" -> answer.put("result", JSONObject().put("entries", JSONArray()))
                method == "outputs.list" -> answer.put("result", JSONObject().put("outputs", JSONArray()))
                method == "playback.play" -> answer.put("error", JSONObject().put("code", "not_found").put("message", "no such entry"))
                method == "catalogue.artwork" -> answer.put("result", JSONObject().put("image", JSONObject.NULL))
                else -> answer.put("result", JSONObject.NULL)
            }
            writer.println(answer)
        }
        clients -= writer
    }

    override fun close() {
        server.close()
        sockets.forEach { runCatching { it.close() } }
    }

    companion object {
        fun entry(id: String, title: String): JSONObject = JSONObject()
            .put("entry", "00000000-0000-0000-0000-00000000000$id".takeLast(36).padStart(36, '0'))
            .put("path", Base64.getEncoder().encodeToString("/music/$title.flac".toByteArray()))
            .put("title", title)
            .put("duration_ms", 1000)
            .put("group", JSONObject().put("album", "Album").put("artist", "Artist"))
            .put("segment", JSONObject().put("start_sample", 5))
    }
}

class EngineClientTest {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    @After
    fun tearDown() = scope.cancel()

    private suspend fun <T> eventually(what: String, read: () -> T, done: (T) -> Boolean): T {
        return try {
            withTimeout(5_000) {
                while (true) {
                    val value = read()
                    if (done(value)) return@withTimeout value
                    delay(20)
                }
                @Suppress("UNREACHABLE_CODE") read()
            }
        } catch (_: Throwable) {
            fail("never: $what (last ${read()})"); throw IllegalStateException()
        }
    }

    @Test
    fun anAddressIsReadAsTyped() {
        assertEquals(Endpoint("gemenon", 6603), Endpoint.parse("gemenon"))
        assertEquals(Endpoint("10.0.0.2", 7000), Endpoint.parse(" 10.0.0.2:7000 "))
        assertEquals(Endpoint("fe80::1", 6603), Endpoint.parse("fe80::1"))
        assertEquals(Endpoint("fe80::1", 7000), Endpoint.parse("[fe80::1]:7000"))
        assertEquals(null, Endpoint.parse("host:notaport"))
    }

    @Test
    fun requestsAreLinesAnsweredByIdAndErrorsAreThrown() = runBlocking {
        FakeEngine().use { engine ->
            val connection = EngineConnection.open(engine.endpoint, scope)
            assertEquals("fake", connection.call("engine.info").getString("name"))
            // A void answer is an empty object, not null.
            assertEquals(0, connection.call("playback.stop").length())
            try {
                connection.call("playback.play", JSONObject().put("entry", "x"))
                fail("an error must be thrown")
            } catch (error: EngineError) {
                assertEquals("not_found", error.code)
            }
            assertEquals(3, engine.asked.size)
            assertEquals(listOf(1, 2, 3), engine.asked.map { it.getInt("id") })
            connection.close()
        }
    }

    @Test
    fun aPasswordIsGivenFirstAndAWrongOneIsNotRetried() = runBlocking {
        FakeEngine(password = "correct horse").use { engine ->
            val connection = EngineConnection.open(engine.endpoint, scope)
            assertEquals("session.authenticate", engine.asked.first().getString("method"))
            assertEquals("fake", connection.call("engine.info").getString("name"))
            connection.close()

            val client = EngineClient(scope, clock = { 0L })
            client.connect(engine.endpoint.copy(password = "wrong"))
            eventually("refused", { client.connection.value }) { it is ConnectionState.Refused }
            val tries = engine.asked.count { it.getString("method") == "session.authenticate" }
            delay(2_500)
            assertEquals("a refused password is not tried again", tries,
                engine.asked.count { it.getString("method") == "session.authenticate" })
            client.disconnect()
        }
    }

    @Test
    fun theClientMirrorsStateAndRefetchesTheQueueWhenItChanged() = runBlocking {
        FakeEngine().use { engine ->
            val client = EngineClient(scope, clock = { 0L })
            client.connect(engine.endpoint)
            eventually("connected", { client.connection.value }) { it is ConnectionState.Connected }
            eventually("the queue", { client.queue.value }) { it.size == 1 }
            assertEquals("Alpha", client.queue.value.first().title)

            // Another client changed the queue: the revision says so.
            engine.queue = JSONArray().put(FakeEngine.entry("a", "Alpha")).put(FakeEngine.entry("b", "Beta"))
            engine.push("playback.changed", JSONObject().put("status", "playing").put("queue_revision", 2).put("position_ms", 500))
            eventually("the new queue", { client.queue.value }) { it.size == 2 }
            assertTrue(client.state.value.playing)
            assertEquals(500, client.state.value.positionMs)

            // An edit sends back the engine's own entries, whole: what this
            // client does not show -- a segment -- survives it.
            client.removeFromQueue(client.queue.value.last().entry)
            val sent = eventually("the edit", { engine.asked.lastOrNull { it.getString("method") == "playback.replace_queue" } }) { it != null }!!
            val entries = sent.getJSONObject("params").getJSONArray("entries")
            assertEquals(1, entries.length())
            assertEquals(5, entries.getJSONObject(0).getJSONObject("segment").getInt("start_sample"))
            client.disconnect()
        }
    }

    @Test
    fun aDroppedConnectionIsMadeAgain() = runBlocking {
        val engine = FakeEngine()
        val client = EngineClient(scope, clock = { 0L })
        client.connect(engine.endpoint)
        eventually("connected", { client.connection.value }) { it is ConnectionState.Connected }
        val port = engine.server.localPort
        engine.close()
        eventually("noticed", { client.connection.value }) { it is ConnectionState.Connecting }
        // The engine is back, on the same port: found again without asking.
        FakeEngine(port = port).use {
            eventually("connected again", { client.connection.value }) { it is ConnectionState.Connected }
        }
        client.disconnect()
    }

    @Test
    fun theClockCountsOnWhilePlaying() {
        val state = PlaybackState(status = "playing", positionMs = 1_000, durationMs = 3_000, receivedAtMs = 10_000)
        assertEquals(1_500, state.positionAt(10_500))
        assertEquals("never past the end", 3_000, state.positionAt(20_000))
        assertEquals("paused stays put", 1_000, state.copy(status = "paused").positionAt(20_000))
    }
}
