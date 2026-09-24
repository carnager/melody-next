package com.melody.next.ui

import android.os.SystemClock
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.automirrored.filled.VolumeDown
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.VolumeUp
import androidx.compose.material.icons.filled.ErrorOutline
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.LooksOne
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Repeat
import androidx.compose.material.icons.filled.Shuffle
import androidx.compose.material.icons.filled.SkipNext
import androidx.compose.material.icons.filled.SkipPrevious
import androidx.compose.material.icons.filled.Speaker
import androidx.compose.material3.FilledIconButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.melody.next.CoverKey
import com.melody.next.engine.ConnectionState
import com.melody.next.engine.LibraryEntry
import com.melody.next.engine.PlaybackState
import com.melody.next.engine.QueueEntry
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/** The entry playing, found in the queue or Up Next -- it can be in either. */
@Composable
fun currentEntry(vm: MainViewModel): QueueEntry? {
    val state by vm.client.state.collectAsState()
    val queue by vm.client.queue.collectAsState()
    val upNext by vm.client.upNext.collectAsState()
    return remember(state.entry, queue, upNext) {
        queue.firstOrNull { it.entry == state.entry } ?: upNext.firstOrNull { it.entry == state.entry }
    }
}

/** The position now, ticking while it plays: counted here, corrected by the engine. */
@Composable
fun livePosition(state: PlaybackState): Long {
    val now by produceState(SystemClock.elapsedRealtime(), state) {
        while (state.playing) {
            value = SystemClock.elapsedRealtime()
            delay(250)
        }
        value = SystemClock.elapsedRealtime()
    }
    return state.positionAt(now)
}

@Composable
fun ConnectionBanner(connection: ConnectionState) {
    val text = when (connection) {
        is ConnectionState.Connecting -> "Connecting to ${connection.endpoint}…" +
            if (connection.lastError.isNotEmpty()) " (${connection.lastError})" else ""
        is ConnectionState.Refused -> "${connection.endpoint}: ${connection.reason}"
        else -> return
    }
    Surface(color = MaterialTheme.colorScheme.errorContainer) {
        Text(
            text, style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onErrorContainer,
            maxLines = 1, overflow = TextOverflow.Ellipsis,
            modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 6.dp),
        )
    }
}

