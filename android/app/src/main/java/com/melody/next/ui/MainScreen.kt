package com.melody.next.ui

import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.consumeWindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.QueueMusic
import androidx.compose.material.icons.filled.DeleteSweep
import androidx.compose.material.icons.filled.LibraryMusic
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Speaker
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import com.melody.next.engine.ConnectionState

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MainScreen(vm: MainViewModel, onChangeEngine: () -> Unit) {
    var tab by rememberSaveable { mutableIntStateOf(0) }
    var nowPlaying by rememberSaveable { mutableStateOf(false) }
    var settings by rememberSaveable { mutableStateOf(false) }
    var outputs by remember { mutableStateOf(false) }
    var clearing by remember { mutableStateOf(false) }
    val connection by vm.client.connection.collectAsState()
    val queue by vm.client.queue.collectAsState()
    val snackbar = remember { SnackbarHostState() }

    // Back leaves a drill-down first, then goes home to the library.
    BackHandler(enabled = tab != 0) { tab = 0 }
    BackHandler(enabled = tab == 0 && vm.levels.size > 1) { vm.back() }

    LaunchedEffect(vm.message) {
        vm.message?.let {
            snackbar.showSnackbar(it)
            vm.messageShown()
        }
    }

    val engineName = (connection as? ConnectionState.Connected)?.name
    val title = when (tab) {
        0 -> when (val level = vm.level) {
            LibraryLevel.Artists -> engineName ?: "Library"
            LibraryLevel.Latest -> "Newest"
            is LibraryLevel.Albums -> level.artist.label
            is LibraryLevel.Tracks -> level.album.album.ifEmpty { level.album.label }
            LibraryLevel.Offline -> "On this phone"
            is LibraryLevel.OfflineAlbum -> vm.app.offline.album(level.key)?.album?.album ?: "On this phone"
        }
        1 -> "Search"
        else -> "Queue"
    }

    Box(Modifier.fillMaxSize()) {
        Scaffold(
            topBar = {
                TopAppBar(
                    title = { Text(title, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                    navigationIcon = {
                        if (tab == 0 && vm.levels.size > 1) {
                            IconButton(onClick = { vm.back() }) { Icon(Icons.AutoMirrored.Filled.ArrowBack, "Back") }
                        }
                    },
                    actions = {
                        if (tab == 2 && queue.isNotEmpty()) {
                            IconButton(onClick = { clearing = true }) { Icon(Icons.Default.DeleteSweep, "Clear the queue") }
                        }
                        IconButton(onClick = { outputs = true }) { Icon(Icons.Default.Speaker, "Where it plays") }
                        IconButton(onClick = { settings = true }) { Icon(Icons.Default.Settings, "Settings") }
                    },
                    colors = TopAppBarDefaults.topAppBarColors(containerColor = MaterialTheme.colorScheme.surface),
                )
            },
            bottomBar = {
                Column {
                    ConnectionBanner(connection)
                    MiniPlayer(vm) { nowPlaying = true }
                    NavigationBar(containerColor = MaterialTheme.colorScheme.surface) {
                        NavigationBarItem(
                            selected = tab == 0,
                            onClick = { if (tab == 0) vm.showLatest(vm.level == LibraryLevel.Latest) else tab = 0 },
                            icon = { Icon(Icons.Default.LibraryMusic, null) },
                            label = { Text("Library") },
                        )
                        NavigationBarItem(
                            selected = tab == 1, onClick = { tab = 1 },
                            icon = { Icon(Icons.Default.Search, null) }, label = { Text("Search") },
                        )
                        NavigationBarItem(
                            selected = tab == 2, onClick = { tab = 2 },
                            icon = { Icon(Icons.AutoMirrored.Filled.QueueMusic, null) }, label = { Text("Queue") },
                        )
                    }
                }
            },
            snackbarHost = { SnackbarHost(snackbar) },
            // The lists' own colour, so headers and rows sit on one ground.
            containerColor = MaterialTheme.colorScheme.surface,
        ) { padding ->
            Box(Modifier.padding(padding).consumeWindowInsets(padding)) {
                when (tab) {
                    0 -> LibraryScreen(vm)
                    1 -> SearchScreen(vm)
                    else -> QueueScreen(vm, onBrowse = { tab = 0 })
                }
            }
        }

        if (clearing) {
            AlertDialog(
                onDismissRequest = { clearing = false },
                title = { Text("Clear the queue?") },
                text = { Text("All ${queue.size} tracks are taken out and playback stops.") },
                confirmButton = {
                    TextButton(onClick = { clearing = false; vm.client.clearQueue() }) {
                        Text("Clear", color = MaterialTheme.colorScheme.error)
                    }
                },
                dismissButton = { TextButton(onClick = { clearing = false }) { Text("Cancel") } },
            )
        }
        vm.acting?.let { ActionSheet(vm, it) }
        if (outputs) OutputsSheet(vm) { outputs = false }

        AnimatedVisibility(nowPlaying, enter = slideInVertically { it }, exit = slideOutVertically { it }) {
            NowPlayingScreen(vm, onOutputs = { outputs = true }) { nowPlaying = false }
        }
        AnimatedVisibility(settings, enter = slideInVertically { it }, exit = slideOutVertically { it }) {
            SettingsScreen(vm, onChangeEngine = { settings = false; onChangeEngine() }) { settings = false }
        }
    }
}
