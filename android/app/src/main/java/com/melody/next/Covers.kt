package com.melody.next

import android.content.Context
import android.graphics.BitmapFactory
import android.util.LruCache
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import com.melody.next.engine.EngineClient
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.withContext
import java.io.File
import java.security.MessageDigest

/** Which cover: an album by its key, or the album a file belongs to. */
data class CoverKey(val albumKey: String? = null, val path: String? = null, val group: String = "") {
    /** One album, one cover, however it was asked for. */
    val identity: String get() = albumKey ?: group.ifEmpty { path ?: "" }
}

/**
 * Covers, scaled by the engine and kept here: in memory for what is on
 * screen, on disk so scrolling back past an album -- or opening the app
 * tomorrow -- costs nothing on the network. Kept per engine, since the same
 * key on another engine is another album.
 */
class Covers(context: Context, private val client: EngineClient) {
    private val memory = object : LruCache<String, ImageBitmap>(48 * 1024 * 1024) {
        override fun sizeOf(key: String, value: ImageBitmap) = value.width * value.height * 4
    }
    private val directory = File(context.cacheDir, "covers").apply { mkdirs() }
    private val missing = HashSet<String>()
    // A grid asks for a screenful at once; the engine answers one at a time
    // per connection anyway, so a few in flight is plenty.
    private val fetching = Semaphore(4)
    var engine: String = ""

    fun cached(key: CoverKey, size: Int): ImageBitmap? = memory.get(name(key, size))

    suspend fun load(key: CoverKey, size: Int): ImageBitmap? {
        val name = name(key, size)
        memory.get(name)?.let { return it }
        if (synchronized(missing) { name in missing }) return null
        return withContext(Dispatchers.IO) {
            val file = File(directory, digest(name))
            val bytes = if (file.isFile) {
                file.readBytes()
            } else {
                // Not reachable is not "no cover": only the engine's answer
                // that there is none is remembered.
                val fetched = try {
                    fetching.withPermit { client.cover(key.albumKey, key.path, size) }
                } catch (failure: Exception) {
                    if (failure is kotlinx.coroutines.CancellationException) throw failure
                    return@withContext null
                }
                if (fetched == null) {
                    synchronized(missing) { missing += name }
                    return@withContext null
                }
                runCatching { file.writeBytes(fetched) }
                fetched
            }
            val bitmap = BitmapFactory.decodeByteArray(bytes, 0, bytes.size)?.asImageBitmap()
            bitmap?.also { memory.put(name, it) }
        }
    }

    /** The encoded cover, for what wants bytes -- the media notification. */
    suspend fun bytes(key: CoverKey, size: Int): ByteArray? {
        if (load(key, size) == null) return null
        return withContext(Dispatchers.IO) {
            File(directory, digest(name(key, size))).takeIf { it.isFile }?.readBytes()
        }
    }

    /** Covers may have changed: forget the ones that were not there. */
    fun forgetMissing() = synchronized(missing) { missing.clear() }

    private fun name(key: CoverKey, size: Int) = "$engine\u0000${key.identity}\u0000$size"

    private fun digest(text: String): String =
        MessageDigest.getInstance("SHA-1").digest(text.toByteArray()).joinToString("") { "%02x".format(it) }
}
