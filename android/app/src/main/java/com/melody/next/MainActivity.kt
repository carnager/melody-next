package com.melody.next

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.lifecycle.viewmodel.compose.viewModel
import com.melody.next.ui.MainScreen
import com.melody.next.ui.MainViewModel
import com.melody.next.ui.MelodyTheme
import com.melody.next.ui.SetupScreen

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            // For the playback controls in the notification.
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 1)
        }
        val app = application as MelodyApp
        // The media controls: started while in front, which Android allows;
        // it stays while the engine plays.
        runCatching { startService(android.content.Intent(this, PlaybackService::class.java)) }
        setContent {
            MelodyTheme(app.settings) {
                var choosing by rememberSaveable { mutableStateOf(app.settings.endpoint == null) }
                if (choosing) {
                    // Opened to change the engine, it can be left as it was.
                    SetupScreen(
                        onChosen = { choosing = false },
                        onCancel = if (app.settings.endpoint != null) ({ choosing = false }) else null,
                    )
                } else {
                    val vm: MainViewModel = viewModel()
                    MainScreen(vm, onChangeEngine = { choosing = true })
                }
            }
        }
    }
}
