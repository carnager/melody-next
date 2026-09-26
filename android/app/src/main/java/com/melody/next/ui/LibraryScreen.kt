package com.melody.next.ui

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.PlaylistAdd
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.melody.next.CoverKey
import com.melody.next.engine.ConnectionState
import com.melody.next.engine.EngineList
import com.melody.next.engine.LibraryEntry

private val topLevels = listOf(LibraryLevel.Artists, LibraryLevel.Latest, LibraryLevel.Lists, LibraryLevel.Offline)

@Composable
fun LibraryScreen(vm: MainViewModel) {
    Column(Modifier.fillMaxSize()) {
        val level = vm.level
        val top = topLevels.indexOf(level)
        if (top >= 0) {
            TextTabs(listOf("Artists", "Newest", "Lists", "On this phone"), top) { vm.showTop(topLevels[it]) }
        }
        Box(Modifier.fillMaxSize()) {
            when {
                level == LibraryLevel.Offline -> OfflineAlbums(vm)
                level is LibraryLevel.OfflineAlbum -> OfflineAlbumPage(vm, level.key)
                vm.libraryError.isNotEmpty() -> Unreachable(vm, vm.libraryError)
                level == LibraryLevel.Lists -> ListsPage(vm)
                level is LibraryLevel.ListPage -> ListPage(vm, level.list)
                vm.loading && vm.entries.isEmpty() -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    CircularProgressIndicator(strokeWidth = 2.dp)
                }
                vm.entries.isEmpty() && !vm.loading -> {
                    val connection by vm.client.connection.collectAsState()
                    if (connection is ConnectionState.Connected) Centered("Nothing here") else Unreachable(vm, null)
                }
                else -> when (level) {
                    LibraryLevel.Artists -> ArtistList(vm)
                    LibraryLevel.Latest -> AlbumGrid(vm, vm.entries, newest = true)
                    is LibraryLevel.Albums -> AlbumGrid(vm, vm.entries, newest = false)
                    is LibraryLevel.Tracks -> AlbumPage(vm, level.album, vm.entries)
                    else -> Unit
                }
            }
        }
    }
}

@Composable
fun Centered(text: String) {
    Box(Modifier.fillMaxSize().padding(32.dp), contentAlignment = Alignment.Center) {
        Text(text, color = LocalTones.current.secondary, style = MaterialTheme.typography.bodyMedium)
    }
}

/** No engine in reach: said, and what still plays offered. */
@Composable
private fun Unreachable(vm: MainViewModel, detail: String?) {
    val kept by vm.app.offline.albums.collectAsState()
    Column(
        Modifier.fillMaxSize().padding(32.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Text("No engine in reach", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(4.dp))
        Text(
            detail ?: if (kept.isEmpty()) "The library shows once the engine can be reached." else "Albums kept on this phone still play.",
            color = LocalTones.current.secondary,
            style = MaterialTheme.typography.bodySmall,
        )
        if (kept.isNotEmpty()) {
            Spacer(Modifier.height(16.dp))
            Button(onClick = { vm.showTop(LibraryLevel.Offline) }) { Text("Albums on this phone") }
        }
    }
}

/** Artists by letter, with the letters down the side to get anywhere fast. */
@Composable
private fun ArtistList(vm: MainViewModel) {
    val list = rememberLazyListState()
    val tones = LocalTones.current
    // Rows and the letter heads between them, and where each letter starts.
    val rows = remember(vm.entries) {
        buildList<Pair<Char?, LibraryEntry?>> {
            var last: Char? = null
            vm.entries.forEach { artist ->
                val letter = sectionLetter(artist.label)
                if (letter != last) {
                    add(letter to null)
                    last = letter
                }
                add(null to artist)
            }
        }
    }
    val sections = remember(rows) { rows.mapIndexedNotNull { index, (letter, _) -> letter?.let { it to index } } }
    Box(Modifier.fillMaxSize()) {
        LazyColumn(state = list, modifier = Modifier.fillMaxSize(), contentPadding = PaddingValues(start = 20.dp, end = 40.dp, bottom = 12.dp)) {
            itemsIndexed(rows, key = { index, (letter, artist) -> artist?.key ?: "letter:$letter:$index" }) { _, (letter, artist) ->
                if (letter != null) {
                    Text(
                        letter.toString(),
                        style = MaterialTheme.typography.labelSmall,
                        fontWeight = FontWeight.ExtraBold,
                        color = MaterialTheme.colorScheme.primary,
                        modifier = Modifier.padding(top = 14.dp, bottom = 4.dp),
                    )
                } else if (artist != null) {
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .height(52.dp)
                            .combinedClickableCompat(onClick = { vm.open(LibraryLevel.Albums(artist)) }),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(14.dp),
                    ) {
                        InitialsTile(artist.label.ifEmpty { "?" })
                        Text(
                            artist.label.ifEmpty { "Unknown artist" },
                            style = MaterialTheme.typography.bodyLarge,
                            maxLines = 1, overflow = TextOverflow.Ellipsis,
                            modifier = Modifier.weight(1f),
                        )
                        Text("${artist.albums}", style = MaterialTheme.typography.bodySmall, color = tones.muted)
                    }
                }
            }
        }
        AlphabetRail(
            sections, list,
            Modifier.align(Alignment.CenterEnd).fillMaxHeight().padding(vertical = 8.dp, horizontal = 2.dp),
        )
    }
}

