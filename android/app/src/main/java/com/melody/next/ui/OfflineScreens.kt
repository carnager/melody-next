package com.melody.next.ui

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Album
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Download
import androidx.compose.material.icons.filled.DownloadDone
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.melody.next.engine.ConnectionState
import com.melody.next.engine.LibraryEntry
import com.melody.next.offline.OfflineAlbum
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/** A kept album's cover, from its folder: no engine needed to show it. */
@Composable
fun KeptCover(album: OfflineAlbum, size: Dp, corner: Dp = 8.dp) {
    val image by produceState<ImageBitmap?>(null, album.cover) {
        value = withContext(Dispatchers.IO) {
            runCatching { android.graphics.BitmapFactory.decodeFile(album.cover.path)?.asImageBitmap() }.getOrNull()
        }
    }
    Box(
        Modifier.size(size).clip(RoundedCornerShape(corner)).background(MaterialTheme.colorScheme.surfaceVariant),
        contentAlignment = Alignment.Center,
    ) {
        val shown = image
        if (shown != null) Image(shown, null, contentScale = ContentScale.Crop, modifier = Modifier.fillMaxSize())
        else Icon(Icons.Default.Album, null, tint = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.5f), modifier = Modifier.size(size * 0.45f))
    }
}

@Composable
fun OfflineAlbums(vm: MainViewModel) {
    val albums by vm.app.offline.albums.collectAsState()
    val progress by vm.app.offline.progress.collectAsState()
    if (albums.isEmpty()) {
        Centered("Nothing kept on this phone yet. An album's download button keeps it here for listening without the engine.")
        return
    }
    LazyColumn(Modifier.fillMaxSize()) {
        items(albums, key = { it.album.key }) { album ->
            val going = progress[album.album.key]
            ListItem(
                leadingContent = { KeptCover(album, 56.dp) },
                headlineContent = { Text(album.album.album.ifEmpty { album.album.label }, maxLines = 1, overflow = TextOverflow.Ellipsis, fontWeight = FontWeight.Medium) },
                supportingContent = {
                    Text(
                        listOfNotNull(
                            album.album.artist, album.album.year.ifEmpty { null },
                            when {
                                going != null -> "downloading ${going.done}/${going.total}"
                                !album.complete -> "waiting to download"
                                album.bitrateKbps > 0 -> "Opus ${album.bitrateKbps}"
                                else -> "original files"
                            },
                        ).joinToString(" · "),
                        maxLines = 1, overflow = TextOverflow.Ellipsis, color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                },
                modifier = Modifier.combinedClickableCompat(onClick = { vm.open(LibraryLevel.OfflineAlbum(album.album.key)) }),
            )
        }
    }
}

@Composable
fun OfflineAlbumPage(vm: MainViewModel, key: String) {
    val albums by vm.app.offline.albums.collectAsState()
    val album = albums.firstOrNull { it.album.key == key } ?: run {
        Centered("No longer kept on this phone")
        return
    }
    val playing by vm.app.offlinePlayer.state.collectAsState()
    var removing by remember { mutableStateOf(false) }
    LazyColumn(Modifier.fillMaxSize()) {
        item {
            Column(Modifier.fillMaxWidth().padding(16.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    KeptCover(album, 128.dp, corner = 12.dp)
                    Spacer(Modifier.width(16.dp))
                    Column(Modifier.weight(1f)) {
                        Text(album.album.album, style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold, maxLines = 3, overflow = TextOverflow.Ellipsis)
                        Text(album.album.artist, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        Text(
                            listOfNotNull(album.album.year.ifEmpty { null }, "${album.tracks.size} tracks",
                                if (album.bitrateKbps > 0) "Opus ${album.bitrateKbps}" else "original files").joinToString(" · "),
                            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
                Spacer(Modifier.height(12.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                    Button(onClick = { vm.app.offlinePlayer.play(album) }, enabled = album.tracks.any { it.file.isFile }) {
                        Icon(Icons.Default.PlayArrow, null)
                        Spacer(Modifier.width(4.dp))
                        Text("Play on this phone")
                    }
                    IconButton(onClick = { removing = true }) { Icon(Icons.Default.Delete, "Remove from this phone") }
                }
            }
            HorizontalDivider()
        }
        itemsIndexed(album.tracks, key = { _, track -> track.entry.key }) { index, track ->
            val current = playing.album?.album?.key == key && playing.track?.entry?.key == track.entry.key
            ListItem(
                leadingContent = { Text("${index + 1}", modifier = Modifier.width(24.dp), color = MaterialTheme.colorScheme.onSurfaceVariant) },
                headlineContent = {
                    Text(track.entry.title.ifEmpty { track.entry.label }, maxLines = 1, overflow = TextOverflow.Ellipsis,
                        color = if (current) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurface,
                        fontWeight = if (current) FontWeight.Bold else FontWeight.Normal)
                },
                trailingContent = {
                    if (track.file.isFile) Text(formatTime(track.entry.durationMs), style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    else CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
                },
                modifier = Modifier.combinedClickableCompat(onClick = {
                    val kept = album.tracks.filter { it.file.isFile }
                    vm.app.offlinePlayer.play(album, kept.indexOfFirst { it.entry.key == track.entry.key }.coerceAtLeast(0))
                }),
            )
        }
    }
    if (removing) {
        AlertDialog(
            onDismissRequest = { removing = false },
            title = { Text("Remove from this phone?") },
            text = { Text("${album.album.album} stays in the library; only the copy here goes.") },
            confirmButton = {
                TextButton(onClick = {
                    removing = false
                    vm.app.offline.remove(key)
                    vm.back()
                }) { Text("Remove", color = MaterialTheme.colorScheme.error) }
            },
            dismissButton = { TextButton(onClick = { removing = false }) { Text("Cancel") } },
        )
    }
}

/** Keep this album on the phone, see how far that is, or that it is. */
@Composable
fun DownloadButton(vm: MainViewModel, album: LibraryEntry) {
    val albums by vm.app.offline.albums.collectAsState()
    val progress by vm.app.offline.progress.collectAsState()
    val connection by vm.client.connection.collectAsState()
    val kept = albums.firstOrNull { it.album.key == album.key }
    val going = progress[album.key]
    when {
        going != null -> Box(contentAlignment = Alignment.Center, modifier = Modifier.size(48.dp)) {
            CircularProgressIndicator(progress = { going.done.toFloat() / going.total.coerceAtLeast(1) }, modifier = Modifier.size(28.dp), strokeWidth = 3.dp)
        }
        kept != null && kept.complete -> IconButton(onClick = { vm.open(LibraryLevel.OfflineAlbum(album.key)) }) {
            Icon(Icons.Default.DownloadDone, "Kept on this phone", tint = MaterialTheme.colorScheme.primary)
        }
        kept != null -> IconButton(onClick = {}) { Icon(Icons.Default.Download, "Waiting to download", tint = MaterialTheme.colorScheme.onSurfaceVariant) }
        else -> IconButton(onClick = {
            val engine = (connection as? ConnectionState.Connected)?.name ?: return@IconButton
            vm.app.offline.download(engine, album)
        }) { Icon(Icons.Default.Download, "Keep on this phone") }
    }
}
