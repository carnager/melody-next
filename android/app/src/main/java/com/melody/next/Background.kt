// SPDX-License-Identifier: GPL-3.0-only
package com.melody.next

import android.annotation.SuppressLint
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.PowerManager
import android.provider.Settings as SystemSettings

/**
 * Whether Android lets Melody run in the background as it needs to -- its
 * battery use "Unrestricted". Optimised, the system stops a speaker's
 * connection and stream when the screen is off, most of all on mobile data
 * behind a VPN, and the engine pauses each time (ADR-0274).
 */
object Background {
    fun unrestricted(context: Context): Boolean =
        context.getSystemService(PowerManager::class.java)
            ?.isIgnoringBatteryOptimizations(context.packageName) ?: true

    /**
     * Asks for it: the system's own question where it offers one, else the
     * battery settings, else the app's page.
     */
    @SuppressLint("BatteryLife") // A speaker that streams in the background is what the exemption is for.
    fun ask(context: Context) {
        val tries = listOf(
            Intent(SystemSettings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:${context.packageName}")),
            Intent(SystemSettings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS),
            Intent(SystemSettings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:${context.packageName}")),
        )
        for (intent in tries) {
            val opened = runCatching {
                context.startActivity(intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
            }.isSuccess
            if (opened) return
        }
    }
}
