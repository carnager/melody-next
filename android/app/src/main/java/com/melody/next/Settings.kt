package com.melody.next

import android.content.Context
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.melody.next.engine.Endpoint
import com.melody.next.engine.Transport
import com.melody.next.engine.sameAddress

/** What this phone remembers: the engine it talks to and how it looks. */
class Settings(context: Context) {
    private val preferences = context.getSharedPreferences("melody", Context.MODE_PRIVATE)

    var endpoint: Endpoint?
        get() {
            val host = preferences.getString("host", null) ?: return null
            return Endpoint(
                host, preferences.getInt("port", Endpoint.DEFAULT_PORT), preferences.getString("password", "") ?: "",
                transportOf(preferences.getString("transport", null)),
                preferences.getString("path", null) ?: Endpoint.DEFAULT_PATH,
            )
        }
        set(value) {
            val before = endpoint
            preferences.edit().apply {
                // Another engine: the id known was the last one's.
                if (value == null || before == null || !before.sameAddress(value)) remove("engine_id")
                if (value == null) {
                    remove("host"); remove("port"); remove("password"); remove("transport"); remove("path")
                } else {
                    putString("host", value.host)
                    putInt("port", value.port)
                    putString("password", value.password)
                    putString("transport", value.transport.name)
                    putString("path", value.path)
                }
            }.apply()
        }

    /**
     * ADR-0272: the id of the engine at [endpoint], as it gave it -- what it
     * is found by on this network, before the first connection is made.
     */
    var engineId: String?
        get() = preferences.getString("engine_id", null)
        set(value) = preferences.edit().putString("engine_id", value).apply()

    /**
     * ADR-0234: engines besides the one this phone plays through, whose lists
     * it shows too -- a desktop's open tabs beside the NAS's lists.
     */
    var otherEngines by mutableStateOf(readOtherEngines())
        private set

    fun updateOtherEngines(engines: List<Endpoint>) {
        otherEngines = engines.distinctBy { it.host to it.port }
        val stored = org.json.JSONArray()
        otherEngines.forEach {
            stored.put(
                org.json.JSONObject().put("host", it.host).put("port", it.port).put("password", it.password)
                    .put("transport", it.transport.name).put("path", it.path)
            )
        }
        preferences.edit().putString("other_engines", stored.toString()).apply()
    }

    private fun readOtherEngines(): List<Endpoint> = runCatching {
        val stored = org.json.JSONArray(preferences.getString("other_engines", "[]") ?: "[]")
        (0 until stored.length()).map { index ->
            val engine = stored.getJSONObject(index)
            Endpoint(
                engine.getString("host"), engine.optInt("port", Endpoint.DEFAULT_PORT), engine.optString("password"),
                transportOf(engine.optString("transport", "")),
                engine.optString("path", Endpoint.DEFAULT_PATH).ifEmpty { Endpoint.DEFAULT_PATH },
            )
        }
    }.getOrDefault(emptyList())

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

    /** Opus bit rate for offline copies in kbps, or 0 for the original files. */
    var downloadBitrate by mutableStateOf(preferences.getInt("download_bitrate", 160))
        private set

    /** Downloads wait for an unmetered network. */
    var downloadOnWifiOnly by mutableStateOf(preferences.getBoolean("download_wifi_only", true))
        private set

    fun updateDownloadBitrate(kbps: Int) {
        downloadBitrate = kbps
        preferences.edit().putInt("download_bitrate", kbps).apply()
    }

    fun updateDownloadOnWifiOnly(on: Boolean) {
        downloadOnWifiOnly = on
        preferences.edit().putBoolean("download_wifi_only", on).apply()
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

private fun transportOf(name: String?): Transport =
    Transport.entries.firstOrNull { it.name == name } ?: Transport.TCP
