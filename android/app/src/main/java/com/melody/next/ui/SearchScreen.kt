package com.melody.next.ui

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Clear
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.melody.next.CoverKey

/**
 * Words against albums (artist, title, year) and tracks (title, artists,
 * album, year) -- the quick pick's matching. A tap on an album opens it; on
 * a track, plays it with its album around it.
 */
@Composable
fun SearchScreen(vm: MainViewModel) {
    val focus = LocalFocusManager.current
    Column(Modifier.fillMaxSize()) {
        OutlinedTextField(
            value = vm.searchText,
            onValueChange = vm::search,
            placeholder = { Text("Albums, artists, tracks, years") },
            leadingIcon = { Icon(Icons.Default.Search, null) },
            trailingIcon = {
                if (vm.searchText.isNotEmpty()) IconButton(onClick = { vm.search("") }) { Icon(Icons.Default.Clear, "Clear") }
            },
            singleLine = true,
            keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
            keyboardActions = KeyboardActions(onSearch = { focus.clearFocus() }),
            modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
        )
        if (vm.searching) LinearProgressIndicator(Modifier.fillMaxWidth())
        LazyColumn(Modifier.fillMaxSize()) {
            if (vm.foundAlbums.isNotEmpty()) {
                item { Section("Albums") }
                items(vm.foundAlbums, key = { "album:" + it.key }) { album ->
                    AlbumRow(vm, album, showArtist = true, showAdded = false)
                }
            }
            if (vm.foundTracks.isNotEmpty()) {
                item { Section("Tracks") }
                items(vm.foundTracks, key = { "track:" + it.key }) { track ->
                    ListItem(
                        leadingContent = { Cover(CoverKey(path = track.key, group = "${track.artist}\u0000${track.album}\u0000${track.date}"), 48.dp) },
                        headlineContent = { Text(track.title.ifEmpty { track.label }, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                        supportingContent = {
                            Text(listOf(track.artist, track.album, track.year).filter { it.isNotEmpty() }.joinToString(" · "),
                                maxLines = 1, overflow = TextOverflow.Ellipsis, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        },
                        trailingContent = {
                            IconButton(onClick = { vm.act(Target.Tracks(listOf(track))) }) { Icon(Icons.Default.MoreVert, "More") }
                        },
                        modifier = Modifier.combinedClickableCompat(
                            onClick = { vm.perform(MainViewModel.Action.Play, Target.Tracks(listOf(track))) },
                            onLongClick = { vm.act(Target.Tracks(listOf(track))) },
                        ),
                    )
                }
            }
            if (vm.searchText.isNotBlank() && !vm.searching && vm.foundAlbums.isEmpty() && vm.foundTracks.isEmpty()) {
                item { Centered("Nothing found for “${vm.searchText}”") }
            }
        }
    }
}

@Composable
fun Section(title: String) {
    Text(
        title, style = MaterialTheme.typography.labelLarge, fontWeight = FontWeight.Bold,
        color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(start = 16.dp, top = 16.dp, bottom = 4.dp),
    )
}
