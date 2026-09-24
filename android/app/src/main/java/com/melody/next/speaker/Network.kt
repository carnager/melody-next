package com.melody.next.speaker

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * Whether the network is metered -- mobile data, a tethered hotspot -- which
 * decides whether music comes as the original files or as Opus.
 */
class Network(context: Context) {
    private val connectivity = context.getSystemService(ConnectivityManager::class.java)
    private val _metered = MutableStateFlow(isMetered())
    val metered: StateFlow<Boolean> = _metered

    init {
        connectivity.registerDefaultNetworkCallback(object : ConnectivityManager.NetworkCallback() {
            override fun onCapabilitiesChanged(network: Network, capabilities: NetworkCapabilities) {
                _metered.value = !capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED)
            }

            override fun onLost(network: Network) {
                _metered.value = isMetered()
            }
        })
    }

    private fun isMetered(): Boolean {
        val capabilities = connectivity.getNetworkCapabilities(connectivity.activeNetwork) ?: return false
        return !capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED)
    }
}
