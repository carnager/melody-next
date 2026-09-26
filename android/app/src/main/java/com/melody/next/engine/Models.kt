package com.melody.next.engine

import org.json.JSONArray
import org.json.JSONObject
import java.util.Base64
import java.util.UUID

/**
 * Paths and keys are bytes, so they travel base64 (ADR-0222) and stay that
 * way here: the phone never opens a file, it only hands a path back. Decoded
 * only to be shown.
 */
object RawPath {
    fun encode(bytes: ByteArray): String = Base64.getEncoder().encodeToString(bytes)
    fun decode(encoded: String): ByteArray = runCatching { Base64.getDecoder().decode(encoded) }
        .getOrDefault(ByteArray(0))

    /** The last part of a path, for a file with no title. */
    fun fileName(encoded: String): String =
        String(decode(encoded), Charsets.UTF_8).substringAfterLast('/')
}

internal fun JSONObject.text(name: String): String =
    if (isNull(name)) "" else optString(name, "")

internal fun JSONArray?.objects(): List<JSONObject> =
    if (this == null) emptyList() else (0 until length()).mapNotNull { optJSONObject(it) }

data class Modes(
    val repeat: Boolean = false,
    val random: Boolean = false,
    val albumRandom: Boolean = false,
    /** 0 off, 1 on, 2 once -- the engine's one-shot states. */
    val single: Int = 0,
    val consume: Int = 0,
) {
    companion object {
        fun from(json: JSONObject?) = if (json == null) Modes() else Modes(
            repeat = json.optBoolean("repeat"),
            random = json.optBoolean("random"),
            albumRandom = json.optBoolean("album_random"),
            single = json.optInt("single"),
            consume = json.optInt("consume"),
        )
    }
}

/** What the engine is doing: `playback.state` and `playback.changed`. */
data class PlaybackState(
    val status: String = "stopped",
    val entry: String = "",
    val path: String = "",
    val positionMs: Long = 0,
    val durationMs: Long = -1,
    val queueSize: Int = 0,
    val requests: Int = 0,
    val modes: Modes = Modes(),
    val volume: Int = 100,
    val queueRevision: Long = 0,
    val replayGain: String = "off",
    /** Why playing failed, when it did. */
    val error: String = "",
    /** Another engine playing on these speakers, by name. */
    val speakersTakenBy: String = "",
    /** When this was heard, on the monotonic clock: the base for the clock. */
    val receivedAtMs: Long = 0,
) {
    val playing get() = status == "playing"

    /** Where playback is now, counted on from when this was heard. */
    fun positionAt(nowMs: Long): Long {
        if (!playing) return positionMs
        val moved = positionMs + (nowMs - receivedAtMs).coerceAtLeast(0)
        return if (durationMs > 0) moved.coerceAtMost(durationMs) else moved
    }

    companion object {
        fun from(json: JSONObject, receivedAtMs: Long) = PlaybackState(
            status = json.optString("status", "stopped"),
            entry = json.text("entry"),
            path = json.text("path"),
            positionMs = json.optLong("position_ms"),
            durationMs = json.optLong("duration_ms", -1),
            queueSize = json.optInt("queue_size"),
            requests = json.optInt("requests"),
            modes = Modes.from(json.optJSONObject("modes")),
            volume = json.optInt("volume_percent", 100),
            queueRevision = json.optLong("queue_revision"),
            replayGain = json.optJSONObject("replay_gain")?.optString("mode", "off") ?: "off",
            error = json.text("error"),
            speakersTakenBy = json.optJSONObject("output")?.text("taken_by") ?: "",
            receivedAtMs = receivedAtMs,
        )
    }
}

/**
 * An entry in the engine's queue or Up Next. The engine's own JSON is kept
 * whole and sent back as it came, so an edit made here keeps what this
 * client does not show -- a CUE segment, a stream, a gain.
 */
data class QueueEntry(val json: JSONObject) {
    val entry: String get() = json.text("entry")
    val path: String get() = json.text("path")
    val durationMs: Long get() = json.optLong("duration_ms", -1)
    private val group get() = json.optJSONObject("group") ?: JSONObject()
    val title: String get() = json.text("title").ifEmpty { RawPath.fileName(path) }
    val artist: String get() = group.text("artist")
    val albumArtist: String get() = group.text("album_artist").ifEmpty { artist }
    val album: String get() = group.text("album")
    val date: String get() = group.text("date")

    /** Tracks of one release sit together; this says which release. */
    val albumGroup: String get() = "$albumArtist\u0000$album\u0000$date"
}

