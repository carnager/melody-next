package com.melody.next.speaker

import org.json.JSONObject

/**
 * One phone's player, several engines playing on it (ADR-0234): each
 * engine's agent gets a seat, and the seats share the player the way the
 * desktop's arbiter shares speakers -- the newest to start playing gets it.
 * The engine it was taken from is left paused where it was, and playing
 * there again takes it back, at that place, as the same playback.
 *
 * Every call comes on the player's thread, as the agents make them.
 */
class SharedSpeaker(private val audition: Audition) {
    private val seats = mutableListOf<Seat>()
    private var holder: Seat? = null

    /** A seat for one engine; `onChange` hears of every change it may report. */
    fun seat(onChange: () -> Unit): Audition = Seat(onChange).also { seats += it }

    /** The player changed: every seat may have something new to say. */
    fun changed() = seats.forEach { it.onChange() }

    /** A seat let go of: what it held, if it held the player, stops. */
    fun close(seat: Audition) {
        if (holder === seat) {
            audition.stop()
            holder = null
        }
        seats.remove(seat)
    }

    private inner class Seat(val onChange: () -> Unit) : Audition {
        // What the engine set, kept by the seat: the player has them only
        // while it holds it.
        private var volumePercent = 100
        private val replayGain = JSONObject().put("mode", 0).put("preamp_with_gain_db", 0.0).put("preamp_without_gain_db", 0.0)
        private var bufferCapacityMs = 750L
        private var bufferStartMs = 100L
        /** Its track while another holds the player: paused, where it was left. */
        private var place: Place? = null

        private val holds get() = holder === this

        /** The player, for this seat's engine: whatever held it before is left paused. */
        private fun claim() {
            if (holds) return
            holder?.let { it.place = audition.leave() }
            holder = this
            place = null
            audition.setVolume(volumePercent)
            audition.setReplayGain(replayGain)
            audition.setBuffer(bufferCapacityMs, bufferStartMs)
            changed()
        }

        override fun load(source: JSONObject, play: Boolean, positionMs: Long) {
            // Loaded paused -- a connection taking up where it was -- does
            // not take the player from music playing on it.
            if (!holds && !play && holder?.playing == true) {
                val before = place
                place = Place(
                    source = source,
                    positionMs = positionMs.coerceAtLeast(0),
                    playbackInstance = (before?.playbackInstance ?: 0) + 1,
                    chainTransitions = before?.chainTransitions ?: 0,
                )
                onChange()
                return
            }
            claim()
            audition.load(source, play, positionMs)
        }

        override fun queueNext(source: JSONObject, token: Long) {
            if (holds) return audition.queueNext(source, token)
            val kept = place ?: throw AgentError("unsupported", "nothing plays to continue from")
            place = kept.copy(next = source, nextToken = token)
        }

        override fun clearNext() {
            if (holds) return audition.clearNext()
            place = place?.copy(next = null, nextToken = 0)
        }

        override fun play() {
            if (holds) return audition.play()
            val kept = place ?: throw AgentError("unsupported", "nothing is loaded to play")
            claim()
            audition.takeUp(kept, play = true)
        }

        override fun pause() {
            // Left aside, it is paused already.
            if (holds) audition.pause()
        }

        override fun stop() {
            if (holds) audition.stop() else place = null
            onChange()
        }

        override fun seek(seconds: Double) {
            if (holds) return audition.seek(seconds)
            place = place?.copy(positionMs = (seconds * 1000).toLong().coerceAtLeast(0))
            onChange()
        }

        override fun setVolume(percent: Int) {
            volumePercent = percent.coerceIn(0, 100)
            if (holds) audition.setVolume(percent)
        }

        override fun setReplayGain(params: JSONObject) {
            if (params.has("mode") && params.optInt("mode", -1) !in PhoneAudition.Gain.entries.indices) {
                throw AgentError("invalid_argument", "mode is off, track or album")
            }
            for (key in listOf("mode", "preamp_with_gain_db", "preamp_without_gain_db")) {
                if (params.has(key)) replayGain.put(key, params.get(key))
            }
            if (holds) audition.setReplayGain(params)
        }

        override fun setBuffer(capacityMs: Long, startMs: Long) {
            bufferCapacityMs = capacityMs
            bufferStartMs = startMs
            if (holds) audition.setBuffer(capacityMs, startMs)
        }

        override val playing: Boolean get() = holds && audition.playing
        override val loaded: Boolean get() = if (holds) audition.loaded else place != null

        override fun snapshot(): JSONObject {
            if (holds) return audition.snapshot()
            val kept = place
            return Report(
                state = if (kept == null) PhoneAudition.State.Empty else PhoneAudition.State.Paused,
                positionMs = kept?.positionMs ?: 0,
                durationMs = kept?.durationMs,
                nextArmed = kept?.next != null,
                chainTransitions = kept?.chainTransitions ?: 0,
                playbackInstance = kept?.playbackInstance ?: 0,
                occurrenceToken = kept?.occurrenceToken ?: 0,
                nextToken = kept?.nextToken ?: 0,
                volumePercent = volumePercent,
                gainMode = replayGain.optInt("mode", 0),
                preampWithGain = replayGain.optDouble("preamp_with_gain_db", 0.0),
                preampWithoutGain = replayGain.optDouble("preamp_without_gain_db", 0.0),
                bufferCapacityMs = bufferCapacityMs,
                bufferStartMs = bufferStartMs,
            ).json()
        }

        override fun leave(): Place? = if (holds) audition.leave().also { holder = null } else place.also { place = null }

        override fun takeUp(place: Place, play: Boolean) {
            claim()
            audition.takeUp(place, play)
        }
    }
}
