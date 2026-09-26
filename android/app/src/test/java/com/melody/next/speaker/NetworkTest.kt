package com.melody.next.speaker

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** Whether the phone asks for Opus: the networks as the phone reported them. */
class NetworkTest {
    private val homeWifi = Link(vpn = false, notMetered = true, wifiOrEthernet = true)
    private val mobile = Link(vpn = false, notMetered = false, cellular = true)

    @Test
    fun plainNetworksAreTakenAtTheirWord() {
        assertFalse(meteredOver(homeWifi, listOf(homeWifi)))
        assertTrue(meteredOver(mobile, listOf(mobile)))
        assertFalse(meteredOver(null, emptyList()))
    }

    // WireGuard over mobile data calls its tunnel unmetered, and the phone
    // streamed original FLAC outside. Its transports still say cellular.
    @Test
    fun aVpnOverMobileDataIsMeteredWhateverItSays() {
        val tunnel = Link(vpn = true, notMetered = true, cellular = true)
        assertTrue(meteredOver(tunnel, listOf(tunnel, mobile)))
    }

    // At home the same tunnel runs over Wi-Fi: originals, as before.
    @Test
    fun aVpnOverWifiIsNot() {
        val tunnel = Link(vpn = true, notMetered = true, wifiOrEthernet = true)
        assertFalse(meteredOver(tunnel, listOf(tunnel, homeWifi, mobile)))
    }

    // A VPN that reports no transport beneath it: unmetered only with a Wi-Fi or
    // Ethernet network there to carry it -- mobile data kept up beside it
    // does not count against it.
    @Test
    fun otherwiseAnUnmeteredWifiMustBeThere() {
        val tunnel = Link(vpn = true, notMetered = true)
        assertFalse(meteredOver(tunnel, listOf(tunnel, homeWifi, mobile)))
        assertTrue(meteredOver(tunnel, listOf(tunnel, mobile)))
        val unvalidated = homeWifi.copy(validated = false)
        assertTrue(meteredOver(tunnel, listOf(tunnel, unvalidated, mobile)))
    }
}
