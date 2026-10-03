// SPDX-License-Identifier: GPL-3.0-only
package com.melody.next.engine

import org.json.JSONObject

/**
 * ADR-0260: what this app speaks, and what an engine's version means for
 * working with it -- the same rule as the desktop's protocol/version.hpp.
 */
object ProtocolVersion {
    /** Changes only when a release breaks clients. */
    const val PROTOCOL = 1
    /** Goes up with every release that adds to the protocol. */
    const val LEVEL = 1

    /** What [info], an engine.info answer, means: not usable, and the one sentence to say. */
    data class Verdict(val incompatible: Boolean, val message: String)

    fun of(info: JSONObject, engineName: String): Verdict {
        // An engine too old to say is protocol 1, level 0.
        val protocol = info.optInt("protocol", 1)
        val level = info.optInt("level", 0)
        val name = engineName.ifEmpty { "The engine" }
        return when {
            protocol != PROTOCOL -> Verdict(
                true,
                "$name speaks protocol $protocol; this app speaks protocol $PROTOCOL. " +
                    "Update ${if (protocol > PROTOCOL) "the app" else "$name's melodyd"}.",
            )
            level < LEVEL -> Verdict(
                false,
                "$name's melodyd is older than this app; some things will not work until it is updated.",
            )
            else -> Verdict(false, "")
        }
    }
}
