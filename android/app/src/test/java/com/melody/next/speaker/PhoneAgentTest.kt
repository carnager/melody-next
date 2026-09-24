package com.melody.next.speaker

import com.melody.next.engine.Endpoint
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.json.JSONObject
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.PrintWriter
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.Executors
import kotlinx.coroutines.asCoroutineDispatcher

/** A player that does what it is told and says so. */
private class StandIn : Audition {
    val asked = CopyOnWriteArrayList<String>()
    var state = 0
    var position = 0L
    override fun load(source: JSONObject, play: Boolean, positionMs: Long) {
        asked += "load ${source.getString("url")} play=$play at=$positionMs"
        state = if (play) 4 else 5
        position = positionMs
    }
    override fun queueNext(source: JSONObject, token: Long) { asked += "next ${source.getString("url")} token=$token" }
    override fun clearNext() { asked += "clear_next" }
    override fun play() { asked += "play"; state = 4 }
    override fun pause() { asked += "pause"; state = 5 }
    override fun stop() { asked += "stop"; state = 0 }
    override fun seek(seconds: Double) { asked += "seek $seconds" }
    override fun setVolume(percent: Int) { asked += "volume $percent" }
    override fun setReplayGain(params: JSONObject) { asked += "gain ${params.optInt("mode", -1)}" }
    override fun setBuffer(capacityMs: Long, startMs: Long) { asked += "buffer $capacityMs" }
    override val playing get() = state == 4
    override val loaded get() = state != 0
    override fun snapshot(): JSONObject = JSONObject()
        .put("state", state)
        .put("format", JSONObject().put("sample_rate", 1000).put("channels", 2))
        .put("position_sample", position)
}

/** The engine's end of an agent connection: it takes the registration, then asks. */
private class FakeEngine(val password: String = "") : AutoCloseable {
    val server = ServerSocket(0)
    @Volatile var socket: Socket? = null
    lateinit var reader: BufferedReader
    lateinit var writer: PrintWriter
    val registration = CopyOnWriteArrayList<JSONObject>()
    val events = CopyOnWriteArrayList<JSONObject>()
    private val answers = java.util.concurrent.LinkedBlockingQueue<JSONObject>()

    val endpoint get() = Endpoint("127.0.0.1", server.localPort, password)

    /** Takes the next agent, answers its handshake, and listens. */
    fun accept() {
        val accepted = server.accept()
        socket = accepted
        reader = BufferedReader(InputStreamReader(accepted.getInputStream()))
        writer = PrintWriter(accepted.getOutputStream(), true)
        while (true) {
            val request = JSONObject(reader.readLine())
            registration += request
            writer.println(JSONObject().put("id", request.getInt("id")).put("result",
                if (request.getString("method") == "session.authenticate") JSONObject().put("authenticated", true) else JSONObject()))
            if (request.getString("method") == "agent.register") break
        }
        Thread {
            while (true) {
                val line = runCatching { reader.readLine() }.getOrNull() ?: break
                val message = JSONObject(line)
                if (message.has("event")) events += message else answers.put(message)
            }
        }.apply { isDaemon = true }.start()
    }

    fun ask(id: Int, method: String, params: JSONObject = JSONObject()): JSONObject {
        writer.println(JSONObject().put("id", id).put("method", method).put("params", params))
        return answers.poll(5, java.util.concurrent.TimeUnit.SECONDS) ?: error("no answer to $method")
    }

    fun drop() = socket?.close()

    override fun close() {
        socket?.close()
        server.close()
    }
}

class PhoneAgentTest {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val playerThread = Executors.newSingleThreadExecutor().asCoroutineDispatcher()

    @After
    fun tearDown() {
        scope.cancel()
        playerThread.close()
    }

    private suspend fun eventually(what: String, check: () -> Boolean) {
        try {
            withTimeout(5_000) { while (!check()) delay(20) }
        } catch (_: Throwable) {
            fail("never: $what")
        }
    }

    @Test
    fun itRegistersAsAnAgentWithoutFilesThenAnswersAndReports() = runBlocking {
        FakeEngine(password = "correct horse").use { engine ->
            val player = StandIn()
            val agent = PhoneAgent(scope, player, "Pixel", playerThread, retryMs = 100)
            agent.start(engine.endpoint)
            engine.accept()

            // The password first, then who it is: a phone has no copy of the
            // music, so the engine streams it.
            assertEquals("session.authenticate", engine.registration[0].getString("method"))
            val register = engine.registration[1].getJSONObject("params")
            assertEquals("Pixel", register.getString("name"))
            assertEquals(false, register.getBoolean("files"))
            assertEquals(1, register.getInt("protocol"))
            assertTrue(register.getString("instance").isNotEmpty())
            eventually("registered") { agent.status.value is PhoneAgent.Status.Registered }

            // The connection has turned round: the engine asks, in order.
            val source = JSONObject().put("url", "http://engine:6604/stream?path=x&token=t")
            assertTrue(engine.ask(10, "audition.load", JSONObject().put("source", source).put("play", true).put("position_ms", 1500)).has("result"))
            assertTrue(engine.ask(11, "audition.queue_next", JSONObject().put("source", source).put("token", 7)).has("result"))
            assertTrue(engine.ask(12, "audition.volume", JSONObject().put("percent", 40)).has("result"))
            assertTrue(engine.ask(13, "audition.replay_gain", JSONObject().put("mode", 2)).has("result"))
            assertEquals(
                listOf("load http://engine:6604/stream?path=x&token=t play=true at=1500",
                    "next http://engine:6604/stream?path=x&token=t token=7", "volume 40", "gain 2"),
                player.asked.toList(),
            )
            // What it does not do is refused, not ignored: the engine hears why.
            val refused = engine.ask(14, "audition.invented")
            assertEquals("unsupported", refused.getJSONObject("error").getString("code"))
            assertEquals(14, refused.getInt("id"))

            // And it says how it is playing, so the engine follows the track.
            eventually("a report while playing") {
                engine.events.any { it.getString("event") == "audition.changed" && it.getJSONObject("data").getInt("state") == 4 }
            }
            val reports = engine.events.size
            delay(1_000)
            assertTrue("reported again and again while playing", engine.events.size >= reports + 2)
            agent.stop()
        }
    }

    @Test
    fun whenTheEngineGoesItPausesAndComesBack() = runBlocking {
        FakeEngine().use { engine ->
            val player = StandIn()
            val agent = PhoneAgent(scope, player, "Pixel", playerThread, retryMs = 100)
            agent.start(engine.endpoint)
            engine.accept()
            engine.ask(1, "audition.load", JSONObject().put("source", JSONObject().put("url", "http://x/")).put("play", true))
            engine.drop()
            // What played belonged to that connection: paused, not playing on
            // with nobody to follow it.
            eventually("paused") { player.asked.contains("pause") }
            eventually("connecting again") { agent.status.value is PhoneAgent.Status.Connecting }
            engine.accept()
            eventually("registered again") { agent.status.value is PhoneAgent.Status.Registered }
            agent.stop()
        }
    }
}
