package com.melody.next.offline

import android.content.Context
import com.melody.next.Covers
import com.melody.next.CoverKey
import com.melody.next.engine.EngineClient
import com.melody.next.engine.LibraryEntry
import com.melody.next.engine.objects
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

/** A track kept on the phone: what the library said of it, and its file. */
data class OfflineTrack(val entry: LibraryEntry, val file: File)

/** An album kept on the phone, whole or on its way. */
data class OfflineAlbum(
    val engine: String,
    val album: LibraryEntry,
    val tracks: List<OfflineTrack>,
    val bitrateKbps: Int,
    val directory: File,
) {
    val complete: Boolean get() = tracks.all { it.file.isFile }
    val cover: File get() = File(directory, "cover.jpg")
}

data class Progress(val done: Int, val total: Int)

/**
 * Albums for listening without the engine: fetched with tickets the engine
 * signs (`streams.ticket`), as Opus at the chosen rate or as the files are,
 * each into its own folder with what the library said of it. What is on the
 * way survives the app going: an album not yet whole is taken up again.
 */
class OfflineStore(
    context: Context,
    private val scope: CoroutineScope,
    private val client: EngineClient,
    private val covers: Covers,
    private val wanted: () -> Wanted,
) {
    /** How, and when, to download: read afresh for each album. */
    data class Wanted(val bitrateKbps: Int, val wifiOnly: Boolean, val metered: StateFlow<Boolean>)

    private val root = File(context.filesDir, "offline").apply { mkdirs() }
    private val _albums = MutableStateFlow<List<OfflineAlbum>>(emptyList())
    private val _progress = MutableStateFlow<Map<String, Progress>>(emptyMap())
    private val waiting = ArrayDeque<String>()
    /** Tracks of an album fetched at once. */
    private val parallelFetches = 4
    private var worker: Job? = null

    val albums: StateFlow<List<OfflineAlbum>> = _albums
    /** By album key: how far each download is. */
    val progress: StateFlow<Map<String, Progress>> = _progress
    val problems = MutableStateFlow<String?>(null)

    init {
        scope.launch(Dispatchers.IO) {
            _albums.value = root.listFiles().orEmpty().mapNotNull(::read).sortedBy { it.album.artist.lowercase() + it.album.album.lowercase() }
            // Unfinished ones carry on where they stopped.
            _albums.value.filterNot { it.complete }.forEach { enqueue(it.album.key) }
        }
    }

    fun album(key: String): OfflineAlbum? = _albums.value.firstOrNull { it.album.key == key }

    /** The file for a track the phone has, whole: played instead of streamed. */
    fun fileFor(path: String): File? =
        _albums.value.firstNotNullOfOrNull { album -> album.tracks.firstOrNull { it.entry.key == path }?.file?.takeIf(File::isFile) }

    /** Keeps an album: its tracks as the library lists them, then fetched a few at a time. */
    fun download(engine: String, album: LibraryEntry) {
        scope.launch(Dispatchers.IO) {
            try {
                val tracks = client.tracksOf(album)
                val bitrate = wanted().bitrateKbps
                val directory = File(root, digest(engine + "\u0000" + album.key)).apply { mkdirs() }
                val kept = OfflineAlbum(engine, album, tracks.mapIndexed { index, track ->
                    OfflineTrack(track, File(directory, "%03d.%s".format(index + 1, if (bitrate > 0) "opus" else "audio")))
                }, bitrate, directory)
                write(kept)
                covers.bytes(CoverKey(albumKey = album.key), 512)?.let { kept.cover.writeBytes(it) }
                _albums.value = (_albums.value.filterNot { it.album.key == album.key } + kept)
                    .sortedBy { it.album.artist.lowercase() + it.album.album.lowercase() }
                enqueue(album.key)
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                problems.value = "Could not keep ${album.album}: ${failure.message}"
            }
        }
    }

    fun remove(key: String) {
        val album = album(key) ?: return
        synchronized(waiting) { waiting.remove(key) }
        _albums.value = _albums.value.filterNot { it.album.key == key }
        _progress.value = _progress.value - key
        scope.launch(Dispatchers.IO) { album.directory.deleteRecursively() }
    }

    private fun enqueue(key: String) {
        synchronized(waiting) {
            if (key !in waiting) waiting.addLast(key)
        }
        if (worker?.isActive != true) worker = scope.launch(Dispatchers.IO) { work() }
    }

    private suspend fun work() {
        while (true) {
            val key = synchronized(waiting) { waiting.removeFirstOrNull() } ?: return
            val album = album(key) ?: continue
            val want = wanted()
            if (want.wifiOnly) want.metered.first { !it }
            _progress.value = _progress.value + (key to Progress(album.tracks.count { it.file.isFile }, album.tracks.size))
            // Several at once: the engine converts each whole before its
            // first byte, so one at a time leaves it and the network idle
            // in turn.
            val slots = Semaphore(parallelFetches)
            coroutineScope {
                for (track in album.tracks) {
                    if (track.file.isFile) continue
                    launch {
                        slots.withPermit {
                            if (album(key) != null && keep(key, album, track)) {
                                _progress.update { it + (key to Progress(album.tracks.count { t -> t.file.isFile }, album.tracks.size)) }
                            }
                        }
                    }
                }
            }
            _progress.value = _progress.value - key
        }
    }

    /** Fetches one track, trying again while the album is still wanted. */
    private suspend fun keep(key: String, album: OfflineAlbum, track: OfflineTrack): Boolean {
        var tries = 0
        while (true) {
            try {
                fetch(track, album.bitrateKbps)
                return true
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                // Not connected yet, a moment's outage: again, a little later
                // each time.
                if (album(key) == null) return false
                if (++tries >= 5) {
                    problems.value = "${album.album.album}: ${failure.message}"
                    delay(60_000)
                    tries = 0
                } else {
                    delay(2_000L * tries)
                }
            }
        }
    }

    private suspend fun fetch(track: OfflineTrack, bitrateKbps: Int) {
        val params = JSONObject().put("path", track.entry.key)
        if (bitrateKbps > 0) params.put("format", "opus").put("bitrate", bitrateKbps)
        val ticket = client.call("streams.ticket", params)
        val host = client.engineHost() ?: throw IOException("not connected to an engine")
        val url = URL("http://${if (host.contains(':')) "[$host]" else host}:${ticket.getInt("port")}/stream?${ticket.getString("query")}")
        withContext(Dispatchers.IO) {
            val part = File(track.file.path + ".part")
            val connection = url.openConnection() as HttpURLConnection
            connection.connectTimeout = 10_000
            // Converting takes a moment before the first byte.
            connection.readTimeout = 120_000
            try {
                if (connection.responseCode != 200) throw IOException("the engine answered ${connection.responseCode}")
                connection.inputStream.use { input -> part.outputStream().use { input.copyTo(it) } }
                if (!part.renameTo(track.file)) throw IOException("could not keep ${track.file.name}")
            } finally {
                connection.disconnect()
                part.delete()
            }
        }
    }

    private fun write(album: OfflineAlbum) {
        val json = JSONObject()
            .put("engine", album.engine)
            .put("album", album.album.toJson())
            .put("bitrate", album.bitrateKbps)
            .put("tracks", JSONArray().apply {
                album.tracks.forEach { put(it.entry.toJson().put("file", it.file.name)) }
            })
        File(album.directory, "album.json").writeText(json.toString())
    }

    private fun read(directory: File): OfflineAlbum? = runCatching {
        val json = JSONObject(File(directory, "album.json").readText())
        OfflineAlbum(
            engine = json.getString("engine"),
            album = LibraryEntry.from(json.getJSONObject("album")),
            tracks = json.optJSONArray("tracks").objects().map { OfflineTrack(LibraryEntry.from(it), File(directory, it.getString("file"))) },
            bitrateKbps = json.optInt("bitrate"),
            directory = directory,
        )
    }.getOrNull()

    private fun digest(text: String): String =
        MessageDigest.getInstance("SHA-1").digest(text.toByteArray()).joinToString("") { "%02x".format(it) }
}
