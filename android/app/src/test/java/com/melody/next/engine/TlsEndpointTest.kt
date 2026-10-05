// SPDX-License-Identifier: GPL-3.0-only
package com.melody.next.engine

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.BufferedReader
import java.io.InputStreamReader
import java.net.InetAddress
import java.security.KeyStore
import javax.net.ssl.KeyManagerFactory
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLException
import javax.net.ssl.SSLServerSocket
import javax.net.ssl.SSLSocket
import javax.net.ssl.TrustManagerFactory
import kotlin.concurrent.thread

/** ADR-0270: an engine reached through a proxy that speaks TLS. */
class TlsEndpointTest {
    // A certificate for 127.0.0.1, and one for another address.
    private fun store(name: String): KeyStore = KeyStore.getInstance("PKCS12").apply {
        TlsEndpointTest::class.java.getResourceAsStream(name)!!.use { load(it, "changeit".toCharArray()) }
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

    /** A proxy's TLS end, with `keys`' certificate: answers one line with what it read. */
    private fun withServer(keys: KeyStore = this.keys, test: (port: Int) -> Unit) {
        val server = context(withKeys = true, trusting = false, keys = keys).serverSocketFactory
            .createServerSocket(0, 50, InetAddress.getByName("127.0.0.1")) as SSLServerSocket
        val worker = thread {
            runCatching {
                server.accept().use { socket ->
                    val line = BufferedReader(InputStreamReader(socket.getInputStream())).readLine()
                    socket.getOutputStream().write("heard $line\n".toByteArray())
                    socket.getOutputStream().flush()
                }
            }
        }
        try {
            test(server.localPort)
        } finally {
            server.close()
            worker.join(5_000)
        }
    }

    @Test
    fun anAddressSaysTls() {
        assertEquals(Endpoint("vps.example.org", 7000, tls = true), Endpoint.parse("tls://vps.example.org:7000"))
        assertEquals(Endpoint("vps.example.org", 6603, tls = true), Endpoint.parse("TLS://vps.example.org"))
        assertEquals(Endpoint("fe80::1", 7000, tls = true), Endpoint.parse("tls://[fe80::1]:7000"))
        assertEquals("tls://vps.example.org:7000", Endpoint("vps.example.org", 7000, tls = true).toString())
        assertEquals("gemenon:6603", Endpoint("gemenon").toString())
    }

    @Test
    fun streamsAreWhereThePhoneReachedTheEngine() {
        // The engine names its own address -- behind a proxy, one the phone
        // cannot reach -- and http, where the proxy speaks TLS.
        val given = "http://10.10.10.200:6604/stream?path=x&token=t"
        assertEquals(
            "https://vps.example.org:6604/stream?path=x&token=t",
            Endpoint("vps.example.org", 6603, tls = true).streamUrl(given),
        )
        assertEquals(
            "http://vps.example.org:6604/stream?path=x&token=t",
            Endpoint("vps.example.org", 6603).streamUrl(given),
        )
        assertEquals("http://[fe80::1]:6604/stream?q", Endpoint("fe80::1").streamUrl(6604, "q"))
        // Not a stream of the engine's: left as it is.
        assertEquals("http://x/", Endpoint("vps.example.org", tls = true).streamUrl("http://x/"))
    }

    @Test
    fun aTrustedCertificateForTheHostConnects() = withServer { port ->
        val socket = Endpoint("127.0.0.1", port, tls = true)
            .openSocket(5_000, context(withKeys = false, trusting = true).socketFactory)
        socket.use {
            assertTrue(it is SSLSocket)
            it.getOutputStream().write("hello\n".toByteArray())
            it.getOutputStream().flush()
            assertEquals("heard hello", BufferedReader(InputStreamReader(it.getInputStream())).readLine())
        }
    }

    @Test
    fun aCertificateForAnotherNameIsRefused() = withServer(otherKeys) { port ->
        // Trusted, but for 127.0.0.9: not the address the engine was
        // reached at.
        try {
            Endpoint("127.0.0.1", port, tls = true)
                .openSocket(5_000, context(withKeys = false, trusting = true, keys = otherKeys).socketFactory)
                .close()
            fail("connected to a host the certificate does not name")
        } catch (_: SSLException) {
        }
    }

    @Test
    fun anUntrustedCertificateIsRefused() = withServer { port ->
        try {
            Endpoint("127.0.0.1", port, tls = true).openSocket(5_000).close()
            fail("connected with a certificate nobody vouches for")
        } catch (_: SSLException) {
        }
    }
}
