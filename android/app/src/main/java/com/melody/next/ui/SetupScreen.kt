package com.melody.next.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.MusicNote
import androidx.compose.material.icons.filled.Speaker
import androidx.compose.material3.Button
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import com.melody.next.MelodyApp
import com.melody.next.engine.ConnectionState
import com.melody.next.engine.Endpoint
import com.melody.next.engine.FoundEngine
import com.melody.next.engine.discoverEngines

/**
 * Choosing an engine: the ones on this network by name, as Trackknife finds
 * them, or an address typed in -- over a VPN from elsewhere, say.
 */
@Composable
fun SetupScreen(onChosen: () -> Unit) {
    val app = MelodyApp.instance
    val context = LocalContext.current
    val found by remember { discoverEngines(context) }.collectAsState(initial = emptyList())
    val connection by app.client.connection.collectAsState()
    val saved = app.settings.endpoint
    var address by remember { mutableStateOf(saved?.toString() ?: "") }
    var password by remember { mutableStateOf(saved?.password ?: "") }
    var chosen by remember { mutableStateOf<FoundEngine?>(null) }
    var problem by remember { mutableStateOf("") }

    fun connect(endpoint: Endpoint) {
        problem = ""
        app.useEngine(endpoint)
    }

    // Connected: done. Refused, or not reachable: said here.
    val state = connection
    if (state is ConnectionState.Connected && state.endpoint == app.settings.endpoint) {
        androidx.compose.runtime.LaunchedEffect(state) { onChosen() }
    }
    val connectionProblem = when (state) {
        is ConnectionState.Refused -> "${state.endpoint}: ${state.reason}"
        is ConnectionState.Connecting -> if (state.lastError.isNotEmpty()) "${state.endpoint}: ${state.lastError}" else ""
        else -> ""
    }

    Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
        Column(
            Modifier
                .fillMaxSize()
                .statusBarsPadding()
                .navigationBarsPadding()
                .imePadding()
                .verticalScroll(rememberScrollState())
                .padding(24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Spacer(Modifier.height(32.dp))
            Icon(Icons.Default.MusicNote, null, Modifier.size(56.dp), tint = MaterialTheme.colorScheme.primary)
            Spacer(Modifier.height(12.dp))
            Text("Melody", style = MaterialTheme.typography.headlineLarge, fontWeight = FontWeight.Bold)
            Text("Choose an engine", color = MaterialTheme.colorScheme.onSurfaceVariant)
            Spacer(Modifier.height(24.dp))

            Text(
                "On this network",
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.primary,
                modifier = Modifier.fillMaxWidth(),
            )
            Spacer(Modifier.height(4.dp))
            if (found.isEmpty()) {
                Text(
                    "Looking… an engine started with a name shows up here.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp),
                )
            }
            Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp))) {
                found.forEach { engine ->
                    ListItem(
                        headlineContent = { Text(engine.name) },
                        supportingContent = { Text("${engine.host}:${engine.port}") },
                        leadingContent = { Icon(Icons.Default.Speaker, null) },
                        trailingContent = { if (engine.wantsPassword) Icon(Icons.Default.Lock, "Wants a password") },
                        colors = ListItemDefaults.colors(containerColor = MaterialTheme.colorScheme.surfaceContainer),
                        modifier = Modifier.clickable {
                            address = "${engine.host}:${engine.port}"
                            if (engine.wantsPassword && password.isEmpty()) {
                                chosen = engine
                                problem = "${engine.name} wants its password"
                            } else {
                                connect(engine.endpoint(password))
                            }
                        },
                    )
                    HorizontalDivider()
                }
            }

            Spacer(Modifier.height(24.dp))
            Text(
                "Or by address",
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.primary,
                modifier = Modifier.fillMaxWidth(),
            )
            Spacer(Modifier.height(8.dp))
            OutlinedTextField(
                value = address,
                onValueChange = { address = it },
                label = { Text("Address") },
                placeholder = { Text("gemenon:6603") },
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                modifier = Modifier.fillMaxWidth(),
            )
            Spacer(Modifier.height(8.dp))
            OutlinedTextField(
                value = password,
                onValueChange = { password = it },
                label = { Text("Password (if the engine has one)") },
                singleLine = true,
                visualTransformation = PasswordVisualTransformation(),
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                modifier = Modifier.fillMaxWidth(),
            )
            val shownProblem = problem.ifEmpty { connectionProblem }
            if (shownProblem.isNotEmpty()) {
                Spacer(Modifier.height(8.dp))
                Text(shownProblem, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall,
                    modifier = Modifier.fillMaxWidth())
            }
            Spacer(Modifier.height(16.dp))
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                val connecting = connection is ConnectionState.Connecting
                Button(
                    onClick = {
                        val picked = chosen
                        val endpoint = if (picked != null && address == "${picked.host}:${picked.port}") picked.endpoint(password)
                        else Endpoint.parse(address, password)
                        if (endpoint == null) problem = "That is not an address" else connect(endpoint)
                    },
                    enabled = address.isNotBlank(),
                ) { Text(if (connecting) "Connecting…" else "Connect") }
            }
        }
    }
}
