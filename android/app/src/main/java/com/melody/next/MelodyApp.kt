package com.melody.next

import android.app.Application
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.ProcessLifecycleOwner
import androidx.media3.common.util.UnstableApi
import com.melody.next.engine.ConnectionState
import com.melody.next.engine.Endpoint
import com.melody.next.engine.EngineClient
import com.melody.next.engine.sameAddress
import com.melody.next.speaker.PhoneAgent
import com.melody.next.speaker.PhoneAudition
import com.melody.next.speaker.SharedSpeaker
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

/** The one engine client, the settings and the covers, for the whole app. */
@UnstableApi
class MelodyApp : Application() {
    val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    lateinit var settings: Settings
        private set
    lateinit var client: EngineClient
        private set
    /** ADR-0272: the engines on the network the phone is on. */
    lateinit var nearby: com.melody.next.engine.Nearby
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
        otherClients.value = settings.otherEngines.map { endpoint ->
            EngineClient(scope, nearby = nearby).also { it.connect(endpoint) }
        }
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
        Diagnostics.start(this)
        Diagnostics.note("started, process ${android.os.Process.myPid()}")
        noteLastExits()
        settings = Settings(this)
        // ADR-0272: engines are looked for here all along, so one known by
        // another address is reached directly once the phone is home.
        nearby = com.melody.next.engine.Nearby(
            com.melody.next.engine.discoverEngines(this)
                .stateIn(scope, kotlinx.coroutines.flow.SharingStarted.Eagerly, emptyList())
        )
        client = EngineClient(scope, nearby = nearby)
        scope.launch {
            client.engineId.collect { id -> if (id != null && id != settings.engineId) settings.engineId = id }
        }
        following = Following(client, { it.state.value.playing }, { it.stop() })
        covers = Covers(this, client)
        scope.launch {
            client.connection.collect { state ->
                if (state is ConnectionState.Connected) covers.engine = state.name
            }
        }
        network = com.melody.next.speaker.Network(this)
        scope.launch {
            network.changes.collect {
                nearby.networkChanged()
                client.networkChanged()
                otherClients.value.forEach { it.networkChanged() }
                agents.values.forEach { it.agent.networkChanged() }
            }
        }
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
        settings.endpoint?.let { client.connect(it, settings.engineId) }
        updateOtherEngines()
        followStartsElsewhere()
        // Back in front: a connection the system dropped in the background
        // is made again now rather than on the next retry.
        watchForDiagnostics()
        ProcessLifecycleOwner.get().lifecycle.addObserver(object : DefaultLifecycleObserver {
            override fun onStop(owner: LifecycleOwner) {
                inFront = false
                Diagnostics.note("in the background")
            }

            override fun onStart(owner: LifecycleOwner) {
                inFront = true
                Diagnostics.note("in front")
                client.reconnectNow()
                otherClients.value.forEach { it.reconnectNow() }
            }
        })
    }

    /** Talks to this engine, and offers it this phone to play on. */
    fun useEngine(endpoint: Endpoint) {
        val sameEngine = settings.endpoint?.sameAddress(endpoint) == true
        settings.endpoint = endpoint
        client.connect(endpoint, settings.engineId.takeIf { sameEngine })
        updateSpeaker()
    }

    /** Paused by hand while the speaker waited: no longer worth staying in front for. */
    fun stopWaitingForSpeaker() {
        agents.values.forEach { it.agent.reconnecting.value = false }
        speakerReconnecting.value = false
        // Playing on from its buffer while the engine is away (ADR-0274):
        // paused here, as the engine cannot be asked; it hears so on return.
        audition.pause()
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
        agent = PhoneAgent(scope, seat, settings.speakerName, nearby = nearby).also {
            it.bitrateKbps = if (network.metered.value) settings.mobileBitrate else settings.wifiBitrate
            it.start(endpoint) { clientAt(endpoint)?.engineId?.value }
        }
        scope.launch { agent.status.collect { Diagnostics.note("speaker for $endpoint: $it") } }
        return Speaking(agent, seat, settings.speakerName)
    }

    /** The client for the engine saved at `endpoint`: the main one, or another listed. */
    private fun clientAt(endpoint: Endpoint): EngineClient? {
        if (settings.endpoint?.sameAddress(endpoint) == true) return client
        val index = settings.otherEngines.indexOfFirst { it.sameAddress(endpoint) }
        return otherClients.value.getOrNull(index)
    }

    @Volatile private var inFront = false

    // Why the system ended this app's last processes: frozen, killed for
    // memory, its background use restricted -- the one thing no log of its
    // own can say.
    private fun noteLastExits() {
        if (android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.R) return
        val manager = getSystemService(android.app.ActivityManager::class.java)
        runCatching { manager.getHistoricalProcessExitReasons(packageName, 0, 3) }.getOrNull()?.forEach { exit ->
            Diagnostics.note(
                "earlier process ${exit.pid} ended ${java.text.SimpleDateFormat("MM-dd HH:mm:ss", java.util.Locale.ROOT).format(java.util.Date(exit.timestamp))}: " +
                    "reason ${exit.reason} (${exit.description ?: "no description"}), importance ${exit.importance}"
            )
        }
    }

    private fun watchForDiagnostics() {
        scope.launch { client.connection.collect { Diagnostics.note("engine: $it") } }
        scope.launch { speakerReconnecting.collect { Diagnostics.note("speaker waiting for the engine: $it") } }
        scope.launch { network.metered.collect { Diagnostics.note("network metered: $it") } }
        scope.launch { network.changes.collect { Diagnostics.note("Wi-Fi or Ethernet came or went") } }
        // Every network, VPNs too: a VPN rebuilt when the phone locks drops
        // every connection through it.
        val connectivity = getSystemService(android.net.ConnectivityManager::class.java)
        val describe = { network: android.net.Network ->
            "$network (${connectivity.getNetworkCapabilities(network)?.let(::transportsOf) ?: "gone"})"
        }
        connectivity.registerNetworkCallback(
            android.net.NetworkRequest.Builder()
                .removeCapability(android.net.NetworkCapabilities.NET_CAPABILITY_NOT_VPN)
                .build(),
            object : android.net.ConnectivityManager.NetworkCallback() {
                override fun onAvailable(network: android.net.Network) = Diagnostics.note("network up: ${describe(network)}")
                override fun onLost(network: android.net.Network) = Diagnostics.note("network lost: $network")
            },
        )
        connectivity.registerDefaultNetworkCallback(object : android.net.ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: android.net.Network) = Diagnostics.note("default network now: ${describe(network)}")
        })
        scope.launch { nearby.engines.collect { found -> Diagnostics.note("engines on this network: ${found.map { "${it.name}@${it.host}:${it.port}" }}") } }
        audition.player.addListener(object : androidx.media3.common.Player.Listener {
            override fun onIsPlayingChanged(isPlaying: Boolean) = Diagnostics.note("player playing: $isPlaying")
            override fun onPlaybackStateChanged(state: Int) = Diagnostics.note("player state: $state")
            override fun onPlayerError(error: androidx.media3.common.PlaybackException) =
                Diagnostics.note("player error: ${error.errorCodeName} ${error.message} ${error.cause}")
        })
        // A gap between these is the app frozen, or gone.
        scope.launch {
            while (true) {
                kotlinx.coroutines.delay(30_000)
                Diagnostics.note(
                    "heartbeat: front=$inFront service-foreground=${serviceInForeground()} " +
                        "engine-playing=${client.state.value.playing} player-playing=${audition.player.isPlaying} " +
                        "buffered-ahead=${audition.player.totalBufferedDuration / 1000}s " +
                        "speakers=${agents.values.map { it.agent.status.value::class.simpleName }} network=${transports()}"
                )
            }
        }
    }

    @Suppress("DEPRECATION") // For its own services it still answers.
    private fun serviceInForeground(): Boolean? = runCatching {
        getSystemService(android.app.ActivityManager::class.java).getRunningServices(50)
            .firstOrNull { it.service.className == PlaybackService::class.java.name }?.foreground ?: false
    }.getOrNull()

    private fun transports(): String {
        val connectivity = getSystemService(android.net.ConnectivityManager::class.java)
        val capabilities = connectivity.activeNetwork?.let(connectivity::getNetworkCapabilities) ?: return "none"
        return transportsOf(capabilities)
    }

    private fun transportsOf(capabilities: android.net.NetworkCapabilities): String =
        listOf(
            android.net.NetworkCapabilities.TRANSPORT_WIFI to "wifi",
            android.net.NetworkCapabilities.TRANSPORT_CELLULAR to "cellular",
            android.net.NetworkCapabilities.TRANSPORT_VPN to "vpn",
            android.net.NetworkCapabilities.TRANSPORT_ETHERNET to "ethernet",
        ).filter { capabilities.hasTransport(it.first) }.joinToString("+") { it.second }

    companion object {
        lateinit var instance: MelodyApp
            private set
    }
}