enum class EntryKind(val wire: Int) { Artist(0), Album(1), Track(2) }

/** A row of the library: an artist, an album or a track (`catalogue.query`). */
data class LibraryEntry(
    val kind: EntryKind,
    /** An artist's name, an album's key, a track's path -- encoded. */
    val key: String,
    val label: String,
    val artist: String,
    val album: String,
    val title: String,
    val date: String,
    val tracks: Int,
    val albums: Int,
    val trackNumber: Int,
    val ratingHash: String,
    /** 0-10, half stars. */
    val rating: Int,
    val added: Long,
    val durationMs: Long,
) {
    val year: String get() = date.take(4)

    fun toJson(): JSONObject = JSONObject()
        .put("kind", kind.wire).put("key", key).put("label", label).put("artist", artist)
        .put("album", album).put("title", title).put("date", date).put("tracks", tracks)
        .put("albums", albums).put("track_number", trackNumber).put("rating_hash", ratingHash)
        .put("rating", rating).put("added", added).put("duration_ms", durationMs)

    companion object {
        fun from(json: JSONObject) = LibraryEntry(
            kind = EntryKind.entries.firstOrNull { it.wire == json.optInt("kind") } ?: EntryKind.Track,
            key = json.text("key"),
            label = json.text("label"),
            artist = json.text("artist"),
            album = json.text("album"),
            title = json.text("title"),
            date = json.text("date"),
            tracks = json.optInt("tracks"),
            albums = json.optInt("albums"),
            trackNumber = json.optInt("track_number"),
            ratingHash = json.text("rating_hash"),
            rating = json.optInt("rating"),
            added = json.optLong("added"),
            durationMs = json.optLong("duration_ms", -1),
        )
    }
}

/**
 * A library track as a queue entry: its path, a fresh identity and what the
 * engine shows and groups by. `albumArtist` is the album's, when the track
 * came from one.
 */
fun LibraryEntry.toQueueEntry(albumArtist: String = artist): QueueEntry {
    val json = JSONObject()
        .put("entry", UUID.randomUUID().toString())
        .put("path", key)
        .put("title", title.ifEmpty { label })
        .put(
            "group",
            JSONObject()
                .put("album_artist", albumArtist)
                .put("artist", artist)
                .put("album", album)
                .put("date", date),
        )
    if (durationMs >= 0) json.put("duration_ms", durationMs)
    return QueueEntry(json)
}

/** Somewhere the engine can play: its own speakers, or an agent's. */
data class Output(
    val id: String,
    val name: String,
    val local: Boolean,
    val online: Boolean,
    val selected: Boolean,
) {
    companion object {
        fun from(json: JSONObject) = Output(
            id = json.text("id"),
            name = json.text("name"),
            local = json.optBoolean("local"),
            online = json.optBoolean("online", true),
            selected = json.optBoolean("selected"),
        )
    }
}

/** A track's or an album's rating, 0-10, by the key the library rates it under. */
data class RatingChange(val hash: String, val rating: Int)

/** ADR-0233: one of the engine's lists -- saved, or a working one a window has open. */
data class EngineList(
    val id: String,
    val name: String,
    val saved: Boolean,
    val tracks: Int,
    val revision: Long,
) {
    companion object {
        fun from(json: JSONObject) = EngineList(
            id = json.optString("id"),
            name = json.optString("name"),
            saved = json.optString("kind") == "saved",
            tracks = json.optInt("tracks"),
            revision = json.optLong("revision"),
        )
    }
}

/** An entry of a list: its identity in the list, and what the list says it is. */
data class ListEntry(
    val entry: String,
    /** The track's path, encoded as the engine sends it. */
    val path: String,
    val title: String,
    val artist: String,
    val album: String,
    val durationMs: Long,
) {
    companion object {
        fun from(json: JSONObject): ListEntry {
            val path = json.optString("path")
            // A file the library does not describe is named by its file name.
            val named = json.optString("title").ifEmpty {
                runCatching {
                    String(android.util.Base64.decode(path, android.util.Base64.DEFAULT), Charsets.UTF_8)
                        .substringAfterLast('/')
                }.getOrDefault("")
            }
            return ListEntry(
                entry = json.optString("entry"),
                path = path,
                title = named,
                artist = json.optString("artist"),
                album = json.optString("album"),
                durationMs = if (json.isNull("duration_ms")) -1 else json.optLong("duration_ms", -1),
            )
        }
    }
}
