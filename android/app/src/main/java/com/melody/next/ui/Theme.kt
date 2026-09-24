package com.melody.next.ui

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.ExperimentalTextApi
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontVariation
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp
import com.melody.next.R
import com.melody.next.Settings

/**
 * The few colours the screens use by name, beyond Material's roles: the
 * quiet text tones and the hairlines between rows.
 */
data class Tones(
    val secondary: Color,
    val muted: Color,
    val faint: Color,
    val hairline: Color,
    val raised: Color,
)

val LocalTones = staticCompositionLocalOf {
    Tones(Color(0xFF9C978E), Color(0xFF6E6A64), Color(0xFF4A4742), Color(0xFF1A1D24), Color(0xFF1E222B))
}

// Ink ground, warm off-white text, one soft blue: as the redesign has it.
private val ink = darkColorScheme(
    primary = Color(0xFF8FB4FF),
    onPrimary = Color(0xFF0D1320),
    primaryContainer = Color(0xFF1E2A44),
    onPrimaryContainer = Color(0xFFD8E3FF),
    secondary = Color(0xFF9C978E),
    onSecondary = Color(0xFF101217),
    secondaryContainer = Color(0xFF1E222B),
    onSecondaryContainer = Color(0xFFECE8E1),
    background = Color(0xFF101217),
    onBackground = Color(0xFFECE8E1),
    surface = Color(0xFF101217),
    onSurface = Color(0xFFECE8E1),
    surfaceVariant = Color(0xFF1E222B),
    onSurfaceVariant = Color(0xFF9C978E),
    surfaceContainerLowest = Color(0xFF0B0D11),
    surfaceContainerLow = Color(0xFF14161C),
    surfaceContainer = Color(0xFF171A21),
    surfaceContainerHigh = Color(0xFF1E222B),
    surfaceContainerHighest = Color(0xFF262A33),
    outline = Color(0xFF3A3F49),
    outlineVariant = Color(0xFF22262E),
    error = Color(0xFFF2B8B5),
    errorContainer = Color(0xFF5B2A2A),
    onErrorContainer = Color(0xFFF3D6D2),
)
private val inkTones = Tones(Color(0xFF9C978E), Color(0xFF6E6A64), Color(0xFF4A4742), Color(0xFF1A1D24), Color(0xFF1E222B))

// The same, in daylight: ivory ground, slate text, a deeper blue.
private val paper = lightColorScheme(
    primary = Color(0xFF2F5FC4),
    onPrimary = Color(0xFFFFFFFF),
    primaryContainer = Color(0xFFDCE6FB),
    onPrimaryContainer = Color(0xFF12275A),
    secondary = Color(0xFF5E5A53),
    secondaryContainer = Color(0xFFEAE6DE),
    onSecondaryContainer = Color(0xFF1B1D22),
    background = Color(0xFFF6F4EF),
    onBackground = Color(0xFF1B1D22),
    surface = Color(0xFFF6F4EF),
    onSurface = Color(0xFF1B1D22),
    surfaceVariant = Color(0xFFEAE6DE),
    onSurfaceVariant = Color(0xFF5E5A53),
    surfaceContainerLowest = Color(0xFFFFFFFF),
    surfaceContainerLow = Color(0xFFF1EEE8),
    surfaceContainer = Color(0xFFECE9E2),
    surfaceContainerHigh = Color(0xFFE6E2DA),
    surfaceContainerHighest = Color(0xFFDFDBD2),
    outline = Color(0xFFC9C4BA),
    outlineVariant = Color(0xFFE2DED6),
    errorContainer = Color(0xFFF6D9D6),
    onErrorContainer = Color(0xFF5B1A16),
)
private val paperTones = Tones(Color(0xFF5E5A53), Color(0xFF8A857C), Color(0xFFB3AEA5), Color(0xFFE6E2DA), Color(0xFFECE9E2))

@OptIn(ExperimentalTextApi::class)
private fun manrope(weight: Int) = Font(
    R.font.manrope,
    weight = FontWeight(weight),
    variationSettings = FontVariation.Settings(FontVariation.weight(weight)),
)

val Manrope = FontFamily(manrope(400), manrope(500), manrope(600), manrope(700), manrope(800))

private fun style(size: Int, weight: Int, tracking: Double = 0.0, line: Int = 0) = TextStyle(
    fontFamily = Manrope,
    fontWeight = FontWeight(weight),
    fontSize = size.sp,
    letterSpacing = tracking.em,
    lineHeight = if (line > 0) line.sp else (size * 1.3).sp,
)

private val type = Typography(
    displaySmall = style(28, 800, -0.02),
    headlineMedium = style(28, 800, -0.02, 34),
    headlineSmall = style(24, 800, -0.02, 30),
    titleLarge = style(20, 800, -0.01),
    titleMedium = style(16, 700),
    titleSmall = style(15, 700),
    bodyLarge = style(16, 600),
    bodyMedium = style(15, 600),
    bodySmall = style(13, 500),
    labelLarge = style(15, 700),
    labelMedium = style(13, 600),
    labelSmall = style(12, 700, 0.06),
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
        dark -> ink
        else -> paper
    }
    CompositionLocalProvider(LocalTones provides if (dark) inkTones else paperTones) {
        MaterialTheme(colorScheme = scheme, typography = type, content = content)
    }
}
