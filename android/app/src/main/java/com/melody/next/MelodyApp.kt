package com.melody.next

import android.app.Application
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.ProcessLifecycleOwner
import androidx.media3.common.util.UnstableApi
import com.melody.next.engine.ConnectionState
import com.melody.next.engine.Endpoint
import com.melody.next.engine.EngineClient
import com.melody.next.speaker.PhoneAgent
import com.melody.next.speaker.PhoneAudition
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch

/** The one engine client, the settings and the covers, for the whole app. */
@UnstableApi
class MelodyApp : Application() {
    val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    lateinit var settings: Settings
        private set
    lateinit var client: EngineClient
        private set
    lateinit var covers: Covers
        private set
    /** This phone as an output: its player, and its link to the engine. */
    lateinit var audition: PhoneAudition
        private set
    private var agent: PhoneAgent? = null
    lateinit var network: com.melody.next.speaker.Network
        private set

    override fun onCreate() {
        super.onCreate()
        instance = this
        settings = Settings(this)
        client = EngineClient(scope)
        covers = Covers(this, client)
        scope.launch {
            client.connection.collect { state ->
                if (state is ConnectionState.Connected) covers.engine = state.name
            }
        }
        audition = PhoneAudition(this) { agent?.noteChange() }
        network = com.melody.next.speaker.Network(this)
        // Off Wi-Fi, Opus; on it, what Settings say. The engine hears the
        // change in the next report and sends the next track that way.
        scope.launch {
            kotlinx.coroutines.flow.combine(
                network.metered,
                androidx.compose.runtime.snapshotFlow { settings.mobileBitrate to settings.wifiBitrate },
            ) { metered, (mobile, wifi) -> if (metered) mobile else wifi }
                .collect { bitrate -> agent?.bitrateKbps = bitrate }
        }
        settings.endpoint?.let(::useEngine)
        // Back in front: a connection the system dropped in the background
        // is made again now rather than on the next retry.
        ProcessLifecycleOwner.get().lifecycle.addObserver(object : DefaultLifecycleObserver {
            override fun onStart(owner: LifecycleOwner) = client.reconnectNow()
        })
    }

    /** Talks to this engine, and offers it this phone to play on. */
    fun useEngine(endpoint: Endpoint) {
        settings.endpoint = endpoint
        client.connect(endpoint)
        updateSpeaker()
    }

    /** The speaker follows the settings: on or off, under its name, for the engine chosen. */
    fun updateSpeaker() {
        agent?.stop()
        agent = null
        val endpoint = settings.endpoint ?: return
        if (!settings.speaker) return
        agent = PhoneAgent(scope, audition, settings.speakerName).also {
            it.bitrateKbps = if (network.metered.value) settings.mobileBitrate else settings.wifiBitrate
            it.start(endpoint)
        }
    }

    companion object {
        lateinit var instance: MelodyApp
            private set
    }
}
