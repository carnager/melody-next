package com.melody.next.ui

import android.os.SystemClock
import androidx.activity.compose.BackHandler
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
    Surface(Modifier.fillMaxWidth().clickable(onClick = onOpen), color = MaterialTheme.colorScheme.surfaceContainerHigh) {
        Column {
            LinearProgressIndicator(
                progress = { if (duration > 0) (position.toFloat() / duration).coerceIn(0f, 1f) else 0f },
                modifier = Modifier.fillMaxWidth().height(2.dp),
                trackColor = Color.Transparent,
            )
            Row(
                Modifier.padding(start = 12.dp, end = 4.dp, top = 8.dp, bottom = 8.dp),
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
                IconButton(onClick = vm.client::previous) { Icon(Icons.Default.SkipPrevious, "Previous") }
                IconButton(onClick = vm.client::togglePlay) {
                    Icon(if (state.playing) Icons.Default.Pause else Icons.Default.PlayArrow, "Play or pause", Modifier.size(28.dp))
                }
                IconButton(onClick = vm.client::next) { Icon(Icons.Default.SkipNext, "Next") }
            }
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
    // The library's view of this file: its rating and the key to set one.
    var facts by remember { mutableStateOf<LibraryEntry?>(null) }
    LaunchedEffect(entry?.path) {
        facts = entry?.path?.let { vm.client.track(it) }
    }

    Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
        Column(
            Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding().padding(horizontal = 28.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Row(Modifier.fillMaxWidth().padding(top = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                IconButton(onClick = onClose) { Icon(Icons.Default.KeyboardArrowDown, "Close", Modifier.size(32.dp)) }
                Spacer(Modifier.weight(1f))
                TextButton(onClick = onOutputs) {
                    Icon(Icons.Default.Speaker, null, Modifier.size(18.dp))
                    Spacer(Modifier.width(6.dp))
                    Text(outputs.firstOrNull { it.selected }?.name ?: "Outputs")
                }
            }
            Spacer(Modifier.weight(0.4f))
            BoxWithConstraints(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
                Cover(entry?.let { CoverKey(path = it.path, group = it.albumGroup) }, maxWidth * 0.85f, corner = 20.dp)
            }
            Spacer(Modifier.height(28.dp))
            Text(
                entry?.title ?: "Nothing playing",
                style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Bold,
                maxLines = 2, overflow = TextOverflow.Ellipsis, textAlign = TextAlign.Center,
            )
            Spacer(Modifier.height(4.dp))
            Text(
                listOfNotNull(entry?.artist?.ifEmpty { null }, entry?.album?.ifEmpty { null },
                    entry?.date?.take(4)?.ifEmpty { null }).joinToString(" — "),
                style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1, overflow = TextOverflow.Ellipsis, textAlign = TextAlign.Center,
                modifier = Modifier.fillMaxWidth(),
            )
            if (state.error.isNotEmpty() && !state.playing) {
                Spacer(Modifier.height(8.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(Icons.Default.ErrorOutline, null, tint = MaterialTheme.colorScheme.error, modifier = Modifier.size(18.dp))
                    Spacer(Modifier.width(6.dp))
                    Text("Could not play: ${state.error}", color = MaterialTheme.colorScheme.error,
                        style = MaterialTheme.typography.bodySmall)
                }
            }
            Spacer(Modifier.height(12.dp))
            facts?.let { track ->
                RatingBar(track.rating, onRate = { rating ->
                    scope.launch {
                        runCatching { vm.client.setRating(track.ratingHash, false, rating) }
                            .onSuccess { facts = track.copy(rating = rating) }
                    }
                })
            }
            Spacer(Modifier.height(16.dp))
            SeekBar(vm, state, entry)
            Spacer(Modifier.height(16.dp))
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.Center, verticalAlignment = Alignment.CenterVertically) {
                IconButton(onClick = vm.client::previous, modifier = Modifier.size(64.dp)) {
                    Icon(Icons.Default.SkipPrevious, "Previous", Modifier.size(36.dp))
                }
                Spacer(Modifier.width(20.dp))
                FilledIconButton(onClick = vm.client::togglePlay, modifier = Modifier.size(72.dp), shape = CircleShape) {
                    Icon(if (state.playing) Icons.Default.Pause else Icons.Default.PlayArrow, "Play or pause", Modifier.size(40.dp))
                }
                Spacer(Modifier.width(20.dp))
                IconButton(onClick = vm.client::next, modifier = Modifier.size(64.dp)) {
                    Icon(Icons.Default.SkipNext, "Next", Modifier.size(36.dp))
                }
            }
            Spacer(Modifier.height(16.dp))
            val modes = state.modes
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceEvenly) {
                ModeButton(Icons.Default.Repeat, "Repeat", modes.repeat, onClick = vm.client::toggleRepeat)
                ModeButton(Icons.Default.Shuffle, "Random", modes.random, onClick = vm.client::toggleRandom)
                ModeButton(Icons.Default.LooksOne, "Single", modes.single != 0, once = modes.single == 2, onClick = vm.client::cycleSingle)
                ConsumeButton(modes.consume != 0, once = modes.consume == 2, onClick = vm.client::cycleConsume)
            }
            VolumeBar(state.volume, vm.client::setVolume)
            Spacer(Modifier.weight(1f))
        }
    }
}

@Composable
private fun SeekBar(vm: MainViewModel, state: PlaybackState, entry: QueueEntry?) {
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
    Slider(
        value = fraction,
        onValueChange = { dragging = it },
        onValueChangeFinished = {
            dragging?.let { chosen ->
                sought = (chosen * duration).toLong()
                vm.client.seek(sought)
            }
            dragging = null
        },
        enabled = duration > 0,
        modifier = Modifier.fillMaxWidth(),
    )
    Row(Modifier.fillMaxWidth().padding(horizontal = 4.dp)) {
        Text(formatTime(if (duration > 0) (fraction * duration).toLong() else position),
            style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Spacer(Modifier.weight(1f))
        Text(formatTime(duration), style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun VolumeBar(volume: Int, onVolume: (Int) -> Unit) {
    var dragging by remember { mutableStateOf<Float?>(null) }
    val fraction = dragging ?: (volume / 100f)
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Icon(Icons.AutoMirrored.Filled.VolumeUp, "Volume", Modifier.size(20.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
        Spacer(Modifier.width(8.dp))
        Slider(
            value = fraction,
            onValueChange = { dragging = it },
            onValueChangeFinished = {
                dragging?.let { onVolume((it * 100).toInt()) }
                dragging = null
            },
            modifier = Modifier.weight(1f),
        )
        Spacer(Modifier.width(8.dp))
        Box(Modifier.width(32.dp)) {
            Text("${(fraction * 100).toInt()}", style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
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
    Surface(Modifier.fillMaxWidth().clickable(onClick = onOpen), color = MaterialTheme.colorScheme.surfaceContainerHigh) {
        Column {
            LinearProgressIndicator(
                progress = { if (duration > 0) (position.toFloat() / duration).coerceIn(0f, 1f) else 0f },
                modifier = Modifier.fillMaxWidth().height(2.dp),
                trackColor = Color.Transparent,
            )
            Row(Modifier.padding(start = 12.dp, end = 4.dp, top = 8.dp, bottom = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                KeptCover(album, 40.dp)
                Spacer(Modifier.width(12.dp))
                Column(Modifier.weight(1f)) {
                    Text(offline.track?.entry?.title ?: "—", style = MaterialTheme.typography.bodyMedium, fontWeight = FontWeight.Medium,
                        maxLines = 1, overflow = TextOverflow.Ellipsis)
                    Text("On this phone · ${album.album.artist}", style = MaterialTheme.typography.bodySmall, maxLines = 1,
                        overflow = TextOverflow.Ellipsis, color = MaterialTheme.colorScheme.primary)
                }
                IconButton(onClick = vm.app.offlinePlayer::previous) { Icon(Icons.Default.SkipPrevious, "Previous") }
                IconButton(onClick = vm.app.offlinePlayer::toggle) {
                    Icon(if (offline.playing) Icons.Default.Pause else Icons.Default.PlayArrow, "Play or pause", Modifier.size(28.dp))
                }
                IconButton(onClick = vm.app.offlinePlayer::next) { Icon(Icons.Default.SkipNext, "Next") }
            }
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
