package com.melody.next.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.GraphicEq
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.melody.next.CoverKey
import com.melody.next.engine.QueueEntry

/**
 * The engine's queue and Up Next. Up Next plays before the queue goes on;
 * the queue is shown by album, as Trackknife groups it.
 */
@Composable
fun QueueScreen(vm: MainViewModel, onBrowse: () -> Unit) {
    val queue by vm.client.queue.collectAsState()
    val upNext by vm.client.upNext.collectAsState()
    val state by vm.client.state.collectAsState()
    if (queue.isEmpty() && upNext.isEmpty()) {
        Column(Modifier.fillMaxSize().padding(32.dp), horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = androidx.compose.foundation.layout.Arrangement.Center) {
            Text("The queue is empty", color = MaterialTheme.colorScheme.onSurfaceVariant)
            Spacer(Modifier.width(8.dp))
            Button(onClick = onBrowse) { Text("Browse the library") }
        }
        return
    }
    val list = rememberLazyListState()
    val playingRow = queue.indexOfFirst { it.entry == state.entry }
    // Opened on what plays, not on the top of a long queue.
    LaunchedEffect(Unit) {
        if (playingRow > 0) list.scrollToItem(playingRow + upNext.size + if (upNext.isEmpty()) 0 else 2)
    }
    LazyColumn(state = list, modifier = Modifier.fillMaxSize()) {
        if (upNext.isNotEmpty()) {
            item(key = "up-next-header") {
                Row(Modifier.fillMaxWidth().padding(start = 16.dp, end = 8.dp, top = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                    Section("Up Next")
                    Spacer(Modifier.weight(1f))
                    TextButton(onClick = vm.client::clearRequests) { Text("Clear") }
                }
            }
            itemsIndexed(upNext, key = { _, entry -> "ask:" + entry.entry }) { _, entry ->
                EntryRow(entry, playing = entry.entry == state.entry, showCover = true,
                    onPlay = { vm.client.playEntry(entry.entry) },
                    onRemove = { vm.client.removeRequest(entry.entry) })
            }
            item(key = "queue-header") { Section("Queue") }
        }
        itemsIndexed(queue, key = { _, entry -> entry.entry }) { index, entry ->
            val startsAlbum = index == 0 || queue[index - 1].albumGroup != entry.albumGroup
            if (startsAlbum) AlbumHeader(entry)
            EntryRow(entry, playing = entry.entry == state.entry, showCover = false,
                onPlay = { vm.client.playEntry(entry.entry) },
                onRemove = { vm.client.removeFromQueue(entry.entry) })
        }
    }
}

@Composable
private fun AlbumHeader(entry: QueueEntry) {
    Row(
        Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, top = 14.dp, bottom = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Cover(CoverKey(path = entry.path, group = entry.albumGroup), 40.dp, corner = 6.dp)
        Spacer(Modifier.width(12.dp))
        Column {
            Text(entry.album.ifEmpty { "—" }, fontWeight = FontWeight.Bold, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(listOf(entry.albumArtist, entry.date.take(4)).filter { it.isNotEmpty() }.joinToString(" · "),
                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
    }
}

@Composable
private fun EntryRow(entry: QueueEntry, playing: Boolean, showCover: Boolean, onPlay: () -> Unit, onRemove: () -> Unit) {
    ListItem(
        leadingContent = when {
            showCover -> { { Cover(CoverKey(path = entry.path, group = entry.albumGroup), 40.dp) } }
            playing -> { { Icon(Icons.Default.GraphicEq, "Playing", tint = MaterialTheme.colorScheme.primary) } }
            else -> null
        },
        headlineContent = {
            Text(entry.title, maxLines = 1, overflow = TextOverflow.Ellipsis,
                fontWeight = if (playing) FontWeight.Bold else FontWeight.Normal,
                color = if (playing) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurface)
        },
        supportingContent = if (showCover || entry.artist != entry.albumArtist) {
            { Text(entry.artist, maxLines = 1, overflow = TextOverflow.Ellipsis) }
        } else null,
        trailingContent = {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(formatTime(entry.durationMs), style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                IconButton(onClick = onRemove) { Icon(Icons.Default.Close, "Remove") }
            }
        },
        colors = ListItemDefaults.colors(
            containerColor = if (playing) MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.25f)
            else MaterialTheme.colorScheme.surface,
        ),
        modifier = Modifier.combinedClickableCompat(onClick = onPlay).background(MaterialTheme.colorScheme.surface),
    )
}
