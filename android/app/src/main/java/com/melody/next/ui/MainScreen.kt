package com.melody.next.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material3.NavigationBarItemDefaults
import androidx.compose.ui.Alignment
import androidx.compose.ui.unit.dp
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
    val upNext by vm.client.upNext.collectAsState()
    val tones = LocalTones.current
    val drilling = tab == 0 && vm.levels.size > 1

    Box(Modifier.fillMaxSize()) {
        Scaffold(
            topBar = {
                Column(Modifier.statusBarsPadding()) {
                    if (drilling) {
                        // Inside the library: back, and the artist's name
                        // when it is their albums; an album shows its own.
                        Row(Modifier.fillMaxWidth().padding(start = 4.dp, end = 8.dp, top = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                            IconButton(onClick = { vm.back() }) { Icon(Icons.AutoMirrored.Filled.ArrowBack, "Back") }
                            val level = vm.level
                            if (level is LibraryLevel.Albums) {
                                Text(level.artist.label, style = MaterialTheme.typography.titleLarge, maxLines = 1, overflow = TextOverflow.Ellipsis,
                                    modifier = Modifier.weight(1f).padding(start = 4.dp))
                            } else {
                                Spacer(Modifier.weight(1f))
                            }
                        }
                    } else {
                        val all = queue.size + upNext.size
                        ScreenHeader(
                            title = when (tab) { 0 -> "Library"; 1 -> "Search"; else -> "Queue" },
                            subtitle = when (tab) {
                                0 -> engineName
                                2 -> if (all == 0) null else "$all tracks · ${formatMinutes((queue + upNext).sumOf { it.durationMs.coerceAtLeast(0) })}"
                                else -> null
                            },
                        ) {
                            if (tab == 2 && queue.isNotEmpty()) {
                                IconButton(onClick = { clearing = true }) { Icon(Icons.Default.DeleteSweep, "Clear the queue", tint = tones.secondary) }
                            }
                            IconButton(onClick = { outputs = true }) { Icon(Icons.Default.Speaker, "Where it plays", tint = tones.secondary) }
                            IconButton(onClick = { settings = true }) { Icon(Icons.Default.Tune, "Settings", tint = tones.secondary) }
                        }
                    }
                }
            },
            bottomBar = {
                Column {
                    ConnectionBanner(connection)
                    MiniPlayer(vm) { nowPlaying = true }
                    Box(Modifier.fillMaxWidth().height(1.dp).background(MaterialTheme.colorScheme.outlineVariant))
                    NavigationBar(containerColor = MaterialTheme.colorScheme.surface, tonalElevation = 0.dp) {
                        val colors = NavigationBarItemDefaults.colors(
                            selectedIconColor = MaterialTheme.colorScheme.onSurface,
                            selectedTextColor = MaterialTheme.colorScheme.onSurface,
                            unselectedIconColor = tones.muted,
                            unselectedTextColor = tones.muted,
                            indicatorColor = androidx.compose.ui.graphics.Color.Transparent,
                        )
                        NavigationBarItem(
                            selected = tab == 0,
                            onClick = { if (tab == 0) vm.showTop(vm.levels.first()) else tab = 0 },
                            icon = { Icon(Icons.Default.LibraryMusic, null) },
                            label = { Text("Library", style = MaterialTheme.typography.labelMedium) },
                            colors = colors,
                        )
                        NavigationBarItem(
                            selected = tab == 1, onClick = { tab = 1 },
                            icon = { Icon(Icons.Default.Search, null) },
                            label = { Text("Search", style = MaterialTheme.typography.labelMedium) },
                            colors = colors,
                        )
                        NavigationBarItem(
                            selected = tab == 2, onClick = { tab = 2 },
                            icon = { Icon(Icons.AutoMirrored.Filled.QueueMusic, null) },
                            label = { Text("Queue", style = MaterialTheme.typography.labelMedium) },
                            colors = colors,
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
