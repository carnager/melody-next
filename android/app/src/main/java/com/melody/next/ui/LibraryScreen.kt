package com.melody.next.ui

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.PlaylistAdd
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.melody.next.CoverKey
import com.melody.next.engine.LibraryEntry

@Composable
fun LibraryScreen(vm: MainViewModel) {
    Column(Modifier.fillMaxSize()) {
        val level = vm.level
        if (level == LibraryLevel.Artists || level == LibraryLevel.Latest || level == LibraryLevel.Offline) {
            Row(Modifier.padding(horizontal = 16.dp, vertical = 4.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilterChip(selected = level == LibraryLevel.Artists, onClick = { vm.showTop(LibraryLevel.Artists) }, label = { Text("Artists") })
                FilterChip(selected = level == LibraryLevel.Latest, onClick = { vm.showTop(LibraryLevel.Latest) }, label = { Text("Newest") })
                FilterChip(selected = level == LibraryLevel.Offline, onClick = { vm.showTop(LibraryLevel.Offline) }, label = { Text("On this phone") })
            }
        }
        Box(Modifier.fillMaxSize()) {
            when {
                level == LibraryLevel.Offline -> OfflineAlbums(vm)
                level is LibraryLevel.OfflineAlbum -> OfflineAlbumPage(vm, level.key)
                vm.libraryError.isNotEmpty() -> Column(
                    Modifier.fillMaxSize().padding(32.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.Center,
                ) {
                    Text(vm.libraryError, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    // No engine in reach: what is kept here still plays.
                    if (vm.app.offline.albums.collectAsState().value.isNotEmpty()) {
                        Spacer(Modifier.height(12.dp))
                        Button(onClick = { vm.showTop(LibraryLevel.Offline) }) { Text("Albums on this phone") }
                    }
                }
                vm.loading && vm.entries.isEmpty() -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    CircularProgressIndicator()
                }
                vm.entries.isEmpty() && !vm.loading -> Centered("Nothing here")
                else -> when (level) {
                    LibraryLevel.Artists -> ArtistList(vm)
                    LibraryLevel.Latest -> AlbumList(vm, vm.entries, newest = true)
                    is LibraryLevel.Albums -> AlbumList(vm, vm.entries, newest = false)
                    is LibraryLevel.Tracks -> AlbumTracks(vm, level.album, vm.entries)
                    else -> Unit
                }
            }
        }
    }
}

@Composable
fun Centered(text: String) {
    Box(Modifier.fillMaxSize().padding(32.dp), contentAlignment = Alignment.Center) {
        Text(text, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun ArtistList(vm: MainViewModel) {
    LazyColumn(Modifier.fillMaxSize()) {
        itemsIndexed(vm.entries, key = { _, artist -> artist.key }) { _, artist ->
            ListItem(
                headlineContent = { Text(artist.label.ifEmpty { "Unknown artist" }, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                supportingContent = {
                    Text(if (artist.albums == 1) "1 album" else "${artist.albums} albums",
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                },
                modifier = Modifier.combinedClickableCompat(onClick = { vm.open(LibraryLevel.Albums(artist)) }),
            )
        }
    }
}

@Composable
fun AlbumList(vm: MainViewModel, albums: List<LibraryEntry>, newest: Boolean) {
    LazyColumn(Modifier.fillMaxSize()) {
        itemsIndexed(albums, key = { _, album -> album.key }) { _, album ->
            AlbumRow(vm, album, showArtist = newest, showAdded = newest)
        }
    }
}

@Composable
fun AlbumRow(vm: MainViewModel, album: LibraryEntry, showArtist: Boolean, showAdded: Boolean) {
    ListItem(
        leadingContent = { Cover(CoverKey(albumKey = album.key), 56.dp) },
        headlineContent = {
            Text(album.album.ifEmpty { album.label }, maxLines = 1, overflow = TextOverflow.Ellipsis, fontWeight = FontWeight.Medium)
        },
        supportingContent = {
            val parts = buildList {
                if (showArtist) add(album.artist)
                if (album.year.isNotEmpty()) add(album.year)
                add(if (album.tracks == 1) "1 track" else "${album.tracks} tracks")
                if (showAdded) formatAdded(album.added).takeIf { it.isNotEmpty() }?.let { add("added $it") }
            }
            Text(parts.joinToString(" · "), maxLines = 1, overflow = TextOverflow.Ellipsis,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        },
        trailingContent = {
            IconButton(onClick = { vm.act(Target.Album(album)) }) { Icon(Icons.Default.MoreVert, "More") }
        },
        modifier = Modifier.combinedClickableCompat(
            onClick = { vm.open(LibraryLevel.Tracks(album)) },
            onLongClick = { vm.act(Target.Album(album)) },
        ),
    )
}

@Composable
private fun AlbumTracks(vm: MainViewModel, album: LibraryEntry, tracks: List<LibraryEntry>) {
    val state by vm.client.state.collectAsState()
    val playingPath = state.path
    // Artists named per track only when they differ from the album's:
    // a compilation shows them, a band's album does not repeat itself.
    val mixed = tracks.map { it.artist }.distinct().size > 1
    LazyColumn(Modifier.fillMaxSize()) {
        item(key = "header") {
            Column(Modifier.fillMaxWidth().padding(16.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Cover(CoverKey(albumKey = album.key), 128.dp, corner = 12.dp)
                    Spacer(Modifier.width(16.dp))
                    Column(Modifier.weight(1f)) {
                        Text(album.album.ifEmpty { album.label }, style = MaterialTheme.typography.titleLarge,
                            fontWeight = FontWeight.Bold, maxLines = 3, overflow = TextOverflow.Ellipsis)
                        Text(album.artist, style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        val total = tracks.sumOf { it.durationMs.coerceAtLeast(0) }
                        Text(
                            listOfNotNull(album.year.ifEmpty { null }, "${tracks.size} tracks",
                                total.takeIf { it > 0 }?.let(::formatTime)).joinToString(" · "),
                            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        RatingBar(album.rating, onRate = { vm.rateAlbum(album, it) }, starSize = 20.dp)
                    }
                }
                Spacer(Modifier.height(12.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(onClick = { vm.perform(MainViewModel.Action.Play, Target.Album(album)) }) {
                        Icon(Icons.Default.PlayArrow, null)
                        Spacer(Modifier.width(4.dp))
                        Text("Play")
                    }
                    OutlinedButton(onClick = { vm.perform(MainViewModel.Action.UpNext, Target.Album(album)) }) {
                        Icon(Icons.AutoMirrored.Filled.PlaylistAdd, null)
                        Spacer(Modifier.width(4.dp))
                        Text("Up Next")
                    }
                    DownloadButton(vm, album)
                    IconButton(onClick = { vm.act(Target.Album(album)) }) { Icon(Icons.Default.MoreVert, "More") }
                }
            }
            HorizontalDivider()
        }
        itemsIndexed(tracks, key = { _, track -> track.key }) { index, track ->
            val playing = track.key == playingPath
            ListItem(
                leadingContent = {
                    Text(
                        if (track.trackNumber > 0) "${track.trackNumber}" else "",
                        modifier = Modifier.width(24.dp),
                        color = if (playing) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                },
                headlineContent = {
                    Text(track.title.ifEmpty { track.label }, maxLines = 1, overflow = TextOverflow.Ellipsis,
                        color = if (playing) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurface,
                        fontWeight = if (playing) FontWeight.Bold else FontWeight.Normal)
                },
                supportingContent = if (mixed) {
                    { Text(track.artist, maxLines = 1, overflow = TextOverflow.Ellipsis) }
                } else null,
                trailingContent = {
                    Text(formatTime(track.durationMs), style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                },
                modifier = Modifier.combinedClickableCompat(
                    onClick = { vm.playFrom(tracks, index, album.artist) },
                    onLongClick = { vm.act(Target.Tracks(listOf(track), album.artist)) },
                ),
            )
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
fun Modifier.combinedClickableCompat(onClick: () -> Unit, onLongClick: (() -> Unit)? = null): Modifier =
    combinedClickable(onClick = onClick, onLongClick = onLongClick)
