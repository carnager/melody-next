package com.melody.next.ui

import android.os.Build
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.melody.next.engine.ConnectionState

@Composable
fun SettingsScreen(vm: MainViewModel, onChangeEngine: () -> Unit, onClose: () -> Unit) {
    BackHandler(onBack = onClose)
    val settings = vm.app.settings
    val connection by vm.client.connection.collectAsState()
    Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
        Column(Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding()) {
            Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(8.dp)) {
                IconButton(onClick = onClose) { Icon(Icons.Default.KeyboardArrowDown, "Close") }
                Text("Settings", style = MaterialTheme.typography.titleLarge)
            }
            Section("Engine")
            ListItem(
                headlineContent = {
                    Text(when (val state = connection) {
                        is ConnectionState.Connected -> state.name
                        else -> settings.endpoint?.toString() ?: "None"
                    })
                },
                supportingContent = {
                    Text(when (val state = connection) {
                        is ConnectionState.Connected -> "Connected · ${state.endpoint}"
                        is ConnectionState.Connecting -> "Connecting…"
                        is ConnectionState.Refused -> state.reason
                        ConnectionState.Idle -> "Not connected"
                    })
                },
                trailingContent = { Text("Change", color = MaterialTheme.colorScheme.primary) },
                modifier = Modifier.clickable(onClick = onChangeEngine),
            )
            Section("Appearance")
            Row(Modifier.padding(horizontal = 16.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                listOf("system" to "System", "light" to "Light", "dark" to "Dark").forEach { (mode, label) ->
                    FilterChip(selected = settings.theme == mode, onClick = { settings.updateTheme(mode) }, label = { Text(label) })
                }
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                ListItem(
                    headlineContent = { Text("Colours from the wallpaper") },
                    trailingContent = { Switch(settings.dynamicColor, onCheckedChange = settings::updateDynamicColor) },
                )
            }
        }
    }
}
