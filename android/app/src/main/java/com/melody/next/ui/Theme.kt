package com.melody.next.ui

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import com.melody.next.Settings

// Melody's palette, as the MPD-era app had it: used unless dynamic colour
// is asked for and the system has it.
private val melodyDark = darkColorScheme(
    primary = Color(0xFF60A5FA),
    onPrimary = Color(0xFF0F172A),
    primaryContainer = Color(0xFF1E40AF),
    onPrimaryContainer = Color(0xFFDBEAFE),
    secondary = Color(0xFF94A3B8),
    onSecondary = Color(0xFF0F172A),
    // Selected chips and the navigation pill: Melody's blue, not the
    // library default's purple.
    secondaryContainer = Color(0xFF1E3A8A),
    onSecondaryContainer = Color(0xFFDBEAFE),
    surface = Color(0xFF1E293B),
    onSurface = Color(0xFFF1F5F9),
    surfaceVariant = Color(0xFF334155),
    onSurfaceVariant = Color(0xFF94A3B8),
    surfaceContainerLow = Color(0xFF1E293B),
    surfaceContainer = Color(0xFF1E293B),
    surfaceContainerHigh = Color(0xFF334155),
    background = Color(0xFF0F172A),
    onBackground = Color(0xFFF1F5F9),
    outline = Color(0xFF475569),
)

private val melodyLight = lightColorScheme(
    primary = Color(0xFF2563EB),
    onPrimary = Color(0xFFFFFFFF),
    primaryContainer = Color(0xFFDBEAFE),
    onPrimaryContainer = Color(0xFF1E3A8A),
    secondary = Color(0xFF475569),
    secondaryContainer = Color(0xFFDBEAFE),
    onSecondaryContainer = Color(0xFF1E3A8A),
    surface = Color(0xFFF8FAFC),
    onSurface = Color(0xFF0F172A),
    surfaceVariant = Color(0xFFE2E8F0),
    onSurfaceVariant = Color(0xFF475569),
    surfaceContainerLow = Color(0xFFF1F5F9),
    surfaceContainer = Color(0xFFF1F5F9),
    surfaceContainerHigh = Color(0xFFE2E8F0),
    background = Color(0xFFFFFFFF),
    onBackground = Color(0xFF0F172A),
    outline = Color(0xFF94A3B8),
)

@Composable
fun MelodyTheme(settings: Settings, content: @Composable () -> Unit) {
    val dark = when (settings.theme) {
        "dark" -> true
        "light" -> false
        else -> isSystemInDarkTheme()
    }
    val context = LocalContext.current
    val scheme = when {
        settings.dynamicColor && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ->
            if (dark) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
        dark -> melodyDark
        else -> melodyLight
    }
    MaterialTheme(colorScheme = scheme, content = content)
}
