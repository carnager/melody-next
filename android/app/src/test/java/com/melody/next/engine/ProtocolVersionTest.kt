// SPDX-License-Identifier: GPL-3.0-only
package com.melody.next.engine

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** ADR-0260: the app decides once, from engine.info, whether and how it can work with an engine. */
class ProtocolVersionTest {
    private fun info(protocol: Int?, level: Int?) = JSONObject().apply {
        protocol?.let { put("protocol", it) }
        level?.let { put("level", it) }
    }

    @Test
    fun theSameOrANewerLevelSaysNothing() {
        assertEquals(ProtocolVersion.Verdict(false, ""), ProtocolVersion.of(info(1, ProtocolVersion.LEVEL), "gemenon"))
        assertEquals(ProtocolVersion.Verdict(false, ""), ProtocolVersion.of(info(1, ProtocolVersion.LEVEL + 1), "gemenon"))
    }

    @Test
    fun anOlderEngineIsUsedAndSaidToBeOlder() {
        val verdict = ProtocolVersion.of(info(1, null), "gemenon")
        assertFalse(verdict.incompatible)
        assertTrue(verdict.message.startsWith("gemenon's melodyd is older"))
    }

    @Test
    fun anotherProtocolIsNotUsedAndSaysWhatToUpdate() {
        val newer = ProtocolVersion.of(info(2, 0), "gemenon")
        assertTrue(newer.incompatible)
        assertEquals("gemenon speaks protocol 2; this app speaks protocol 1. Update the app.", newer.message)
        val older = ProtocolVersion.of(info(0, 0), "")
        assertTrue(older.incompatible)
        assertTrue(older.message.endsWith("Update The engine's melodyd."))
    }
}
