// SPDX-License-Identifier: GPL-3.0-only
package com.melody.next.speaker

import com.melody.next.engine.Endpoint
import com.melody.next.engine.Transport
import org.junit.Assert.assertEquals
import org.junit.Test

class OncePerEngineTest {
    private val proxy = Endpoint("music.example", 443, "pw", Transport.WSS)
    private val home = Endpoint("192.168.0.200", 6603, "pw")
    private val other = Endpoint("192.168.0.13", 6603, "pw")

    @Test
    fun oneEngineUnderTwoAddressesIsOfferedTheSpeakerOnce() {
        val ids = mapOf(proxy to "gemenon", home to "gemenon", other to "caprica")
        assertEquals(listOf(proxy, other), oncePerEngine(listOf(proxy, home, other)) { ids[it] })
    }

    @Test
    fun anEngineNotYetKnownIsKeptAndOneAddressCountsOnce() {
        assertEquals(listOf(proxy, home), oncePerEngine(listOf(proxy, home, home.copy(password = "x"))) { null })
    }
}
