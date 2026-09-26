package com.melody.next.speaker

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** The one player, as a phone has it: one track at a time, and where it is. */
private class OnePlayer : Audition {
    var source: JSONObject? = null
    var next: JSONObject? = null
    var state = 0
    var position = 0L
    var instance = 0L
    var level = 100
    val asked = mutableListOf<String>()
    override fun load(source: JSONObject, play: Boolean, positionMs: Long) {
        this.source = source; next = null; position = positionMs; instance++
        state = if (play) 4 else 5
        asked += "load ${source.getString("url")}"
    }
    override fun queueNext(source: JSONObject, token: Long) { next = source }
    override fun clearNext() { next = null }
    override fun play() { state = 4 }
    override fun pause() { state = 5 }
    override fun stop() { source = null; next = null; state = 0 }
    override fun seek(seconds: Double) { position = (seconds * 1000).toLong() }
    override fun setVolume(percent: Int) { level = percent }
    override fun setReplayGain(params: JSONObject) {}
    override fun setBuffer(capacityMs: Long, startMs: Long) {}
    override val playing get() = state == 4
    override val loaded get() = state != 0
    override fun snapshot(): JSONObject = JSONObject().put("state", state).put("position_sample", position).put("playback_instance", instance)
    override fun leave(): Place? {
        val kept = source?.let { Place(it, next = next, positionMs = position, playbackInstance = instance) }
        stop()
        return kept
    }
    override fun takeUp(place: Place, play: Boolean) {
        load(place.source, play, place.positionMs)
        instance = place.playbackInstance
        next = place.next
        asked += "take_up ${place.source.getString("url")}"
    }
}

private fun track(url: String) = JSONObject().put("url", url)

class SharedSpeakerTest {
    private val player = OnePlayer()
    private val speaker = SharedSpeaker(player)
    private val gemenon = speaker.seat {}
    private val caprica = speaker.seat {}

    @Test
    fun theNewestToPlayTakesThePlayerAndTheOtherIsLeftPaused() {
        gemenon.load(track("g1"), play = true, positionMs = 0)
        player.position = 42_000
        caprica.load(track("c1"), play = true, positionMs = 0)

        assertEquals("c1", player.source!!.getString("url"))
        assertTrue(caprica.playing)
        assertFalse(gemenon.playing)
        // Gemenon hears it paused, where it was, as the same playback.
        val left = gemenon.snapshot()
        assertEquals(PhoneAudition.State.Paused.ordinal, left.getInt("state"))
        assertEquals(42_000L, left.getLong("position_sample"))
        assertEquals(1L, left.getLong("playback_instance"))
    }

    @Test
    fun playingAgainTakesItBackWhereItWas() {
        gemenon.load(track("g1"), play = true, positionMs = 0)
        gemenon.queueNext(track("g2"), token = 3)
        player.position = 42_000
        caprica.load(track("c1"), play = true, positionMs = 0)
        player.position = 5_000

        gemenon.play()

        assertEquals("g1", player.source!!.getString("url"))
        assertEquals(42_000L, player.position)
        assertEquals(1L, player.instance)
        assertEquals("g2", player.next!!.getString("url"))
        assertTrue(gemenon.playing)
        // And caprica, in turn, is left paused at its place.
        assertEquals(PhoneAudition.State.Paused.ordinal, caprica.snapshot().getInt("state"))
        assertEquals(5_000L, caprica.snapshot().getLong("position_sample"))
    }

    @Test
    fun aSeatAsideDoesNotTouchWhatPlays() {
        gemenon.load(track("g1"), play = true, positionMs = 0)
        caprica.load(track("c1"), play = true, positionMs = 0)
        gemenon.pause()
        gemenon.seek(10.0)
        gemenon.setVolume(30)

        assertTrue(player.playing)
        assertEquals("c1", player.source!!.getString("url"))
        assertEquals(100, player.level)
        assertEquals(10_000L, gemenon.snapshot().getLong("position_sample"))
        assertEquals(30, gemenon.snapshot().getInt("volume_percent"))

        // Its volume comes with it when it takes the player back.
        gemenon.play()
        assertEquals(30, player.level)
        assertEquals(10_000L, player.position)
    }

    @Test
    fun loadedPausedItDoesNotTakeThePlayerFromMusicPlaying() {
        gemenon.load(track("g1"), play = true, positionMs = 0)
        // Caprica connecting, taking up where it was.
        caprica.load(track("c1"), play = false, positionMs = 7_000)

        assertEquals("g1", player.source!!.getString("url"))
        assertTrue(player.playing)
        assertEquals(PhoneAudition.State.Paused.ordinal, caprica.snapshot().getInt("state"))
        assertEquals(7_000L, caprica.snapshot().getLong("position_sample"))

        caprica.play()
        assertEquals("c1", player.source!!.getString("url"))
        assertEquals(7_000L, player.position)
    }

    @Test
    fun stoppedAsideAndClosedSeats() {
        gemenon.load(track("g1"), play = true, positionMs = 0)
        caprica.load(track("c1"), play = true, positionMs = 0)
        gemenon.stop()
        assertEquals(PhoneAudition.State.Empty.ordinal, gemenon.snapshot().getInt("state"))
        assertTrue(player.playing)
        assertTrue(runCatching { gemenon.play() }.exceptionOrNull() is AgentError)

        // The engine holding the player let go of: what it played stops.
        speaker.close(caprica)
        assertFalse(player.loaded)
    }
}
