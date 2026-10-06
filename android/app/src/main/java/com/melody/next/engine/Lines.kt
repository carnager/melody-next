// SPDX-License-Identifier: GPL-3.0-only
package com.melody.next.engine

import java.io.BufferedReader
import java.io.BufferedWriter
import java.io.ByteArrayOutputStream
import java.io.Closeable
import java.io.DataInputStream
import java.io.IOException
import java.io.InputStreamReader
import java.io.OutputStream
import java.io.OutputStreamWriter
import java.net.Socket
import java.security.MessageDigest
import java.security.SecureRandom
import java.util.Base64
import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.thread
import kotlin.concurrent.withLock

/**
 * Protocol v1 as it travels: one message a line, both ways (ADR-0222) --
 * over a socket, or as WebSocket text messages through an HTTP proxy
 * (ADR-0271).
 */
interface Lines : Closeable {
    /** The next message; null once the engine has gone. Blocks. */
    fun read(): String?

    /** Sends one message. Blocks; callers write one at a time. */
    fun write(line: String)
}

/** Lines on a socket, as the engine's own port speaks them. */
class SocketLines(private val socket: Socket) : Lines {
    private val reader = BufferedReader(InputStreamReader(socket.getInputStream(), Charsets.UTF_8))
    private val writer = BufferedWriter(OutputStreamWriter(socket.getOutputStream(), Charsets.UTF_8))

    override fun read(): String? = reader.readLine()

    override fun write(line: String) {
        writer.write(line)
        writer.write("\n")
        writer.flush()
    }

    override fun close() {
        runCatching { socket.close() }
    }
}

/**
 * Lines as WebSocket text messages (RFC 6455), a client's: its frames masked,
 * pings answered, a close returned. Only what talking to an engine needs.
 *
 * And pinged: a connection over mobile data dies without a word when the
 * phone's address changes -- WireGuard roams, a bare connection does not --
 * and an agent waiting for its next track would wait for good. Every
 * [pingMs] a ping goes; nothing heard for [silentMs], pongs included, and
 * the connection is closed: reading ends, and it is made again.
 */
