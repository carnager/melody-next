package com.melody.next.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.StarHalf
import androidx.compose.material.icons.filled.Album
import androidx.compose.material.icons.filled.Star
import androidx.compose.material.icons.filled.StarOutline
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.melody.next.CoverKey
import com.melody.next.MelodyApp

/**
 * A cover, asked of the engine at the size it is shown -- in pixels, so a
 * dense screen gets a sharp one and a list row gets no more than it needs.
 */
@Composable
fun Cover(key: CoverKey?, size: Dp, modifier: Modifier = Modifier, corner: Dp = 8.dp) {
    val pixels = with(LocalDensity.current) { size.roundToPx() }
    // A few sizes rather than every size: each is fetched and kept apart.
    val asked = when {
        pixels <= 128 -> 128
        pixels <= 256 -> 256
        pixels <= 512 -> 512
        else -> 1024
    }
    val covers = MelodyApp.instance.covers
    var image by remember(key, asked) { mutableStateOf(key?.let { covers.cached(it, asked) }) }
    // Asked again once connected: a cover asked for while the app was
    // still connecting -- the mini player's, at start -- found no engine.
    val connected = MelodyApp.instance.client.connection.collectAsState().value is com.melody.next.engine.ConnectionState.Connected
    LaunchedEffect(key, asked, connected) {
        if (key != null && image == null) image = covers.load(key, asked)
    }
    Box(
        modifier
            .size(size)
            .clip(RoundedCornerShape(corner))
            .background(MaterialTheme.colorScheme.surfaceVariant),
        contentAlignment = Alignment.Center,
    ) {
        val shown = image
        if (shown != null) {
            Image(shown, contentDescription = null, contentScale = ContentScale.Crop, modifier = Modifier.fillMaxSize())
        } else {
            Icon(
                Icons.Default.Album, contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.5f),
                modifier = Modifier.size(size * 0.45f),
            )
        }
    }
}

/** Five stars over the engine's 0-10 rating; a tap on a star's left half sets a half. */
@Composable
fun RatingBar(
    rating: Int,
    onRate: (Int) -> Unit,
    starSize: Dp = 28.dp,
    tint: androidx.compose.ui.graphics.Color = MaterialTheme.colorScheme.primary,
    idle: androidx.compose.ui.graphics.Color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f),
) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        for (star in 1..5) {
            val full = rating >= star * 2
            val half = !full && rating == star * 2 - 1
            Box(Modifier.size(starSize + 6.dp), contentAlignment = Alignment.Center) {
                Icon(
                    when {
                        full -> Icons.Default.Star
                        half -> Icons.AutoMirrored.Filled.StarHalf
                        else -> Icons.Default.StarOutline
                    },
                    contentDescription = "$star stars",
                    tint = if (full || half) tint else idle,
                    modifier = Modifier.size(starSize),
                )
                // Two halves: the left one gives a half star, the right one a
                // whole; tapping what is already set clears it.
                Row(Modifier.fillMaxSize()) {
                    for (value in listOf(star * 2 - 1, star * 2)) {
                        Box(
                            Modifier
                                .weight(1f)
                                .fillMaxSize()
                                .clickable { onRate(if (rating == value) 0 else value) }
                        )
                    }
                }
            }
        }
    }
}

@Composable
fun ModeButton(icon: ImageVector, description: String, active: Boolean, once: Boolean = false, onClick: () -> Unit) {
    val tint = if (active) MaterialTheme.colorScheme.primary
    else MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f)
    IconButton(onClick = onClick, modifier = Modifier.size(44.dp)) {
        Box(contentAlignment = Alignment.Center) {
            Icon(icon, contentDescription = description, modifier = Modifier.size(24.dp), tint = tint)
            if (once) {
                // A one-shot mode: a dot, as Trackknife marks it.
                Canvas(Modifier.size(30.dp)) {
                    drawCircle(tint, radius = 3.dp.toPx(), center = center.copy(x = size.width - 3.dp.toPx(), y = 3.dp.toPx()))
                }
            }
        }
    }
}

/** Consume, drawn as the old app drew it. */
@Composable
fun ConsumeButton(active: Boolean, once: Boolean, onClick: () -> Unit) {
    val color = if (active) MaterialTheme.colorScheme.primary
    else MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f)
    IconButton(onClick = onClick, modifier = Modifier.size(44.dp)) {
        Canvas(Modifier.size(24.dp)) {
            drawArc(color = color, startAngle = 35f, sweepAngle = 290f, useCenter = true)
            if (once) drawCircle(color, radius = 3.dp.toPx(), center = center.copy(x = size.width + 2.dp.toPx(), y = 0f))
        }
    }
}

fun formatTime(ms: Long): String {
    if (ms < 0) return "–:––"
    val seconds = ms / 1000
    val hours = seconds / 3600
    return if (hours > 0) "%d:%02d:%02d".format(hours, (seconds / 60) % 60, seconds % 60)
    else "%d:%02d".format(seconds / 60, seconds % 60)
}

/** "3 hours ago", "yesterday" -- how the newest albums say when they came. */
fun formatAdded(addedSeconds: Long, nowSeconds: Long = System.currentTimeMillis() / 1000): String {
    if (addedSeconds <= 0) return ""
    val minutes = (nowSeconds - addedSeconds).coerceAtLeast(0) / 60
    return when {
        minutes < 1 -> "just now"
        minutes < 60 -> "$minutes min ago"
        minutes < 60 * 24 -> "${minutes / 60} h ago"
        minutes < 60 * 48 -> "yesterday"
        minutes < 60 * 24 * 30 -> "${minutes / (60 * 24)} days ago"
        minutes < 60 * 24 * 365 -> "${minutes / (60 * 24 * 30)} months ago"
        else -> "${minutes / (60 * 24 * 365)} years ago"
    }
}

/** Consume, drawn as the old app drew it, in a given colour. */
@Composable
fun ConsumeMark(active: Boolean, once: Boolean, color: androidx.compose.ui.graphics.Color) {
    Canvas(Modifier.size(22.dp)) {
        drawArc(color = color, startAngle = 35f, sweepAngle = 290f, useCenter = true,
            style = if (active) androidx.compose.ui.graphics.drawscope.Fill else androidx.compose.ui.graphics.drawscope.Stroke(width = 1.8.dp.toPx()))
        if (active) drawCircle(color.copy(alpha = if (once) 0.5f else 1f), radius = 2.dp.toPx(), center = center.copy(y = size.height + 6.dp.toPx()))
    }
}
