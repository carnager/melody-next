package com.melody.next.ui

import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectVerticalDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.TransformOrigin
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.launch
import kotlin.math.roundToInt

/** The screen's name, large, with its actions beside it. */
@Composable
fun ScreenHeader(title: String, subtitle: String? = null, actions: @Composable () -> Unit = {}) {
    Row(
        Modifier.fillMaxWidth().padding(start = 20.dp, end = 8.dp, top = 18.dp, bottom = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.headlineMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
            if (subtitle != null) Text(subtitle, style = MaterialTheme.typography.bodySmall, color = LocalTones.current.muted)
        }
        actions()
    }
}

/** Text tabs with an underline under the one shown: quieter than chips. */
@Composable
fun TextTabs(labels: List<String>, selected: Int, onSelect: (Int) -> Unit) {
    val tones = LocalTones.current
    Column {
        Row(Modifier.padding(horizontal = 20.dp), horizontalArrangement = Arrangement.spacedBy(22.dp)) {
            labels.forEachIndexed { index, label ->
                val chosen = index == selected
                Column(
                    Modifier
                        .width(IntrinsicSize.Max)
                        .clickable { onSelect(index) }
                        .padding(top = 10.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    Text(
                        label,
                        style = MaterialTheme.typography.titleSmall,
                        fontWeight = if (chosen) FontWeight.ExtraBold else FontWeight.SemiBold,
                        color = if (chosen) MaterialTheme.colorScheme.onSurface else tones.muted,
                    )
                    Box(
                        Modifier
                            .padding(top = 10.dp)
                            .height(2.dp)
                            .fillMaxWidth()
                            .background(if (chosen) MaterialTheme.colorScheme.primary else Color.Transparent)
                            .clip(RoundedCornerShape(1.dp)),
                    )
                }
            }
        }
        Box(Modifier.fillMaxWidth().height(1.dp).background(MaterialTheme.colorScheme.outlineVariant))
    }
}

/**
 * Initials on a muted tile, as Trackknife's library shows an artist: a tone
 * of its own per name, so a list reads as more than one grey column.
 */
@Composable
fun InitialsTile(name: String, size: androidx.compose.ui.unit.Dp = 36.dp) {
    val words = name.replace(Regex("[^\\p{L}\\p{N} ]"), " ").trim().split(Regex("\\s+")).filter { it.isNotEmpty() }
    val initials = when {
        words.isEmpty() -> "?"
        // "10 Minute Warning" is 10, not 1M.
        words[0].first().isDigit() || words.size == 1 -> words[0].take(2)
        else -> "${words[0].first()}${words[1].first()}"
    }.uppercase()
    val tint = tileTints[(name.hashCode() and 0x7fffffff) % tileTints.size]
    Box(
        Modifier.size(size).clip(RoundedCornerShape(10.dp)).background(tint),
        contentAlignment = Alignment.Center,
    ) {
        Text(initials, fontSize = (size.value * 0.36f).sp, fontWeight = FontWeight.ExtraBold, color = Color(0xFF0F1115), fontFamily = Manrope)
    }
}

private val tileTints = listOf(
    Color(0xFFC9B58F), Color(0xFF9FB9A4), Color(0xFFB7A4C7), Color(0xFFC7A39A),
    Color(0xFF9FB2C9), Color(0xFFC2BD8E), Color(0xFFA9C3BD),
)

/**
 * What marks the playing track: three small bars where its number was,
 * moving while it plays and still while paused. Nothing else changes about
 * the row but the title's colour.
 */
@Composable
fun PlayingBars(moving: Boolean, color: Color = MaterialTheme.colorScheme.primary) {
    val motion = rememberInfiniteTransition(label = "bars")
    val phases = listOf(0, 300, 600).map { delay ->
        if (moving) motion.animateFloat(
            initialValue = 0.35f, targetValue = 1f,
            animationSpec = infiniteRepeatable(tween(450, delayMillis = delay), RepeatMode.Reverse),
            label = "bar$delay",
        ).value else listOf(0.55f, 0.9f, 0.7f)[delay / 300]
    }
    Row(Modifier.height(14.dp), horizontalArrangement = Arrangement.spacedBy(2.dp), verticalAlignment = Alignment.Bottom) {
        phases.forEach { scale ->
            Box(
                Modifier
                    .width(3.dp)
                    .fillMaxHeight()
                    .graphicsLayer { scaleY = scale; transformOrigin = TransformOrigin(0.5f, 1f) }
                    .clip(RoundedCornerShape(2.dp))
                    .background(color),
            )
        }
    }
}

/**
 * Letters down the right edge: touch or drag along them and the list jumps
 * to that letter, with the letter shown large beside the thumb. `sections`
 * are the letters there are, with the index of each one's first row; a
 * letter with none goes to the next that has some.
 */
@Composable
fun AlphabetRail(sections: List<Pair<Char, Int>>, list: LazyListState, modifier: Modifier = Modifier) {
    if (sections.size < 2) return
    val letters = remember { "#ABCDEFGHIJKLMNOPQRSTUVWXYZ".toList() }
    val scope = rememberCoroutineScope()
    val density = LocalDensity.current
    var height by remember { mutableIntStateOf(1) }
    var touching by remember { mutableStateOf<Char?>(null) }
    var touchY by remember { mutableIntStateOf(0) }
    val present = remember(sections) { sections.map { it.first }.toSet() }

    fun jump(y: Float) {
        val index = (y / height * letters.size).toInt().coerceIn(0, letters.lastIndex)
        val letter = letters[index]
        touching = letter
        touchY = y.roundToInt()
        val target = sections.firstOrNull { letters.indexOf(it.first) >= index } ?: sections.last()
        scope.launch { list.scrollToItem(target.second) }
    }

    Box(modifier) {
        Column(
            Modifier
                .fillMaxHeight()
                .width(28.dp)
                .onSizeChanged { height = it.height.coerceAtLeast(1) }
                .pointerInput(sections) {
                    detectTapGestures(onPress = { offset ->
                        jump(offset.y)
                        tryAwaitRelease()
                        touching = null
                    })
                }
                .pointerInput(sections) {
                    detectVerticalDragGestures(
                        onDragStart = { jump(it.y) },
                        onDragEnd = { touching = null },
                        onDragCancel = { touching = null },
                    ) { change, _ -> jump(change.position.y) }
                },
            verticalArrangement = Arrangement.SpaceEvenly,
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            letters.forEach { letter ->
                Text(
                    letter.toString(),
                    fontSize = 10.sp,
                    lineHeight = 11.sp,
                    fontWeight = FontWeight.Bold,
                    fontFamily = Manrope,
                    color = when {
                        letter == touching -> MaterialTheme.colorScheme.primary
                        letter in present -> LocalTones.current.muted
                        else -> LocalTones.current.faint
                    },
                )
            }
        }
        touching?.let { letter ->
            val bubble = 60.dp
            val offsetY = with(density) { (touchY - bubble.toPx() / 2).roundToInt() }
            Box(
                Modifier
                    .offset { IntOffset(-with(density) { (bubble + 16.dp).roundToPx() }, offsetY.coerceAtLeast(0)) }
                    .size(bubble)
                    .clip(RoundedCornerShape(topStart = 30.dp, topEnd = 30.dp, bottomStart = 30.dp, bottomEnd = 4.dp))
                    .background(MaterialTheme.colorScheme.primary),
                contentAlignment = Alignment.Center,
            ) {
                Text(letter.toString(), fontSize = 28.sp, fontWeight = FontWeight.ExtraBold, fontFamily = Manrope, color = MaterialTheme.colorScheme.onPrimary)
            }
        }
    }
}

/** A row's letter for the rail: '#' for anything that doesn't start with one. */
fun sectionLetter(name: String): Char {
    val first = name.trimStart().firstOrNull()?.uppercaseChar() ?: return '#'
    val plain = java.text.Normalizer.normalize(first.toString(), java.text.Normalizer.Form.NFD).first()
    return if (plain in 'A'..'Z') plain else '#'
}
