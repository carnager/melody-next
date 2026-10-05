// SPDX-License-Identifier: GPL-3.0-only
package com.melody.next.engine

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.update

/**
 * The engines announcing themselves on the network the phone is on, and the
 * nearer way to one the phone knows by another address (ADR-0272): an engine
 * saved as wss://music.example, found at home under the same id, is reached
 * there directly -- not out through the proxy and back in.
 */
class Nearby(val engines: StateFlow<List<FoundEngine>> = MutableStateFlow(emptyList())) {
    // Found, but not answering there: an announcement outlives the phone's
    // leaving the network. Passed over until what is found changes -- the
    // list it was found in is kept to tell -- or the network does.
    private val unreachable = MutableStateFlow(Passed(emptyList(), emptySet()))

    private data class Passed(val among: List<FoundEngine>, val engines: Set<FoundEngine>)

    private fun passedOver(): Set<FoundEngine> =
        unreachable.value.let { if (it.among === engines.value) it.engines else emptySet() }

    private fun found(saved: Endpoint, id: String?): FoundEngine? {
        if (id.isNullOrEmpty()) return null
        val passed = passedOver()
        return engines.value.firstOrNull { it.id == id && it !in passed }
            ?.takeUnless { it.endpoint().sameAddress(saved) }
    }

    /**
     * The engine with this id as found here, with the saved address's
     * password -- one engine, one password; null when it is not found, its
     * id not yet known, or the saved address is already the one found.
     */
    fun nearer(saved: Endpoint, id: String?): Endpoint? = found(saved, id)?.endpoint(saved.password)

    /** Returns once there is a nearer way to the engine than `saved`. */
    suspend fun awaitNearer(saved: Endpoint, id: () -> String?) {
        combine(engines, unreachable) { _, _ -> }.first { nearer(saved, id()) != null }
    }

    /** Another network: what did not answer on the last may on this one. */
    fun networkChanged() {
        unreachable.value = Passed(emptyList(), emptySet())
    }

    /**
     * Reaches the engine the nearer way where there is one, else at its saved
     * address. A found engine can be stale, so it is given a short time, and
     * the saved address is tried after it.
     */
    suspend fun <T> reach(saved: Endpoint, id: String?, savedTimeoutMs: Int, open: suspend (Endpoint, Int) -> T): Reached<T> {
        found(saved, id)?.let { local ->
            try {
                return Reached(local.endpoint(saved.password), open(local.endpoint(saved.password), NEARBY_TIMEOUT_MS))
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (_: Throwable) {
                // Not here after all: the saved address it is.
                val among = engines.value
                unreachable.update { passed ->
                    Passed(among, (if (passed.among === among) passed.engines else emptySet()) + local)
                }
            }
        }
        return Reached(saved, open(saved, savedTimeoutMs))
    }

    data class Reached<T>(val endpoint: Endpoint, val opened: T)

    companion object {
        /** On the same network an engine answers at once; this is generous for it. */
        const val NEARBY_TIMEOUT_MS = 2_000
    }
}

/** The same place to connect to, whatever the password. */
fun Endpoint.sameAddress(other: Endpoint) =
    host == other.host && port == other.port && transport == other.transport && path == other.path