@Composable
fun MiniPlayer(vm: MainViewModel, onOpen: () -> Unit) {
    val offline by vm.app.offlinePlayer.state.collectAsState()
    if (offline.active) {
        OfflineMiniPlayer(vm, onOpen)
        return
    }
    val state by vm.client.state.collectAsState()
    val entry = currentEntry(vm)
    if (state.entry.isEmpty() && state.error.isEmpty()) return
    val position = livePosition(state)
    val duration = state.durationMs.takeIf { it > 0 } ?: entry?.durationMs ?: -1
    Surface(
        Modifier
            .fillMaxWidth()
            .padding(start = 10.dp, end = 10.dp, bottom = 8.dp)
            .clip(androidx.compose.foundation.shape.RoundedCornerShape(16.dp))
            .clickable(onClick = onOpen),
        color = LocalTones.current.raised,
    ) {
        Column {
            Row(
                Modifier.padding(start = 8.dp, end = 4.dp, top = 8.dp, bottom = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Cover(entry?.let { CoverKey(path = it.path, group = it.albumGroup) }, 40.dp)
                Spacer(Modifier.width(12.dp))
                Column(Modifier.weight(1f)) {
                    if (state.error.isNotEmpty() && !state.playing) {
                        Text("Could not play", style = MaterialTheme.typography.bodyMedium, fontWeight = FontWeight.Medium,
                            color = MaterialTheme.colorScheme.error, maxLines = 1)
                        Text(state.error, style = MaterialTheme.typography.bodySmall, maxLines = 1,
                            overflow = TextOverflow.Ellipsis, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    } else {
                        Text(entry?.title ?: "—", style = MaterialTheme.typography.bodyMedium, fontWeight = FontWeight.Medium,
                            maxLines = 1, overflow = TextOverflow.Ellipsis)
                        Text(
                            state.speakersTakenBy.takeIf { it.isNotEmpty() && !state.playing }
                                ?.let { "Paused · $it is playing on these speakers" } ?: entry?.artist.orEmpty(),
                            style = MaterialTheme.typography.bodySmall, maxLines = 1, overflow = TextOverflow.Ellipsis,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
                IconButton(onClick = vm.client::togglePlay) {
                    Icon(if (state.playing) Icons.Default.Pause else Icons.Default.PlayArrow, "Play or pause", Modifier.size(28.dp))
                }
                IconButton(onClick = vm.client::next) { Icon(Icons.Default.SkipNext, "Next") }
            }
            MiniProgress(if (duration > 0) (position.toFloat() / duration).coerceIn(0f, 1f) else 0f)
        }
    }
}

@Composable
fun NowPlayingScreen(vm: MainViewModel, onOutputs: () -> Unit, onClose: () -> Unit) {
    BackHandler(onBack = onClose)
    val offline by vm.app.offlinePlayer.state.collectAsState()
    if (offline.active) {
        OfflineNowPlaying(vm, onClose)
        return
    }
    val state by vm.client.state.collectAsState()
    val outputs by vm.client.outputs.collectAsState()
    val entry = currentEntry(vm)
    val scope = rememberCoroutineScope()
    val cover = entry?.let { CoverKey(path = it.path, group = it.albumGroup) }
    val colours = coverColours(cover)
    // The library's view of this file: its rating and the key to set one.
    var facts by remember { mutableStateOf<LibraryEntry?>(null) }
    LaunchedEffect(entry?.path) {
        facts = entry?.path?.let { vm.client.track(it) }
    }

    Surface(Modifier.fillMaxSize(), color = colours.ground, contentColor = colours.text) {
        Column(
            Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding().padding(horizontal = 28.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Row(Modifier.fillMaxWidth().padding(top = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                IconButton(onClick = onClose, modifier = Modifier.offset(x = (-12).dp)) {
                    Icon(Icons.Default.KeyboardArrowDown, "Close", Modifier.size(30.dp))
                }
                Spacer(Modifier.weight(1f))
                // Where it plays, as a chip: tapping it is how to change it.
                Row(
                    Modifier
                        .clip(RoundedCornerShape(18.dp))
                        .background(colours.text.copy(alpha = 0.08f))
                        .clickable(onClick = onOutputs)
                        .padding(start = 10.dp, end = 14.dp, top = 8.dp, bottom = 8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Icon(Icons.Default.Speaker, null, Modifier.size(18.dp), tint = colours.soft)
                    Spacer(Modifier.width(6.dp))
                    Text(outputs.firstOrNull { it.selected }?.name ?: "Outputs", style = MaterialTheme.typography.labelMedium,
                        fontWeight = FontWeight.Bold, color = colours.soft)
                }
            }
            Spacer(Modifier.weight(0.35f))
            BoxWithConstraints(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
                val side = maxWidth
                Box(Modifier.shadow(28.dp, RoundedCornerShape(16.dp))) {
                    Cover(cover, side, corner = 16.dp)
                }
            }
            Spacer(Modifier.weight(0.3f))
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.Bottom) {
                Column(Modifier.weight(1f)) {
                    Text(entry?.title ?: "Nothing playing", style = MaterialTheme.typography.headlineSmall,
                        maxLines = 2, overflow = TextOverflow.Ellipsis)
                    Text(
                        listOfNotNull(entry?.artist?.ifEmpty { null }, entry?.album?.ifEmpty { null }).joinToString(" · "),
                        style = MaterialTheme.typography.bodyMedium, color = colours.soft,
                        maxLines = 1, overflow = TextOverflow.Ellipsis,
                    )
                }
                facts?.let { track ->
                    Box(Modifier.padding(bottom = 2.dp)) {
                        androidx.compose.runtime.CompositionLocalProvider(
                            androidx.compose.material3.LocalContentColor provides colours.accent,
                        ) {
                            RatingBar(track.rating, onRate = { rating ->
                                scope.launch {
                                    runCatching { vm.client.setRating(track.ratingHash, false, rating) }
                                        .onSuccess { facts = track.copy(rating = rating) }
                                }
                            }, starSize = 15.dp, tint = colours.accent, idle = colours.muted)
                        }
                    }
                }
            }
            if (state.error.isNotEmpty() && !state.playing) {
                Spacer(Modifier.height(8.dp))
                Text("Could not play: ${state.error}", color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall,
                    modifier = Modifier.fillMaxWidth())
            }
            Spacer(Modifier.height(22.dp))
            SeekBar(vm, state, entry, colours)
            Spacer(Modifier.height(10.dp))
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.Center, verticalAlignment = Alignment.CenterVertically) {
                IconButton(onClick = vm.client::previous, modifier = Modifier.size(60.dp)) {
                    Icon(Icons.Default.SkipPrevious, "Previous", Modifier.size(34.dp))
                }
                Spacer(Modifier.width(24.dp))
                Box(
                    Modifier.size(76.dp).clip(CircleShape).background(colours.text).clickable(onClick = vm.client::togglePlay),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(if (state.playing) Icons.Default.Pause else Icons.Default.PlayArrow, "Play or pause",
                        Modifier.size(36.dp), tint = colours.ground)
                }
                Spacer(Modifier.width(24.dp))
                IconButton(onClick = vm.client::next, modifier = Modifier.size(60.dp)) {
                    Icon(Icons.Default.SkipNext, "Next", Modifier.size(34.dp))
                }
            }
            Spacer(Modifier.height(14.dp))
            val modes = state.modes
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                ModeToggle(Icons.Default.Repeat, "Repeat", modes.repeat, false, colours, vm.client::toggleRepeat)
                ModeToggle(Icons.Default.Shuffle, "Random", modes.random, false, colours, vm.client::toggleRandom)
                ModeToggle(Icons.Default.LooksOne, "Single", modes.single != 0, modes.single == 2, colours, vm.client::cycleSingle)
                IconButton(onClick = vm.client::cycleConsume) {
                    ConsumeMark(modes.consume != 0, modes.consume == 2, if (modes.consume != 0) colours.accent else colours.muted)
                }
            }
            Spacer(Modifier.height(8.dp))
            VolumeBar(state.volume, colours, vm.client::setVolume)
            Spacer(Modifier.weight(0.35f))
        }
    }
}

/** A mode: its icon in the accent when on, with a dot beneath when it's a one-shot. */
@Composable
private fun ModeToggle(icon: androidx.compose.ui.graphics.vector.ImageVector, label: String, on: Boolean, once: Boolean,
                       colours: CoverColours, onClick: () -> Unit) {
    IconButton(onClick = onClick) {
        Box(contentAlignment = Alignment.Center) {
            Icon(icon, label, Modifier.size(22.dp), tint = if (on) colours.accent else colours.muted)
            if (on) Box(Modifier.offset(y = 16.dp).size(4.dp).clip(CircleShape).background(colours.accent.copy(alpha = if (once) 0.5f else 1f)))
        }
    }
}

@Composable
private fun SeekBar(vm: MainViewModel, state: PlaybackState, entry: QueueEntry?, colours: CoverColours) {
    val duration = state.durationMs.takeIf { it > 0 } ?: entry?.durationMs ?: -1
    val position = livePosition(state)
    var dragging by remember { mutableStateOf<Float?>(null) }
    // Held after a seek until the engine's state catches up, or the thumb
    // would jump back for a moment.
    var sought by remember { mutableLongStateOf(-1L) }
    LaunchedEffect(state.positionMs, state.entry) { sought = -1L }
    val fraction = when {
        dragging != null -> dragging!!
        sought >= 0 && duration > 0 -> sought.toFloat() / duration
        duration > 0 -> position.toFloat() / duration
        else -> 0f
    }.coerceIn(0f, 1f)
    ThinSlider(
        value = fraction,
        enabled = duration > 0,
        fill = colours.accent,
        track = colours.text.copy(alpha = 0.14f),
        thumb = colours.text,
        onChange = { dragging = it },
        onDone = {
            dragging?.let { chosen ->
                sought = (chosen * duration).toLong()
                vm.client.seek(sought)
            }
            dragging = null
        },
    )
    Row(Modifier.fillMaxWidth().padding(top = 6.dp)) {
        Text(formatTime(if (duration > 0) (fraction * duration).toLong() else position),
            style = MaterialTheme.typography.labelMedium, color = colours.muted)
        Spacer(Modifier.weight(1f))
        Text(formatTime(duration), style = MaterialTheme.typography.labelMedium, color = colours.muted)
    }
}

@Composable
private fun VolumeBar(volume: Int, colours: CoverColours, onVolume: (Int) -> Unit) {
    var dragging by remember { mutableStateOf<Float?>(null) }
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Icon(Icons.AutoMirrored.Filled.VolumeDown, "Volume", Modifier.size(18.dp), tint = colours.muted)
        Spacer(Modifier.width(12.dp))
        Box(Modifier.weight(1f)) {
            ThinSlider(
                value = dragging ?: (volume / 100f),
                enabled = true,
                fill = colours.soft,
                track = colours.text.copy(alpha = 0.14f),
                thumb = null,
                height = 3.dp,
                onChange = { dragging = it },
                onDone = {
                    dragging?.let { onVolume((it * 100).toInt()) }
                    dragging = null
                },
            )
        }
        Spacer(Modifier.width(12.dp))
        Icon(Icons.AutoMirrored.Filled.VolumeUp, null, Modifier.size(18.dp), tint = colours.muted)
    }
}

/**
 * A thin line to drag along, with a small round thumb: the seek and volume
 * bars. Material's slider is thicker than this screen wants.
 */
@Composable
private fun ThinSlider(
    value: Float,
    enabled: Boolean,
    fill: Color,
    track: Color,
    thumb: Color?,
    height: androidx.compose.ui.unit.Dp = 4.dp,
    onChange: (Float) -> Unit,
    onDone: () -> Unit,
) {
    var width by remember { mutableIntStateOf(1) }
    Box(
        Modifier
            .fillMaxWidth()
            .height(28.dp)
            .onSizeChanged { width = it.width.coerceAtLeast(1) }
            .pointerInput(enabled) {
                if (!enabled) return@pointerInput
                detectTapGestures { offset ->
                    onChange((offset.x / width).coerceIn(0f, 1f))
                    onDone()
                }
            }
            .pointerInput(enabled) {
                if (!enabled) return@pointerInput
                detectHorizontalDragGestures(
                    onDragStart = { onChange((it.x / width).coerceIn(0f, 1f)) },
                    onDragEnd = onDone,
                    onDragCancel = onDone,
                ) { change, _ -> onChange((change.position.x / width).coerceIn(0f, 1f)) }
            },
        contentAlignment = Alignment.CenterStart,
    ) {
        Box(Modifier.fillMaxWidth().height(height).clip(RoundedCornerShape(2.dp)).background(track))
        Box(Modifier.fillMaxWidth(value).height(height).clip(RoundedCornerShape(2.dp)).background(fill))
        if (thumb != null && enabled) {
            val offset = with(LocalDensity.current) { (value * width).toDp() - 7.dp }
            Box(Modifier.offset(x = offset.coerceAtLeast(0.dp)).size(14.dp).clip(CircleShape).background(thumb))
        }
    }
}

/** The offline player's position, polled while it plays: ExoPlayer has no flow for it. */
@Composable
private fun offlinePosition(vm: MainViewModel): Pair<Long, Long> {
    val offline by vm.app.offlinePlayer.state.collectAsState()
    val player = vm.app.offlinePlayer.player
    val position by produceState(0L to 0L, offline) {
        while (true) {
            value = player.currentPosition to player.duration.coerceAtLeast(0)
            delay(250)
        }
    }
    return position
}

@Composable
private fun OfflineMiniPlayer(vm: MainViewModel, onOpen: () -> Unit) {
    val offline by vm.app.offlinePlayer.state.collectAsState()
    val (position, duration) = offlinePosition(vm)
    val album = offline.album ?: return
    Surface(
        Modifier
            .fillMaxWidth()
            .padding(start = 10.dp, end = 10.dp, bottom = 8.dp)
            .clip(androidx.compose.foundation.shape.RoundedCornerShape(16.dp))
            .clickable(onClick = onOpen),
        color = LocalTones.current.raised,
    ) {
        Column {
            Row(Modifier.padding(start = 8.dp, end = 4.dp, top = 8.dp, bottom = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                KeptCover(album, 40.dp)
                Spacer(Modifier.width(12.dp))
                Column(Modifier.weight(1f)) {
                    Text(offline.track?.entry?.title ?: "—", style = MaterialTheme.typography.bodyMedium, fontWeight = FontWeight.Medium,
                        maxLines = 1, overflow = TextOverflow.Ellipsis)
                    Text("On this phone · ${album.album.artist}", style = MaterialTheme.typography.bodySmall, maxLines = 1,
                        overflow = TextOverflow.Ellipsis, color = MaterialTheme.colorScheme.primary)
                }
                IconButton(onClick = vm.app.offlinePlayer::toggle) {
                    Icon(if (offline.playing) Icons.Default.Pause else Icons.Default.PlayArrow, "Play or pause", Modifier.size(28.dp))
                }
                IconButton(onClick = vm.app.offlinePlayer::next) { Icon(Icons.Default.SkipNext, "Next") }
            }
            MiniProgress(if (duration > 0) (position.toFloat() / duration).coerceIn(0f, 1f) else 0f)
        }
    }
}

@Composable
private fun OfflineNowPlaying(vm: MainViewModel, onClose: () -> Unit) {
    val offline by vm.app.offlinePlayer.state.collectAsState()
    val (position, duration) = offlinePosition(vm)
    val album = offline.album ?: return
    var dragging by remember { mutableStateOf<Float?>(null) }
    Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
        Column(
            Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding().padding(horizontal = 28.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Row(Modifier.fillMaxWidth().padding(top = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                IconButton(onClick = onClose) { Icon(Icons.Default.KeyboardArrowDown, "Close", Modifier.size(32.dp)) }
                Spacer(Modifier.weight(1f))
                // Back to the engine's controls: this player stops.
                TextButton(onClick = { vm.app.offlinePlayer.close(); onClose() }) { Text("Stop playing offline") }
            }
            Spacer(Modifier.weight(0.4f))
            BoxWithConstraints(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
                KeptCover(album, maxWidth * 0.85f, corner = 20.dp)
            }
            Spacer(Modifier.height(28.dp))
            Text(offline.track?.entry?.title ?: "—", style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Bold,
                maxLines = 2, overflow = TextOverflow.Ellipsis, textAlign = TextAlign.Center)
            Spacer(Modifier.height(4.dp))
            Text(listOf(offline.track?.entry?.artist.orEmpty(), album.album.album).filter { it.isNotEmpty() }.joinToString(" — "),
                style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1, overflow = TextOverflow.Ellipsis, textAlign = TextAlign.Center, modifier = Modifier.fillMaxWidth())
            Text("Playing on this phone, from its own copy", style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.primary)
            Spacer(Modifier.height(16.dp))
            val fraction = dragging ?: if (duration > 0) (position.toFloat() / duration).coerceIn(0f, 1f) else 0f
            Slider(
                value = fraction,
                onValueChange = { dragging = it },
                onValueChangeFinished = { dragging?.let { vm.app.offlinePlayer.seek((it * duration).toLong()) }; dragging = null },
                enabled = duration > 0,
                modifier = Modifier.fillMaxWidth(),
            )
            Row(Modifier.fillMaxWidth().padding(horizontal = 4.dp)) {
                Text(formatTime((fraction * duration).toLong()), style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                Spacer(Modifier.weight(1f))
                Text(formatTime(duration), style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Spacer(Modifier.height(16.dp))
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.Center, verticalAlignment = Alignment.CenterVertically) {
                IconButton(onClick = vm.app.offlinePlayer::previous, modifier = Modifier.size(64.dp)) { Icon(Icons.Default.SkipPrevious, "Previous", Modifier.size(36.dp)) }
                Spacer(Modifier.width(20.dp))
                FilledIconButton(onClick = vm.app.offlinePlayer::toggle, modifier = Modifier.size(72.dp), shape = CircleShape) {
                    Icon(if (offline.playing) Icons.Default.Pause else Icons.Default.PlayArrow, "Play or pause", Modifier.size(40.dp))
                }
                Spacer(Modifier.width(20.dp))
                IconButton(onClick = vm.app.offlinePlayer::next, modifier = Modifier.size(64.dp)) { Icon(Icons.Default.SkipNext, "Next", Modifier.size(36.dp)) }
            }
            Spacer(Modifier.weight(1f))
        }
    }
}

/** The thin line along the mini player's bottom edge. */
@Composable
private fun MiniProgress(fraction: Float) {
    Box(Modifier.fillMaxWidth().height(2.dp).background(MaterialTheme.colorScheme.outline.copy(alpha = 0.35f))) {
        Box(Modifier.fillMaxWidth(fraction).height(2.dp).background(MaterialTheme.colorScheme.primary))
    }
}
