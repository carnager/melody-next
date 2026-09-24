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
