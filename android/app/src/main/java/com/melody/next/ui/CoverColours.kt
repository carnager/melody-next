package com.melody.next.ui

import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.graphics.toArgb
import androidx.core.graphics.ColorUtils
import androidx.palette.graphics.Palette
import com.melody.next.CoverKey
import com.melody.next.MelodyApp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/** Now playing's colours, taken from the cover: a dark ground and a light accent. */
data class CoverColours(
    val ground: Color,
    val text: Color,
    val soft: Color,
    val muted: Color,
    val accent: Color,
)

private val cache = HashMap<String, CoverColours>()

/**
 * The cover's colours, worked out off the main thread: its darkest usable
 * tone, darkened further, as the ground, and its lightest muted tone as the
 * accent. Until they are known, and for an album with no cover, the theme's.
 */
@Composable
fun coverColours(key: CoverKey?): CoverColours {
    val fallback = CoverColours(
        ground = MaterialTheme.colorScheme.background,
        text = MaterialTheme.colorScheme.onBackground,
        soft = LocalTones.current.secondary,
        muted = LocalTones.current.muted,
        accent = MaterialTheme.colorScheme.primary,
    )
    val colours by produceState(key?.identity?.let { synchronized(cache) { cache[it] } } ?: fallback, key?.identity) {
        val identity = key?.identity ?: return@produceState
        synchronized(cache) { cache[identity] }?.let { value = it; return@produceState }
        val bitmap = MelodyApp.instance.covers.load(key, 128) ?: return@produceState
        val made = withContext(Dispatchers.Default) { fromPalette(Palette.from(bitmap.asAndroidBitmap()).generate()) } ?: return@produceState
        synchronized(cache) { cache[identity] = made }
        value = made
    }
    return colours
}

private fun fromPalette(palette: Palette): CoverColours? {
    val base = palette.darkMutedSwatch ?: palette.mutedSwatch ?: palette.darkVibrantSwatch ?: palette.dominantSwatch ?: return null
    val light = palette.lightMutedSwatch ?: palette.lightVibrantSwatch ?: palette.vibrantSwatch ?: palette.mutedSwatch
    // Dark enough for white text whatever the cover: its hue, little light.
    val hsl = FloatArray(3).also { ColorUtils.colorToHSL(base.rgb, it) }
    hsl[1] = hsl[1].coerceAtMost(0.35f)
    hsl[2] = 0.13f
    val ground = Color(ColorUtils.HSLToColor(hsl))
    val accentHsl = FloatArray(3).also { ColorUtils.colorToHSL(light?.rgb ?: base.rgb, it) }
    accentHsl[1] = accentHsl[1].coerceIn(0.2f, 0.55f)
    accentHsl[2] = accentHsl[2].coerceIn(0.66f, 0.78f)
    val accent = Color(ColorUtils.HSLToColor(accentHsl))
    val text = Color(0xFFEEF0EC)
    return CoverColours(
        ground = ground,
        text = text,
        soft = Color(ColorUtils.blendARGB(text.toArgb(), accent.toArgb(), 0.35f)).copy(alpha = 0.85f),
        muted = text.copy(alpha = 0.5f),
        accent = accent,
    )
}
