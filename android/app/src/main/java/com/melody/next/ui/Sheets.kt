package com.melody.next.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.PlaylistAdd
import androidx.compose.material.icons.automirrored.filled.QueueMusic
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.SkipNext
import androidx.compose.material.icons.filled.Speaker
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ActionSheet(vm: MainViewModel, target: Target) {
    ModalBottomSheet(onDismissRequest = vm::doneActing) {
        Column(Modifier.navigationBarsPadding()) {
            Text(target.title, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.Bold,
                modifier = Modifier.padding(horizontal = 24.dp, vertical = 8.dp))
            SheetAction(Icons.Default.PlayArrow, "Play", "Replace the queue") { vm.perform(MainViewModel.Action.Play, target) }
            SheetAction(Icons.Default.SkipNext, "Play next", "First in Up Next") { vm.perform(MainViewModel.Action.PlayNext, target) }
            SheetAction(Icons.AutoMirrored.Filled.PlaylistAdd, "Add to Up Next", "After what is waiting") {
                vm.perform(MainViewModel.Action.UpNext, target)
            }
            SheetAction(Icons.AutoMirrored.Filled.QueueMusic, "Add to the queue", "At its end") {
                vm.perform(MainViewModel.Action.Append, target)
            }
        }
    }
}

@Composable
private fun SheetAction(icon: ImageVector, title: String, detail: String, onClick: () -> Unit) {
    ListItem(
        leadingContent = { Icon(icon, null) },
        headlineContent = { Text(title) },
        supportingContent = { Text(detail) },
        modifier = Modifier.clickable(onClick = onClick),
    )
}

/** Where the engine plays: its own speakers, or an agent's. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun OutputsSheet(vm: MainViewModel, onDismiss: () -> Unit) {
    val outputs by vm.client.outputs.collectAsState()
    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(Modifier.navigationBarsPadding()) {
            Text("Play on", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.Bold,
                modifier = Modifier.padding(horizontal = 24.dp, vertical = 8.dp))
            if (outputs.isEmpty()) {
                Text("No outputs", color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.padding(24.dp))
            }
            outputs.forEach { output ->
                ListItem(
                    leadingContent = { Icon(if (output.local) Icons.Default.Speaker else Icons.Default.Wifi, null) },
                    headlineContent = { Text(output.name) },
                    supportingContent = {
                        Text(
                            when {
                                !output.online -> "Offline"
                                output.local -> "The engine's own speakers"
                                output.name == vm.app.settings.speakerName -> "This phone"
                                else -> "Agent"
                            }
                        )
                    },
                    trailingContent = {
                        if (output.selected) Icon(Icons.Default.CheckCircle, "Playing here", tint = MaterialTheme.colorScheme.primary)
                    },
                    modifier = Modifier.clickable(enabled = output.online && !output.selected) {
                        vm.client.selectOutput(output.id)
                        onDismiss()
                    },
                )
            }
        }
    }
}
