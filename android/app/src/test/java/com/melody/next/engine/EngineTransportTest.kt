// SPDX-License-Identifier: GPL-3.0-only
package com.melody.next.engine

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.BufferedReader
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.InputStreamReader
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.security.KeyStore
import javax.net.ssl.KeyManagerFactory
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLException
import javax.net.ssl.SSLServerSocket
import javax.net.ssl.SSLSocket
import javax.net.ssl.TrustManagerFactory
import kotlin.concurrent.thread

/** ADR-0271: an engine reached as a WebSocket, through an HTTP proxy. */
class EngineTransportTest {
    // A certificate for 127.0.0.1, and one for another address.
    private fun store(name: String): KeyStore = KeyStore.getInstance("PKCS12").apply {
        EngineTransportTest::class.java.getResourceAsStream(name)!!.use { load(it, "changeit".toCharArray()) }
    }

    private val keys = store("/tls-test.p12")
    private val otherKeys = store("/tls-other.p12")

    private fun context(withKeys: Boolean, trusting: Boolean, keys: KeyStore = this.keys): SSLContext {
        val keyManagers = if (withKeys) {
            KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm())
                .apply { init(keys, "changeit".toCharArray()) }.keyManagers
        } else null
        val trustManagers = if (trusting) {
            TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm()).apply { init(keys) }.trustManagers
        } else null
        return SSLContext.getInstance("TLS").apply { init(keyManagers, trustManagers, null) }
    }

    /** One connection on 127.0.0.1, served by `serve`; the test given its port. */
    private fun withServer(server: ServerSocket, serve: (Socket) -> Unit, test: (port: Int) -> Unit) {
        val worker = thread { runCatching { server.accept().use(serve) } }
        try {
            test(server.localPort)
        } finally {
            server.close()
            worker.join(5_000)
        }
    }

    private fun tlsServer(keys: KeyStore = this.keys): ServerSocket =
        context(withKeys = true, trusting = false, keys = keys).serverSocketFactory
            .createServerSocket(0, 50, InetAddress.getByName("127.0.0.1")) as SSLServerSocket

    private val echoLine: (Socket) -> Unit = { socket ->
        val line = BufferedReader(InputStreamReader(socket.getInputStream())).readLine()
        socket.getOutputStream().write("heard $line\n".toByteArray())
        socket.getOutputStream().flush()
    }

    @Test
    fun anAddressSaysHowItIsReached() {
        assertEquals(Endpoint("music.example.org", 443, transport = Transport.WSS), Endpoint.parse("wss://music.example.org"))
        assertEquals(Endpoint("music.example.org", 443, transport = Transport.WSS), Endpoint.parse("WSS://music.example.org/"))
        assertEquals(
            Endpoint("music.example.org", 8443, transport = Transport.WSS, path = "/melody/protocol"),
            Endpoint.parse("wss://music.example.org:8443/melody/protocol"),
        )
        assertEquals(Endpoint("10.0.0.2", 80, transport = Transport.WS), Endpoint.parse("ws://10.0.0.2"))
        assertEquals(Endpoint("gemenon", 6603), Endpoint.parse("gemenon"))
        assertEquals("wss://music.example.org", Endpoint("music.example.org", 443, transport = Transport.WSS).toString())
        assertEquals(
            "wss://music.example.org:8443/melody",
            Endpoint("music.example.org", 8443, transport = Transport.WSS, path = "/melody").toString(),
        )
        assertEquals("gemenon:6603", Endpoint("gemenon").toString())
    }

    @Test
    fun streamsAreWhereThePhoneReachedTheEngine() {
        // The engine names its own address and stream port -- behind a proxy,
        // neither is the phone's to reach; the proxy serves the streams too.
        val given = "http://10.10.10.200:6604/stream?path=x&token=t"
        assertEquals(
            "https://music.example.org:443/stream?path=x&token=t",
            Endpoint("music.example.org", 443, transport = Transport.WSS).streamUrl(given),
        )
        assertEquals(
            "http://music.example.org:6604/stream?path=x&token=t",
            Endpoint("music.example.org", 6603).streamUrl(given),
        )
        assertEquals("http://[fe80::1]:6604/stream?q", Endpoint("fe80::1").streamUrl(6604, "q"))
        assertEquals("http://x/", Endpoint("music.example.org", transport = Transport.WSS).streamUrl("http://x/"))
    }

    @Test
    fun aTrustedCertificateForTheHostConnects() = withServer(tlsServer(), echoLine) { port ->
        Endpoint("127.0.0.1", port, transport = Transport.WSS)
            .openSocket(5_000, context(withKeys = false, trusting = true).socketFactory).use {
                assertTrue(it is SSLSocket)
                it.getOutputStream().write("hello\n".toByteArray())
                it.getOutputStream().flush()
                assertEquals("heard hello", BufferedReader(InputStreamReader(it.getInputStream())).readLine())
            }
    }

    @Test
    fun aCertificateForAnotherNameIsRefused() = withServer(tlsServer(otherKeys), echoLine) { port ->
        // Trusted, but for 127.0.0.9: not the address the engine was reached at.
        try {
            Endpoint("127.0.0.1", port, transport = Transport.WSS)
                .openSocket(5_000, context(withKeys = false, trusting = true, keys = otherKeys).socketFactory)
                .close()
            fail("connected to a host the certificate does not name")
        } catch (_: SSLException) {
        }
    }

    @Test
    fun anUntrustedCertificateIsRefused() = withServer(tlsServer(), echoLine) { port ->
        try {
            Endpoint("127.0.0.1", port, transport = Transport.WSS).openSocket(5_000).close()
            fail("connected with a certificate nobody vouches for")
        } catch (_: SSLException) {
        }
    }

    // --- A WebSocket server as the engine's bridge is ------------------------

    private class ServerSide(socket: Socket) {
        val input = DataInputStream(socket.getInputStream())
        val output = socket.getOutputStream()
        lateinit var request: String

        fun handshake() {
            val head = StringBuilder()
            while (!head.endsWith("\r\n\r\n")) head.append(input.read().toChar())
            request = head.toString()
            val key = request.split("\r\n").first { it.startsWith("Sec-WebSocket-Key:") }.substringAfter(':').trim()
            output.write(
                ("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" +
                    "Sec-WebSocket-Accept: ${WebSocketLines.accept(key)}\r\n\r\n").toByteArray()
            )
            output.flush()
        }

        /** A client frame, unmasked: its opcode and payload. */
        fun frame(): Pair<Int, String> {
            val first = input.readUnsignedByte()
            val second = input.readUnsignedByte()
            assertTrue("a client's frame is masked", second and 0x80 != 0)
            var size = second and 0x7F
            if (size == 126) size = input.readUnsignedShort()
            val mask = ByteArray(4).also { input.readFully(it) }
            val payload = ByteArray(size).also { input.readFully(it) }
            for (index in payload.indices) payload[index] = (payload[index].toInt() xor mask[index % 4].toInt()).toByte()
            return (first and 0x0F) to String(payload, Charsets.UTF_8)
        }

        fun send(opcode: Int, payload: String, fin: Boolean = true) {
            val bytes = payload.toByteArray()
            val out = ByteArrayOutputStream()
            out.write((if (fin) 0x80 else 0) or opcode)
            if (bytes.size < 126) out.write(bytes.size) else {
                out.write(126); out.write(bytes.size shr 8); out.write(bytes.size and 0xFF)
            }
            out.write(bytes)
            output.write(out.toByteArray())
            output.flush()
        }
    }

    @Test
    fun linesTravelAsWebSocketMessages() {
        val server = ServerSocket(0, 50, InetAddress.getByName("127.0.0.1"))
        var seenRequest = ""
        var pong = ""
        var closedWith = -1
        withServer(server, { socket ->
            val side = ServerSide(socket)
            side.handshake()
            seenRequest = side.request
            val (opcode, line) = side.frame()
            assertEquals(1, opcode)
            // An answer longer than a short frame holds, in two parts.
            val answer = "{\"result\":{\"echo\":\"" + "x".repeat(300) + "\"},\"id\":1}"
            side.send(0x9, "anyone?")
            pong = side.frame().second
            side.send(0x1, answer.substring(0, 100), fin = false)
            side.send(0x0, answer.substring(100))
            assertEquals("{\"id\":1,\"method\":\"test.echo\"}", line)
            side.send(0x8, "")
            closedWith = side.frame().first
        }) { port ->
            val lines = Endpoint("127.0.0.1", port, transport = Transport.WS, path = "/protocol").openLines(5_000)
            lines.write("{\"id\":1,\"method\":\"test.echo\"}")
            val answer = lines.read()
            assertEquals("{\"result\":{\"echo\":\"" + "x".repeat(300) + "\"},\"id\":1}", answer)
            assertNull("a close ends it", lines.read())
            lines.close()
        }
        assertTrue(seenRequest.startsWith("GET /protocol HTTP/1.1\r\n"))
        assertTrue("routed by its host", seenRequest.contains("\r\nHost: 127.0.0.1:"))
        assertEquals("a ping is answered with its payload", "anyone?", pong)
        assertEquals("a close is returned", 0x8, closedWith)
    }

    @Test
    fun aWebSocketGoneQuietIsClosed() {
        val server = ServerSocket(0, 50, InetAddress.getByName("127.0.0.1"))
        var pings = 0
        withServer(server, { socket ->
            val side = ServerSide(socket)
            side.handshake()
            // Answering pings for longer than the silence allowed: kept.
            val answering = System.nanoTime() + 1_000_000_000L
            while (System.nanoTime() < answering) {
                val (opcode, payload) = side.frame()
                if (opcode == 0x9) {
                    pings++
                    side.send(0xA, payload)
                }
            }
            // Then nothing, as a connection gone with the phone's address.
            runCatching { while (true) side.frame() }
        }) { port ->
            val socket = Socket().apply { connect(java.net.InetSocketAddress("127.0.0.1", port), 5_000) }
            val lines = WebSocketLines.open(socket, "127.0.0.1", port, "/protocol", 80, pingMs = 100, silentMs = 400)
            val started = System.nanoTime()
            try {
                lines.read()
                fail("a silent connection must not be read from for ever")
            } catch (gone: java.io.IOException) {
                assertEquals("the engine stopped answering", gone.message)
            }
            val tookMs = (System.nanoTime() - started) / 1_000_000
            assertTrue("kept while pongs came ($tookMs ms)", tookMs >= 1_000)
            assertTrue("closed soon after they stopped ($tookMs ms)", tookMs < 3_000)
            lines.close()
        }
        assertTrue("pinged ($pings)", pings >= 5)
    }

    @Test
    fun aServerThatDoesNotTakeTheWebSocketIsSaidSo() {
        val server = ServerSocket(0, 50, InetAddress.getByName("127.0.0.1"))
        withServer(server, { socket ->
            val input = BufferedReader(InputStreamReader(socket.getInputStream()))
            while (input.readLine().isNotEmpty()) {}
            socket.getOutputStream().write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n".toByteArray())
            socket.getOutputStream().flush()
        }) { port ->
            try {
                Endpoint("127.0.0.1", port, transport = Transport.WS).openLines(5_000)
                fail("took a 404 for a WebSocket")
            } catch (refused: java.io.IOException) {
                assertTrue(refused.message!!.contains("404"))
            }
        }
    }
}
