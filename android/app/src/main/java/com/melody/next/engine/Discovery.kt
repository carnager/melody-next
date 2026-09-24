package com.melody.next.engine

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.atomic.AtomicBoolean

/** An engine announcing itself on this network (`_melody._tcp`). */
data class FoundEngine(val name: String, val host: String, val port: Int, val wantsPassword: Boolean) {
    fun endpoint(password: String = "") = Endpoint(host, port, password)
}

/**
 * Engines on the network, by name, as Trackknife and the agents find them:
 * each announces `_melody._tcp` over multicast DNS with its port and whether
 * it wants a password. Android resolves one service at a time, so found ones
 * wait their turn.
 */
fun discoverEngines(context: Context): Flow<List<FoundEngine>> = callbackFlow {
    val nsd = context.getSystemService(Context.NSD_SERVICE) as NsdManager
    val found = linkedMapOf<String, FoundEngine>()
    val waiting = ConcurrentLinkedQueue<NsdServiceInfo>()
    val resolving = AtomicBoolean(false)

    fun publish() = trySend(found.values.sortedBy { it.name.lowercase() })

    fun resolveNext() {
        if (!resolving.compareAndSet(false, true)) return
        val next = waiting.poll()
        if (next == null) {
            resolving.set(false)
            return
        }
        @Suppress("DEPRECATION")
        nsd.resolveService(next, object : NsdManager.ResolveListener {
            override fun onResolveFailed(info: NsdServiceInfo, error: Int) {
                resolving.set(false)
                resolveNext()
            }

            override fun onServiceResolved(info: NsdServiceInfo) {
                @Suppress("DEPRECATION")
                val host = info.host?.hostAddress
                if (host != null) {
                    val auth = info.attributes["auth"]?.let { String(it, Charsets.UTF_8) }
                    synchronized(found) {
                        found[info.serviceName] = FoundEngine(info.serviceName, host, info.port, auth == "1")
                        publish()
                    }
                }
                resolving.set(false)
                resolveNext()
            }
        })
    }

    val listener = object : NsdManager.DiscoveryListener {
        override fun onDiscoveryStarted(type: String) = Unit
        override fun onDiscoveryStopped(type: String) = Unit
        override fun onStartDiscoveryFailed(type: String, error: Int) = Unit
        override fun onStopDiscoveryFailed(type: String, error: Int) = Unit

        override fun onServiceFound(info: NsdServiceInfo) {
            waiting.add(info)
            resolveNext()
        }

        override fun onServiceLost(info: NsdServiceInfo) {
            synchronized(found) {
                if (found.remove(info.serviceName) != null) publish()
            }
        }
    }
    send(emptyList())
    nsd.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, listener)
    awaitClose { runCatching { nsd.stopServiceDiscovery(listener) } }
}

private const val SERVICE_TYPE = "_melody._tcp."
