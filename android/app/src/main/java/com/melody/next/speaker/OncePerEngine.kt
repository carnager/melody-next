// SPDX-License-Identifier: GPL-3.0-only
package com.melody.next.speaker

import com.melody.next.engine.Endpoint

/**
 * The engines to offer this phone's speaker to, each once, in the order
 * given: one address once, and one engine once where its id is known --
 * listed at home and through a proxy, it is still one engine. An engine
 * whose id is not known yet is kept; the list is made again when it is.
 */
fun oncePerEngine(endpoints: List<Endpoint>, idOf: (Endpoint) -> String?): List<Endpoint> {
    val seen = HashSet<String>()
    return endpoints.distinctBy { it.host to it.port }.filter { endpoint ->
        val id = idOf(endpoint)
        id.isNullOrEmpty() || seen.add(id)
    }
}
