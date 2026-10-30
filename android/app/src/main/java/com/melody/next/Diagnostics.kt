// SPDX-License-Identifier: GPL-3.0-only
package com.melody.next

import android.content.Context
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * What happened to the app's connections and its playing, kept on the phone:
 * the system's own log reaches back a minute, and a drop out of doors is
 * seen hours later. Shared from Settings, to be read by whoever looks into
 * it. Small, and the oldest let go.
 */
object Diagnostics {
    private const val KEPT_BYTES = 512 * 1024
    private val time = SimpleDateFormat("MM-dd HH:mm:ss.SSS", Locale.ROOT)
    private val lock = Any()
    @Volatile private var file: File? = null

    fun start(context: Context) {
        file = File(context.filesDir, "diagnostics.log")
    }

    fun note(what: String) {
        // In the system's log too, to be followed over adb as it happens.
        android.util.Log.i("Melody", what)
        val target = file ?: return
        synchronized(lock) {
            runCatching {
                target.appendText("${time.format(Date())} $what\n")
                if (target.length() > KEPT_BYTES) {
                    val text = target.readText()
                    target.writeText(text.substring(text.length - KEPT_BYTES / 2).substringAfter('\n'))
                }
            }
        }
    }

    fun read(): String = synchronized(lock) { runCatching { file?.readText() }.getOrNull() ?: "" }

    /**
     * The whole log as a file to share, in the folder the app's FileProvider
     * hands out: shared as text it was too big -- Android refuses an
     * intent over about a megabyte, and the chooser never opened.
     */
    fun export(context: Context): File? = synchronized(lock) {
        runCatching {
            val folder = File(context.cacheDir, "shared").apply { mkdirs() }
            File(folder, "melody-connection-log.txt").apply { writeText(file?.readText() ?: "") }
        }.getOrNull()
    }

    fun clear() = synchronized(lock) { runCatching { file?.writeText("") } }
}
