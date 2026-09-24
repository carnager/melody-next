package com.melody.next

import android.content.Context
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.melody.next.engine.Endpoint

/** What this phone remembers: the engine it talks to and how it looks. */
class Settings(context: Context) {
    private val preferences = context.getSharedPreferences("melody", Context.MODE_PRIVATE)

    var endpoint: Endpoint?
        get() {
            val host = preferences.getString("host", null) ?: return null
            return Endpoint(host, preferences.getInt("port", Endpoint.DEFAULT_PORT), preferences.getString("password", "") ?: "")
        }
        set(value) {
            preferences.edit().apply {
                if (value == null) {
                    remove("host"); remove("port"); remove("password")
                } else {
                    putString("host", value.host)
                    putInt("port", value.port)
                    putString("password", value.password)
                }
            }.apply()
        }

    /** Whether this phone offers itself to the engine as somewhere to play. */
    var speaker by mutableStateOf(preferences.getBoolean("speaker", true))
        private set

    /** What the engine and the other clients call this phone. */
    var speakerName by mutableStateOf(preferences.getString("speaker_name", null) ?: android.os.Build.MODEL)
        private set

    fun updateSpeaker(on: Boolean) {
        speaker = on
        preferences.edit().putBoolean("speaker", on).apply()
    }

    fun updateSpeakerName(name: String) {
        speakerName = name.trim().ifEmpty { android.os.Build.MODEL }
        preferences.edit().putString("speaker_name", speakerName).apply()
    }

    /** Opus bit rate on mobile data, in kbps. */
    var mobileBitrate by mutableStateOf(preferences.getInt("mobile_bitrate", 128))
        private set

    /** Opus bit rate on Wi-Fi in kbps, or 0 for the original files. */
    var wifiBitrate by mutableStateOf(preferences.getInt("wifi_bitrate", 0))
        private set

    fun updateMobileBitrate(kbps: Int) {
        mobileBitrate = kbps
        preferences.edit().putInt("mobile_bitrate", kbps).apply()
    }

    fun updateWifiBitrate(kbps: Int) {
        wifiBitrate = kbps
        preferences.edit().putInt("wifi_bitrate", kbps).apply()
    }

    /** "system", "dark" or "light". */
    var theme by mutableStateOf(preferences.getString("theme", "system") ?: "system")
        private set

    var dynamicColor by mutableStateOf(preferences.getBoolean("dynamic_color", false))
        private set

    fun updateTheme(mode: String) {
        theme = mode
        preferences.edit().putString("theme", mode).apply()
    }

    fun updateDynamicColor(on: Boolean) {
        dynamicColor = on
        preferences.edit().putBoolean("dynamic_color", on).apply()
    }
}
