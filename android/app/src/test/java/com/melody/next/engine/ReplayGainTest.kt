// SPDX-License-Identifier: GPL-3.0-only
package com.melody.next.engine

import org.junit.Assert.assertEquals
import org.junit.Test

/** The phone's ReplayGain choice as the engine is told it, by Trackknife's rule. */
class ReplayGainTest {
    @Test
    fun automaticIsTrackGainWithRandomAndAlbumGainOtherwise() {
        assertEquals("track", resolveReplayGain("auto", random = true))
        assertEquals("album", resolveReplayGain("auto", random = false))
    }

    @Test
    fun theEnginesOwnModesAreToldAsTheyAre() {
        for (random in listOf(true, false)) {
            assertEquals("off", resolveReplayGain("off", random))
            assertEquals("track", resolveReplayGain("track", random))
            assertEquals("album", resolveReplayGain("album", random))
        }
        assertEquals("anything else is off", "off", resolveReplayGain("loud", false))
    }

    private fun state(random: Boolean, mode: String) = PlaybackState(modes = Modes(random = random), replayGain = mode)

    @Test
    fun automaticTellsTheEngineWhenChosenAndWhenRandomMoves() {
        val automatic = AutomaticReplayGain()
        // Chosen while the engine has it off: album gain, in order.
        assertEquals(AutomaticReplayGain.Step.Tell("album"), automatic.on(state(false, "off"), auto = true, connected = true))
        // Its own change, not back yet: no one else's choice.
        assertEquals(AutomaticReplayGain.Step.Nothing, automatic.on(state(false, "off"), auto = true, connected = true))
        assertEquals(AutomaticReplayGain.Step.Nothing, automatic.on(state(false, "album"), auto = true, connected = true))
        // Random on: track gain.
        assertEquals(AutomaticReplayGain.Step.Tell("track"), automatic.on(state(true, "album"), auto = true, connected = true))
        assertEquals(AutomaticReplayGain.Step.Nothing, automatic.on(state(true, "track"), auto = true, connected = true))
    }

    @Test
    fun aModeChosenElsewhereEndsAutomatic() {
        val automatic = AutomaticReplayGain()
        assertEquals(AutomaticReplayGain.Step.Nothing, automatic.on(state(false, "album"), auto = true, connected = true))
        // Trackknife set to Track, Random as it was.
        assertEquals(AutomaticReplayGain.Step.Chosen, automatic.on(state(false, "track"), auto = true, connected = true))
    }

    @Test
    fun offOrAwayItDoesNothingAndLooksAfreshAfter() {
        val automatic = AutomaticReplayGain()
        assertEquals(AutomaticReplayGain.Step.Nothing, automatic.on(state(true, "off"), auto = false, connected = true))
        assertEquals(AutomaticReplayGain.Step.Nothing, automatic.on(state(true, "off"), auto = true, connected = false))
        // Reached again: what it means is told, not taken for someone's choice.
        assertEquals(AutomaticReplayGain.Step.Tell("track"), automatic.on(state(true, "off"), auto = true, connected = true))
    }
}
