// SPDX-License-Identifier: GPL-3.0-only
package com.melody.next.engine

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** ADR-0233: the engine's lists, as list.all and list.get describe them. */
class ListModelsTest {
    @Test
    fun aListSaysWhetherItIsSavedAndHowLong() {
        val saved = EngineList.from(
            JSONObject("""{"id":"a","name":"Road trip","kind":"saved","revision":3,"tracks":12,"modified_ms":1}"""),
        )
        assertEquals("Road trip", saved.name)
        assertTrue(saved.saved)
        assertEquals(12, saved.tracks)
        assertEquals(3L, saved.revision)
        assertFalse(EngineList.from(JSONObject("""{"id":"b","name":"Untitled","kind":"working"}""")).saved)
    }

    @Test
    fun anEntryKeepsItsIdentityAndWhatTheListSays() {
        val entry = ListEntry.from(
            JSONObject(
                """{"entry":"e1","path":"L211c2ljL2EuZmxhYw==","title":"A","artist":"Someone",""" +
                    """"album":"Album","duration_ms":1000}""",
            ),
        )
        assertEquals("e1", entry.entry)
        assertEquals("L211c2ljL2EuZmxhYw==", entry.path)
        assertEquals("A", entry.title)
        assertEquals("Someone", entry.artist)
        assertEquals(1000L, entry.durationMs)
        val unknown = ListEntry.from(JSONObject("""{"entry":"e2","path":"x","title":"B","duration_ms":null}"""))
        assertEquals(-1L, unknown.durationMs)
    }
}