class WebSocketLines private constructor(
    private val socket: Socket,
    private val input: DataInputStream,
    private val output: OutputStream,
    private val pingMs: Long,
    private val silentMs: Long,
) : Lines {
    private val random = SecureRandom()
    private val writing = ReentrantLock()
    @Volatile private var closed = false
    @Volatile private var heard = System.nanoTime()
    @Volatile private var silent = false

    init {
        thread(isDaemon = true, name = "websocket-ping") { keepAlive() }
    }

    private fun keepAlive() {
        while (!closed) {
            try {
                Thread.sleep(pingMs)
            } catch (_: InterruptedException) {
                return
            }
            if (closed) return
            if ((System.nanoTime() - heard) / 1_000_000 >= silentMs) {
                // Gone without a word: closing ends the read waiting on it.
                silent = true
                closed = true
                runCatching { socket.close() }
                return
            }
            // Never behind a write stuck on a dead connection: the check
            // above must go on running. Someone writing, the ping waits.
            if (!writing.tryLock()) continue
            try {
                output.write(frame(PING, ByteArray(0)))
                output.flush()
            } catch (_: IOException) {
                closed = true
                runCatching { socket.close() }
                return
            } finally {
                writing.unlock()
            }
        }
    }

    companion object {
        private const val GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
        // As the engine's: a message larger than a line may be is refused.
        private const val MAXIMUM_MESSAGE = 1 shl 20
        private const val CONTINUATION = 0x0
        private const val TEXT = 0x1
        private const val BINARY = 0x2
        private const val CLOSE = 0x8
        private const val PING = 0x9
        private const val PONG = 0xA
        /** WireGuard's keepalive is 25 s; a mobile network's quiet is shorter. */
        const val PING_MS = 10_000L
        const val SILENT_MS = 25_000L

        /** The handshake's answer to `key`, as a server must give it. */
        fun accept(key: String): String = Base64.getEncoder().encodeToString(
            MessageDigest.getInstance("SHA-1").digest((key + GUID).toByteArray(Charsets.US_ASCII))
        )

        /**
         * Asks `socket` -- connected, and secured for wss -- to carry a
         * WebSocket at `path`; `host` is what the proxy routes by.
         */
        fun open(
            socket: Socket,
            host: String,
            port: Int,
            path: String,
            defaultPort: Int,
            pingMs: Long = PING_MS,
            silentMs: Long = SILENT_MS,
        ): WebSocketLines {
            val input = DataInputStream(socket.getInputStream().buffered())
            val output = socket.getOutputStream()
            val key = Base64.getEncoder().encodeToString(ByteArray(16).also { SecureRandom().nextBytes(it) })
            val authority = (if (host.contains(':')) "[$host]" else host) + if (port == defaultPort) "" else ":$port"
            val request = "GET $path HTTP/1.1\r\nHost: $authority\r\nUpgrade: websocket\r\n" +
                "Connection: Upgrade\r\nSec-WebSocket-Key: $key\r\nSec-WebSocket-Version: 13\r\n\r\n"
            output.write(request.toByteArray(Charsets.US_ASCII))
            output.flush()
            val head = StringBuilder()
            while (!head.endsWith("\r\n\r\n")) {
                if (head.length > 16 * 1024) throw IOException("the WebSocket handshake was not answered")
                val byte = input.read()
                if (byte < 0) throw IOException("the server closed before answering the WebSocket")
                head.append(byte.toChar())
            }
            val lines = head.split("\r\n")
            val status = lines.first()
            if (!status.startsWith("HTTP/1.1 101")) {
                throw IOException("the server did not take the WebSocket: ${status.removePrefix("HTTP/1.1 ")}")
            }
            val accepted = lines.drop(1).firstOrNull { it.startsWith("Sec-WebSocket-Accept:", ignoreCase = true) }
                ?.substringAfter(':')?.trim()
            if (accepted != accept(key)) throw IOException("the WebSocket handshake was answered wrongly")
            return WebSocketLines(socket, input, output, pingMs, silentMs)
        }
    }

    override fun read(): String? = try {
        readMessage()
    } catch (failure: IOException) {
        if (silent) throw IOException("the engine stopped answering", failure)
        throw failure
    }

    private fun readMessage(): String? {
        val message = ByteArrayOutputStream()
        var inMessage = false
        while (true) {
            val first = input.read()
            if (first < 0) return null
            heard = System.nanoTime()
            val second = input.readUnsignedByte()
            val fin = first and 0x80 != 0
            val opcode = first and 0x0F
            if (second and 0x80 != 0) throw IOException("a server's frame was masked")
            var size = (second and 0x7F).toLong()
            if (size == 126L) size = input.readUnsignedShort().toLong()
            else if (size == 127L) size = input.readLong()
            if (size < 0 || size > MAXIMUM_MESSAGE) throw IOException("the engine sent a message too large")
            val payload = ByteArray(size.toInt()).also { input.readFully(it) }
            when (opcode) {
                PING -> send(PONG, payload)
                PONG -> {}
                CLOSE -> {
                    if (!closed) runCatching { send(CLOSE, payload) }
                    return null
                }
                TEXT, BINARY, CONTINUATION -> {
                    if ((opcode == CONTINUATION) != inMessage) throw IOException("a WebSocket message broke off")
                    message.write(payload)
                    if (message.size() > MAXIMUM_MESSAGE) throw IOException("the engine sent a message too large")
                    if (!fin) {
                        inMessage = true
                        continue
                    }
                    return message.toString(Charsets.UTF_8)
                }
                else -> throw IOException("the engine sent a frame of an unknown kind")
            }
        }
    }

    override fun write(line: String) = send(TEXT, line.toByteArray(Charsets.UTF_8))

    private fun send(opcode: Int, payload: ByteArray) {
        val bytes = frame(opcode, payload)
        writing.withLock {
            output.write(bytes)
            output.flush()
        }
    }

    // A client's frame: always masked (RFC 6455, 5.3).
    private fun frame(opcode: Int, payload: ByteArray): ByteArray {
        val mask = ByteArray(4).also { random.nextBytes(it) }
        val frame = ByteArrayOutputStream(payload.size + 14)
        frame.write(0x80 or opcode)
        when {
            payload.size < 126 -> frame.write(0x80 or payload.size)
            payload.size <= 0xFFFF -> {
                frame.write(0x80 or 126)
                frame.write(payload.size shr 8)
                frame.write(payload.size and 0xFF)
            }
            else -> {
                frame.write(0x80 or 127)
                for (shift in 56 downTo 0 step 8) frame.write(((payload.size.toLong() shr shift) and 0xFF).toInt())
            }
        }
        frame.write(mask)
        for (index in payload.indices) frame.write(payload[index].toInt() xor mask[index % 4].toInt())
        return frame.toByteArray()
    }

    override fun close() {
        if (!closed) {
            closed = true
            runCatching { send(CLOSE, byteArrayOf(0x03, 0xE8.toByte())) }
        }
        runCatching { socket.close() }
    }
}
