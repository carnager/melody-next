package com.melody.next

import org.junit.Assert.assertEquals
import org.junit.Test

class FollowingTest {
    private val playing = mutableSetOf<String>()
    private val stopped = mutableListOf<String>()
    private val following = Following("gemenon", { it in playing }, { stopped += it; playing -= it })

    @Test
    fun playingOnAnotherEngineFollowsItAndStopsTheOneBefore() {
        playing += "gemenon"
        following.playOn("caprica")
        assertEquals("caprica", following.followed.value)
        assertEquals(listOf("gemenon"), stopped)
    }

    @Test
    fun playingOnTheFollowedEngineStopsNothing() {
        playing += "gemenon"
        following.playOn("gemenon")
        assertEquals(emptyList<String>(), stopped)
    }

    @Test
    fun anEngineStartedElsewhereIsFollowedOnlyWhileTheFollowedOneIsIdle() {
        following.started("caprica")
        assertEquals("caprica", following.followed.value)
        playing += "caprica"
        following.started("gemenon")
        assertEquals("caprica", following.followed.value)
    }

    @Test
    fun anEngineLetGoOfHandsBackToTheMainOne() {
        following.playOn("caprica")
        following.gone("caprica", "gemenon")
        assertEquals("gemenon", following.followed.value)
        following.gone("caprica", "gemenon")
        assertEquals("gemenon", following.followed.value)
    }
}
