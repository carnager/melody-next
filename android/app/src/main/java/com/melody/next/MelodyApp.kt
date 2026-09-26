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
import com.melody.next.speaker.SharedSpeaker
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.collectLatest
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
    /** The one player, a seat on it for each engine the phone plays for. */
    private lateinit var speaker: SharedSpeaker
    private class Speaking(val agent: PhoneAgent, val seat: com.melody.next.speaker.Audition, val name: String)
    /** ADR-0234: this phone's speaker, offered to every engine listed, by its endpoint. */
    private var agents: Map<Endpoint, Speaking> = emptyMap()
    private var watchingAgents: Job? = null
    /** ADR-0234: a connection to each engine whose lists are shown besides the main one's. */
    val otherClients = kotlinx.coroutines.flow.MutableStateFlow<List<EngineClient>>(emptyList())

    /** The other engines follow the settings: connected, or let go. */
    fun updateOtherEngines() {
        otherClients.value.forEach { it.disconnect() }
        otherCovers.clear()
        otherClients.value = settings.otherEngines.map { endpoint -> EngineClient(scope).also { it.connect(endpoint) } }
        if (followed.value !== client && followed.value !in otherClients.value) following.gone(followed.value, client)
        updateSpeaker()
    }

    /** ADR-0234: the engine whose playback the phone shows and controls. */
    private lateinit var following: Following<EngineClient>
    val followed: kotlinx.coroutines.flow.StateFlow<EngineClient> get() = following.followed

    /** About to play on `target`: followed from now on, and the one before stops. */
    fun playOn(target: EngineClient) = following.playOn(target)

    private val otherCovers = HashMap<EngineClient, Covers>()

    /** Covers as an engine has them: the same key on another is another album. */
    fun coversOf(engine: EngineClient): Covers =
        if (engine === client) covers
        else otherCovers.getOrPut(engine) {
            Covers(this, engine).also { kept ->
                scope.launch {
                    engine.connection.collect { state ->
                        if (state is ConnectionState.Connected) kept.engine = state.name
                    }
                }
            }
        }

    /** Another engine started -- by a window, another phone -- while the followed one is idle: followed. */
    private fun followStartsElsewhere() {
        scope.launch {
            otherClients.collectLatest { others ->
                kotlinx.coroutines.coroutineScope {
                    (listOf(client) + others).forEach { engine ->
                        launch {
                            var was = engine.state.value.playing
                            engine.state.collect { state ->
                                val started = state.playing && !was
                                was = state.playing
                                if (started) following.started(engine)
                            }
                        }
                    }
                }
            }
        }
    }

    /** This phone's speaker lost the engine mid-track and is getting it back. */
    val speakerReconnecting = kotlinx.coroutines.flow.MutableStateFlow(false)
    lateinit var network: com.melody.next.speaker.Network
        private set
    /** Albums kept on the phone, and the player for them when no engine is in reach. */
    lateinit var offline: com.melody.next.offline.OfflineStore
        private set
    lateinit var offlinePlayer: com.melody.next.offline.OfflinePlayer
        private set

    override fun onCreate() {
        super.onCreate()
        instance = this
        settings = Settings(this)
        client = EngineClient(scope)
        following = Following(client, { it.state.value.playing }, { it.stop() })
        covers = Covers(this, client)
        scope.launch {
            client.connection.collect { state ->
                if (state is ConnectionState.Connected) covers.engine = state.name
            }
        }
        network = com.melody.next.speaker.Network(this)
        offline = com.melody.next.offline.OfflineStore(this, scope, client, covers) {
            com.melody.next.offline.OfflineStore.Wanted(settings.downloadBitrate, settings.downloadOnWifiOnly, network.metered)
        }
        offlinePlayer = com.melody.next.offline.OfflinePlayer(this)
        audition = PhoneAudition(this, localCopy = { path -> offline.fileFor(path) }) { speaker.changed() }
        speaker = SharedSpeaker(audition)
        // Off Wi-Fi, Opus; on it, what Settings say. The engine hears the
        // change in the next report and sends the next track that way.
        scope.launch {
            kotlinx.coroutines.flow.combine(
                network.metered,
                androidx.compose.runtime.snapshotFlow { settings.mobileBitrate to settings.wifiBitrate },
            ) { metered, (mobile, wifi) -> if (metered) mobile else wifi }
                .collect { bitrate -> agents.values.forEach { it.agent.bitrateKbps = bitrate } }
        }
        settings.endpoint?.let(::useEngine)
        updateOtherEngines()
        followStartsElsewhere()
        // Back in front: a connection the system dropped in the background
        // is made again now rather than on the next retry.
        ProcessLifecycleOwner.get().lifecycle.addObserver(object : DefaultLifecycleObserver {
            override fun onStart(owner: LifecycleOwner) {
                client.reconnectNow()
                otherClients.value.forEach { it.reconnectNow() }
            }
        })
    }

    /** Talks to this engine, and offers it this phone to play on. */
    fun useEngine(endpoint: Endpoint) {
        settings.endpoint = endpoint
        client.connect(endpoint)
        updateSpeaker()
    }

    /** Paused by hand while the speaker waited: no longer worth staying in front for. */
    fun stopWaitingForSpeaker() {
        agents.values.forEach { it.agent.reconnecting.value = false }
        speakerReconnecting.value = false
    }

    /**
     * The speaker follows the settings: on or off, under its name, for the
     * engine chosen and every other engine listed. An engine still listed
     * keeps its connection, and what plays on it goes on playing.
     */
    fun updateSpeaker() {
        val wanted = if (!settings.speaker) emptyList()
        else (listOfNotNull(settings.endpoint) + settings.otherEngines).distinctBy { it.host to it.port }
        val kept = agents.filter { (endpoint, speaking) -> endpoint in wanted && speaking.name == settings.speakerName }
        (agents - kept.keys).values.forEach { speaking ->
            speaking.agent.stop()
            speaker.close(speaking.seat)
        }
        agents = wanted.associateWith { endpoint -> kept[endpoint] ?: startAgent(endpoint) }
        // Waiting for any engine that went away mid-track keeps the app in front.
        watchingAgents?.cancel()
        speakerReconnecting.value = agents.values.any { it.agent.reconnecting.value }
        if (agents.isNotEmpty()) {
            watchingAgents = scope.launch {
                kotlinx.coroutines.flow.combine(agents.values.map { it.agent.reconnecting }) { flags -> flags.any { it } }
                    .collect { speakerReconnecting.value = it }
            }
        }
    }

    private fun startAgent(endpoint: Endpoint): Speaking {
        lateinit var agent: PhoneAgent
        val seat = speaker.seat { agent.noteChange() }
        agent = PhoneAgent(scope, seat, settings.speakerName).also {
            it.bitrateKbps = if (network.metered.value) settings.mobileBitrate else settings.wifiBitrate
            it.start(endpoint)
        }
        return Speaking(agent, seat, settings.speakerName)
    }

    companion object {
        lateinit var instance: MelodyApp
            private set
    }
}
