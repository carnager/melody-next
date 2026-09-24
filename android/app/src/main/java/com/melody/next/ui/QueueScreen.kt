package com.melody.next.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Delete
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SwipeToDismissBox
import androidx.compose.material3.SwipeToDismissBoxValue
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberSwipeToDismissBoxState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.melody.next.CoverKey
import com.melody.next.engine.QueueEntry

/**
 * The engine's queue and Up Next. Up Next plays before the queue goes on;
 * the queue is shown by album, as Trackknife groups it, with what has
 * already played dimmed. A row goes with a swipe.
 */
@Composable
fun QueueScreen(vm: MainViewModel, onBrowse: () -> Unit) {
    val queue by vm.client.queue.collectAsState()
    val upNext by vm.client.upNext.collectAsState()
    val state by vm.client.state.collectAsState()
    val tones = LocalTones.current
    if (queue.isEmpty() && upNext.isEmpty()) {
        Column(
            Modifier.fillMaxSize().padding(32.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
        ) {
            Text("Nothing queued", style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.height(4.dp))
            Text("Play an album, or add one to Up Next.", style = MaterialTheme.typography.bodySmall, color = tones.secondary)
            Spacer(Modifier.height(16.dp))
            Button(onClick = onBrowse) { Text("Browse the library") }
        }
        return
    }
    val list = rememberLazyListState()
    val playingRow = queue.indexOfFirst { it.entry == state.entry }
    // Opened on what plays, not on the top of a long queue.
    LaunchedEffect(Unit) {
        if (playingRow > 2) list.scrollToItem(playingRow + (if (upNext.isEmpty()) 0 else 1) + albumsBefore(queue, playingRow) - 1)
    }
    LazyColumn(state = list, modifier = Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = 16.dp)) {
        if (upNext.isNotEmpty()) {
            item(key = "up-next") {
                Column(
                    Modifier
                        .padding(horizontal = 12.dp, vertical = 4.dp)
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(14.dp))
                        .background(MaterialTheme.colorScheme.surfaceContainer)
                        .padding(bottom = 6.dp),
                ) {
                    Row(Modifier.padding(start = 14.dp, end = 4.dp, top = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                        Text("UP NEXT", style = MaterialTheme.typography.labelSmall, fontWeight = FontWeight.ExtraBold,
                            color = MaterialTheme.colorScheme.primary, modifier = Modifier.weight(1f))
                        TextButton(onClick = vm.client::clearRequests) { Text("Clear", color = tones.secondary) }
                    }
                    upNext.forEach { entry ->
                        Swipeable(onRemove = { vm.client.removeRequest(entry.entry) }, background = MaterialTheme.colorScheme.surfaceContainer) {
                            AskRow(entry, playing = entry.entry == state.entry, moving = state.playing) { vm.client.playEntry(entry.entry) }
                        }
                    }
                }
            }
        }
        itemsIndexed(queue, key = { _, entry -> entry.entry }) { index, entry ->
            Column {
                if (index == 0 || queue[index - 1].albumGroup != entry.albumGroup) AlbumHeader(entry)
                Swipeable(onRemove = { vm.client.removeFromQueue(entry.entry) }) {
                    Box(Modifier.padding(horizontal = 20.dp)) {
                        TrackRow(
                            number = numberInAlbum(queue, index),
                            title = entry.title,
                            subtitle = if (entry.artist != entry.albumArtist) entry.artist else null,
                            durationMs = entry.durationMs,
                            playing = entry.entry == state.entry,
                            moving = state.playing,
                            dimmed = playingRow >= 0 && index < playingRow,
                            onClick = { vm.client.playEntry(entry.entry) },
                        )
                    }
                }
            }
        }
    }
}

/** A row's number within its album group, as it shows in the album. */
private fun numberInAlbum(queue: List<QueueEntry>, index: Int): Int {
    var start = index
    while (start > 0 && queue[start - 1].albumGroup == queue[index].albumGroup) start--
    return index - start + 1
}

private fun albumsBefore(queue: List<QueueEntry>, row: Int): Int =
    (1..row).count { queue[it - 1].albumGroup != queue[it].albumGroup } + 1

@Composable
private fun AlbumHeader(entry: QueueEntry) {
    Row(
        Modifier.fillMaxWidth().padding(start = 20.dp, end = 20.dp, top = 18.dp, bottom = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Cover(CoverKey(path = entry.path, group = entry.albumGroup), 44.dp)
        Column {
            Text(entry.album.ifEmpty { "—" }, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.ExtraBold,
                maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(listOf(entry.albumArtist, entry.date.take(4)).filter { it.isNotEmpty() }.joinToString(" · "),
                style = MaterialTheme.typography.bodySmall, color = LocalTones.current.muted, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
    }
}

@Composable
private fun AskRow(entry: QueueEntry, playing: Boolean, moving: Boolean, onPlay: () -> Unit) {
    val tones = LocalTones.current
    Row(
        Modifier
            .fillMaxWidth()
            .height(52.dp)
            .combinedClickableCompat(onClick = onPlay)
            .padding(horizontal = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Box(contentAlignment = Alignment.Center) {
            Cover(CoverKey(path = entry.path, group = entry.albumGroup), 36.dp, corner = 6.dp)
            if (playing) Box(Modifier.size(36.dp).clip(RoundedCornerShape(6.dp)).background(MaterialTheme.colorScheme.scrim.copy(alpha = 0.45f)), contentAlignment = Alignment.Center) {
                PlayingBars(moving)
            }
        }
        Column(Modifier.weight(1f)) {
            Text(entry.title, style = MaterialTheme.typography.bodyMedium, maxLines = 1, overflow = TextOverflow.Ellipsis,
                color = if (playing) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurface)
            Text(entry.artist, style = MaterialTheme.typography.bodySmall, color = tones.muted, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        Text(formatTime(entry.durationMs), style = MaterialTheme.typography.bodySmall, color = tones.muted)
    }
}

/** A row that goes when swiped away, showing what that does as it moves. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun Swipeable(
    onRemove: () -> Unit,
    background: androidx.compose.ui.graphics.Color = MaterialTheme.colorScheme.surface,
    content: @Composable () -> Unit,
) {
    val swipe = rememberSwipeToDismissBoxState(confirmValueChange = { value ->
        if (value != SwipeToDismissBoxValue.Settled) onRemove()
        value != SwipeToDismissBoxValue.Settled
    })
    SwipeToDismissBox(
        state = swipe,
        backgroundContent = {
            Row(
                Modifier.fillMaxSize().background(MaterialTheme.colorScheme.errorContainer).padding(horizontal = 22.dp),
                horizontalArrangement = Arrangement.End,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(Icons.Outlined.Delete, null, tint = MaterialTheme.colorScheme.onErrorContainer, modifier = Modifier.size(18.dp))
                Spacer(Modifier.width(6.dp))
                Text("Remove", style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onErrorContainer)
            }
        },
        enableDismissFromStartToEnd = false,
    ) {
        Box(Modifier.background(background)) { content() }
    }
}
