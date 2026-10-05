package com.melody.next.speaker

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow

/** What decides whether a network costs: as much of it as matters here. */
data class Link(
    val vpn: Boolean,
    val notMetered: Boolean,
    val wifiOrEthernet: Boolean = false,
    val cellular: Boolean = false,
    val validated: Boolean = true,
)

/**
 * Whether music should come as Opus rather than the original files.
 *
 * A VPN is not the network the bytes cost on: WireGuard declares its tunnel
 * unmetered whatever carries it, and the phone pulled original FLAC over
 * mobile data. So behind a VPN, what it runs over decides -- mobile data
 * among the transports it reports, or failing that, whether an unmetered
 * Wi-Fi or Ethernet network is there to carry it.
 */
fun meteredOver(default: Link?, others: List<Link>): Boolean {
    if (default == null) return false
    if (!default.vpn) return !default.notMetered
    if (default.cellular) return true
    return others.none { !it.vpn && it.validated && it.wifiOrEthernet && it.notMetered }
}

/**
 * Whether the network is metered -- mobile data, a tethered hotspot, either
 * of them under a VPN -- which decides whether music comes as the original
 * files or as Opus.
 */
class Network(context: Context) {
    private val connectivity = context.getSystemService(ConnectivityManager::class.java)
    private val _metered = MutableStateFlow(decide())
    val metered: StateFlow<Boolean> = _metered
    private val _changes = MutableSharedFlow<Unit>(extraBufferCapacity = 1)
    /** A Wi-Fi or Ethernet network came or went: home joined or left. */
    val changes: SharedFlow<Unit> = _changes

    init {
        // Any network, not only the default: behind a VPN the default stays
        // the VPN while what it runs over changes underneath it.
        connectivity.registerDefaultNetworkCallback(refresher())
        connectivity.registerNetworkCallback(NetworkRequest.Builder().build(), refresher())
        // Wi-Fi and Ethernet themselves: behind a VPN the default network
        // stays the VPN while home is joined or left beneath it.
        val local = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .addTransportType(NetworkCapabilities.TRANSPORT_ETHERNET)
            .build()
        connectivity.registerNetworkCallback(local, object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) { _changes.tryEmit(Unit) }
            override fun onLost(network: Network) { _changes.tryEmit(Unit) }
        })
    }

    private fun refresher() = object : ConnectivityManager.NetworkCallback() {
        override fun onAvailable(network: Network) { _metered.value = decide() }
        override fun onCapabilitiesChanged(network: Network, capabilities: NetworkCapabilities) {
            _metered.value = decide()
        }
        override fun onLost(network: Network) { _metered.value = decide() }
    }

    private fun link(capabilities: NetworkCapabilities) = Link(
        vpn = capabilities.hasTransport(NetworkCapabilities.TRANSPORT_VPN),
        notMetered = capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED),
        wifiOrEthernet = capabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) ||
            capabilities.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET),
        cellular = capabilities.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR),
        validated = capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED),
    )

    @Suppress("DEPRECATION") // allNetworks: what carries a VPN that does not say.
    private fun decide(): Boolean {
        val default = connectivity.activeNetwork?.let(connectivity::getNetworkCapabilities)
        val others = connectivity.allNetworks.mapNotNull { connectivity.getNetworkCapabilities(it)?.let(::link) }
        return meteredOver(default?.let(::link), others)
    }
}