/** Albums as covers, two across: the newest, or one artist's. */
@Composable
private fun AlbumGrid(vm: MainViewModel, albums: List<LibraryEntry>, newest: Boolean) {
    LazyVerticalGrid(
        columns = GridCells.Adaptive(150.dp),
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(start = 20.dp, end = 20.dp, top = 18.dp, bottom = 16.dp),
        horizontalArrangement = Arrangement.spacedBy(16.dp),
        verticalArrangement = Arrangement.spacedBy(20.dp),
    ) {
        if (!newest) {
            item(span = { GridItemSpan(maxLineSpan) }) {
                Text("${albums.size} albums", style = MaterialTheme.typography.bodySmall, color = LocalTones.current.muted)
            }
        }
        items(albums, key = { it.key }) { album -> AlbumTile(vm, album, newest) }
    }
}

@Composable
private fun AlbumTile(vm: MainViewModel, album: LibraryEntry, newest: Boolean) {
    val tones = LocalTones.current
    Column(
        Modifier.combinedClickableCompat(
            onClick = { vm.open(LibraryLevel.Tracks(album)) },
            onLongClick = { vm.act(Target.Album(album)) },
        ),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        BoxWithConstraints(Modifier.fillMaxWidth()) {
            Cover(CoverKey(albumKey = album.key), maxWidth, corner = 10.dp)
        }
        Column(verticalArrangement = Arrangement.spacedBy(1.dp)) {
            Text(album.album.ifEmpty { album.label }, style = MaterialTheme.typography.titleSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(
                if (newest) album.artist else listOf(album.year, "${album.tracks} tracks").filter { it.isNotEmpty() }.joinToString(" · "),
                style = MaterialTheme.typography.bodySmall, color = tones.secondary, maxLines = 1, overflow = TextOverflow.Ellipsis,
            )
            if (newest) {
                Text(formatAdded(album.added).replaceFirstChar { it.uppercase() }, style = MaterialTheme.typography.bodySmall, color = tones.muted)
            }
        }
    }
}

/** One album as a row: what search lists. */
@Composable
fun AlbumRow(vm: MainViewModel, album: LibraryEntry, showArtist: Boolean, showAdded: Boolean) {
    val tones = LocalTones.current
    Row(
        Modifier
            .fillMaxWidth()
            .combinedClickableCompat(
                onClick = { vm.open(LibraryLevel.Tracks(album)) },
                onLongClick = { vm.act(Target.Album(album)) },
            )
            .padding(horizontal = 20.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        Cover(CoverKey(albumKey = album.key), 52.dp)
        Column(Modifier.weight(1f)) {
            Text(album.album.ifEmpty { album.label }, style = MaterialTheme.typography.titleSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
            val parts = buildList {
                if (showArtist) add(album.artist)
                if (album.year.isNotEmpty()) add(album.year)
                add(if (album.tracks == 1) "1 track" else "${album.tracks} tracks")
                if (showAdded) formatAdded(album.added).takeIf { it.isNotEmpty() }?.let { add(it) }
            }
            Text(parts.joinToString(" · "), style = MaterialTheme.typography.bodySmall, color = tones.secondary, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
    }
}

/** An album: its cover and facts, what to do with it, and its tracks. */
@Composable
private fun AlbumPage(vm: MainViewModel, album: LibraryEntry, tracks: List<LibraryEntry>) {
    val state by vm.client.state.collectAsState()
    val tones = LocalTones.current
    // Artists named per track only when they differ from the album's:
    // a compilation shows them, a band's album does not repeat itself.
    val mixed = tracks.map { it.artist }.distinct().size > 1
    val total = tracks.sumOf { it.durationMs.coerceAtLeast(0) }
    LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(start = 20.dp, end = 20.dp, bottom = 16.dp)) {
        item(key = "head") {
            Column(Modifier.fillMaxWidth().padding(top = 4.dp)) {
                Row(verticalAlignment = Alignment.Bottom, horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                    Box(Modifier.shadow(18.dp, RoundedCornerShape(12.dp))) {
                        Cover(CoverKey(albumKey = album.key), 148.dp, corner = 12.dp)
                    }
                    Column(Modifier.padding(bottom = 4.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        Text(album.album.ifEmpty { album.label }, style = MaterialTheme.typography.headlineSmall, maxLines = 3, overflow = TextOverflow.Ellipsis)
                        Text(album.artist, style = MaterialTheme.typography.bodyMedium, color = tones.secondary)
                        Text(
                            listOfNotNull(album.year.ifEmpty { null }, "${tracks.size} tracks", total.takeIf { it > 0 }?.let(::formatMinutes)).joinToString(" · "),
                            style = MaterialTheme.typography.bodySmall, color = tones.muted,
                        )
                        RatingBar(album.rating, onRate = { vm.rateAlbum(album, it) }, starSize = 16.dp)
                    }
                }
                Row(
                    Modifier.padding(top = 18.dp, bottom = 10.dp),
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Button(
                        onClick = { vm.perform(MainViewModel.Action.Play, Target.Album(album)) },
                        contentPadding = PaddingValues(start = 16.dp, end = 22.dp),
                    ) {
                        Icon(Icons.Default.PlayArrow, null)
                        Spacer(Modifier.width(4.dp))
                        Text("Play", fontWeight = FontWeight.ExtraBold)
                    }
                    FilledTonalButton(
                        onClick = { vm.perform(MainViewModel.Action.UpNext, Target.Album(album)) },
                        contentPadding = PaddingValues(start = 14.dp, end = 18.dp),
                        colors = ButtonDefaults.filledTonalButtonColors(containerColor = tones.raised),
                    ) {
                        Icon(Icons.AutoMirrored.Filled.PlaylistAdd, null)
                        Spacer(Modifier.width(6.dp))
                        Text("Up Next")
                    }
                    Spacer(Modifier.weight(1f))
                    DownloadButton(vm, album)
                    IconButton(onClick = { vm.act(Target.Album(album)) }) { Icon(Icons.Default.MoreVert, "More", tint = tones.secondary) }
                }
            }
        }
        itemsIndexed(tracks, key = { _, track -> track.key }) { index, track ->
            TrackRow(
                number = if (track.trackNumber > 0) track.trackNumber else index + 1,
                title = track.title.ifEmpty { track.label },
                subtitle = if (mixed) track.artist else null,
                durationMs = track.durationMs,
                playing = track.key == state.path,
                moving = state.playing,
                onClick = { vm.playFrom(tracks, index, album.artist) },
                onLongClick = { vm.act(Target.Tracks(listOf(track), album.artist)) },
            )
        }
    }
}

/**
 * A track in a list: number, title, length. Playing, the number becomes the
 * bars and the title takes the accent; nothing else about the row changes.
 */
@Composable
fun TrackRow(
    number: Int,
    title: String,
    subtitle: String?,
    durationMs: Long,
    playing: Boolean,
    moving: Boolean,
    dimmed: Boolean = false,
    onClick: () -> Unit,
    onLongClick: (() -> Unit)? = null,
) {
    val tones = LocalTones.current
    Row(
        Modifier
            .fillMaxWidth()
            .height(if (subtitle != null) 56.dp else 48.dp)
            .combinedClickableCompat(onClick = onClick, onLongClick = onLongClick),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Box(Modifier.width(24.dp), contentAlignment = Alignment.Center) {
            if (playing) PlayingBars(moving)
            else Text("$number", style = MaterialTheme.typography.bodySmall, color = if (dimmed) tones.faint else tones.muted)
        }
        Column(Modifier.weight(1f)) {
            Text(
                title, style = MaterialTheme.typography.bodyMedium, maxLines = 1, overflow = TextOverflow.Ellipsis,
                color = when {
                    playing -> MaterialTheme.colorScheme.primary
                    dimmed -> tones.muted
                    else -> MaterialTheme.colorScheme.onSurface
                },
            )
            if (subtitle != null) Text(subtitle, style = MaterialTheme.typography.bodySmall, color = tones.muted, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        Text(formatTime(durationMs), style = MaterialTheme.typography.bodySmall, color = tones.muted)
    }
}

/** "40 min", "1 h 23 min": how long an album or a queue is. */
fun formatMinutes(ms: Long): String {
    val minutes = (ms + 30_000) / 60_000
    return if (minutes >= 60) "${minutes / 60} h ${minutes % 60} min" else "$minutes min"
}

@OptIn(ExperimentalFoundationApi::class)
fun Modifier.combinedClickableCompat(onClick: () -> Unit, onLongClick: (() -> Unit)? = null): Modifier =
    combinedClickable(onClick = onClick, onLongClick = onLongClick)


/** ADR-0233: the engine's lists -- saved ones first, working ones said to be. */
@Composable
private fun ListsPage(vm: MainViewModel) {
    val tones = LocalTones.current
    if (vm.loading && vm.lists.isEmpty()) {
        Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { CircularProgressIndicator(strokeWidth = 2.dp) }
        return
    }
    if (vm.lists.isEmpty()) {
        Centered("No lists yet. Lists saved in Trackknife show here.")
        return
    }
    LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(start = 20.dp, end = 20.dp, bottom = 12.dp)) {
        itemsIndexed(vm.lists, key = { _, list -> list.id }) { _, list ->
            Column(
                Modifier
                    .fillMaxWidth()
                    .clickable { vm.open(LibraryLevel.ListPage(list)) }
                    .padding(vertical = 10.dp),
            ) {
                Text(list.name, style = MaterialTheme.typography.bodyLarge, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text(
                    listOfNotNull("${list.tracks} tracks", if (list.saved) null else "working").joinToString(" · "),
                    style = MaterialTheme.typography.bodySmall,
                    color = tones.muted,
                )
            }
        }
    }
}

/** One of the engine's lists: played whole, or from a track. */
@Composable
private fun ListPage(vm: MainViewModel, list: EngineList) {
    val state by vm.client.state.collectAsState()
    val tones = LocalTones.current
    val entries = vm.listEntries
    val total = entries.sumOf { it.durationMs.coerceAtLeast(0) }
    LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(start = 20.dp, end = 20.dp, bottom = 16.dp)) {
        item(key = "head") {
            Column(Modifier.fillMaxWidth().padding(top = 4.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(list.name, style = MaterialTheme.typography.headlineSmall, maxLines = 3, overflow = TextOverflow.Ellipsis)
                Text(
                    listOfNotNull("${entries.size} tracks", total.takeIf { it > 0 }?.let(::formatMinutes), if (list.saved) null else "working")
                        .joinToString(" · "),
                    style = MaterialTheme.typography.bodySmall, color = tones.muted,
                )
                Row(Modifier.padding(top = 14.dp, bottom = 10.dp)) {
                    Button(
                        onClick = { vm.client.playList(list.id) },
                        enabled = entries.isNotEmpty(),
                        contentPadding = PaddingValues(start = 16.dp, end = 22.dp),
                    ) {
                        Icon(Icons.Default.PlayArrow, null)
                        Spacer(Modifier.width(4.dp))
                        Text("Play", fontWeight = FontWeight.ExtraBold)
                    }
                }
            }
        }
        itemsIndexed(entries, key = { _, entry -> entry.entry }) { index, entry ->
            TrackRow(
                number = index + 1,
                title = entry.title,
                subtitle = listOf(entry.artist, entry.album).filter { it.isNotEmpty() }.joinToString(" · ").ifEmpty { null },
                durationMs = entry.durationMs,
                playing = entry.path == state.path,
                moving = state.playing,
                onClick = { vm.client.playList(list.id, entry.entry) },
                onLongClick = {},
            )
        }
    }
}
