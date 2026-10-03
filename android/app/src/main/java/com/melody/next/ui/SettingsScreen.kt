package com.melody.next.ui

import android.os.Build
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.clickable
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
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
import androidx.compose.runtime.setValue
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
        Column(Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding().verticalScroll(rememberScrollState())) {
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
                        is ConnectionState.Connected ->
                            "Connected · ${state.endpoint}" + state.notice.let { if (it.isEmpty()) "" else "\n$it" }
                        is ConnectionState.Connecting -> "Connecting…"
                        is ConnectionState.Refused -> state.reason
                        ConnectionState.Idle -> "Not connected"
                    })
                },
                trailingContent = { Text("Change", color = MaterialTheme.colorScheme.primary) },
                modifier = Modifier.clickable(onClick = onChangeEngine),
            )
            OtherEngines(vm)
            Section("This phone as a speaker")
            ListItem(
                headlineContent = { Text("Offer it to the engine") },
                supportingContent = { Text("The engine, Trackknife and the other clients can then play on it") },
                trailingContent = {
                    Switch(settings.speaker, onCheckedChange = {
                        settings.updateSpeaker(it)
                        vm.app.updateSpeaker()
                    })
                },
            )
            var name by androidx.compose.runtime.saveable.rememberSaveable { androidx.compose.runtime.mutableStateOf(settings.speakerName) }
            androidx.compose.material3.OutlinedTextField(
                value = name,
                onValueChange = { name = it },
                label = { Text("Its name") },
                singleLine = true,
                enabled = settings.speaker,
                keyboardActions = androidx.compose.foundation.text.KeyboardActions(onDone = {
                    settings.updateSpeakerName(name)
                    vm.app.updateSpeaker()
                }),
                keyboardOptions = androidx.compose.foundation.text.KeyboardOptions(imeAction = androidx.compose.ui.text.input.ImeAction.Done),
                modifier = Modifier.padding(horizontal = 16.dp).fillMaxWidth(),
            )
            Section("Streaming to this phone")
            Text(
                "On Wi-Fi",
                style = MaterialTheme.typography.bodyMedium,
                modifier = Modifier.padding(start = 16.dp, top = 4.dp),
            )
            Row(Modifier.padding(horizontal = 16.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                listOf(0 to "Original", 192 to "Opus 192", 128 to "Opus 128").forEach { (kbps, label) ->
                    FilterChip(selected = settings.wifiBitrate == kbps, onClick = { settings.updateWifiBitrate(kbps) }, label = { Text(label) })
                }
            }
            Text(
                "On mobile data",
                style = MaterialTheme.typography.bodyMedium,
                modifier = Modifier.padding(start = 16.dp, top = 8.dp),
            )
            Row(Modifier.padding(horizontal = 16.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                listOf(64, 96, 128, 160).forEach { kbps ->
                    FilterChip(selected = settings.mobileBitrate == kbps, onClick = { settings.updateMobileBitrate(kbps) }, label = { Text("$kbps") })
                }
            }
            Text(
                "Opus kbps. What the phone cannot play itself is always sent as Opus.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
            )
            Section("Keeping albums on this phone")
            Row(Modifier.padding(horizontal = 16.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                listOf(128 to "Opus 128", 160 to "Opus 160", 192 to "Opus 192", 0 to "Original").forEach { (kbps, label) ->
                    FilterChip(selected = settings.downloadBitrate == kbps, onClick = { settings.updateDownloadBitrate(kbps) }, label = { Text(label) })
                }
            }
            ListItem(
                headlineContent = { Text("Only on Wi-Fi") },
                supportingContent = { Text("Downloads wait for a network that is not metered") },
                trailingContent = { Switch(settings.downloadOnWifiOnly, onCheckedChange = settings::updateDownloadOnWifiOnly) },
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

/**
 * ADR-0234: engines besides the one this phone plays through, whose lists
 * show too -- a desktop's open tabs beside the NAS's. Found on the network,
 * or by address where the network does not reach (a VPN); with this phone's
 * engine password.
 */
@Composable
private fun OtherEngines(vm: MainViewModel) {
    val settings = vm.app.settings
    val context = androidx.compose.ui.platform.LocalContext.current
    val found by androidx.compose.runtime.remember { com.melody.next.engine.discoverEngines(context) }
        .collectAsState(initial = emptyList())
    val clients by vm.app.otherClients.collectAsState()
    val password = settings.endpoint?.password ?: ""
    val update = { engines: List<com.melody.next.engine.Endpoint> ->
        settings.updateOtherEngines(engines)
        vm.app.updateOtherEngines()
    }
    Section("Lists from other engines")
    settings.otherEngines.forEachIndexed { index, endpoint ->
        val connection = clients.getOrNull(index)?.connection?.collectAsState()?.value
        ListItem(
            headlineContent = { Text((connection as? ConnectionState.Connected)?.name ?: endpoint.host) },
            supportingContent = {
                Text(
                    when (connection) {
                        is ConnectionState.Connected -> "Connected · $endpoint"
                        is ConnectionState.Refused -> connection.reason
                        is ConnectionState.Connecting -> "Connecting…"
                        else -> endpoint.toString()
                    },
                )
            },
            trailingContent = {
                IconButton(onClick = { update(settings.otherEngines - endpoint) }) {
                    Icon(androidx.compose.material.icons.Icons.Default.Close, "Remove")
                }
            },
        )
    }
    val main = settings.endpoint
    val offered = found.filter { engine ->
        (main == null || engine.host != main.host || engine.port != main.port) &&
            settings.otherEngines.none { it.host == engine.host && it.port == engine.port }
    }
    offered.forEach { engine ->
        ListItem(
            headlineContent = { Text(engine.name) },
            supportingContent = { Text("On the network · ${engine.host}:${engine.port}") },
            trailingContent = { Text("Add", color = MaterialTheme.colorScheme.primary) },
            modifier = Modifier.clickable { update(settings.otherEngines + engine.endpoint(password)) },
        )
    }
    var typed by androidx.compose.runtime.saveable.rememberSaveable { androidx.compose.runtime.mutableStateOf("") }
    androidx.compose.material3.OutlinedTextField(
        value = typed,
        onValueChange = { typed = it },
        label = { Text("Add by address") },
        placeholder = { Text("host or host:port") },
        singleLine = true,
        keyboardActions = androidx.compose.foundation.text.KeyboardActions(onDone = {
            com.melody.next.engine.Endpoint.parse(typed, password)?.let {
                update(settings.otherEngines + it)
                typed = ""
            }
        }),
        keyboardOptions = androidx.compose.foundation.text.KeyboardOptions(imeAction = androidx.compose.ui.text.input.ImeAction.Done),
        modifier = Modifier.padding(horizontal = 16.dp).fillMaxWidth(),
    )
}
