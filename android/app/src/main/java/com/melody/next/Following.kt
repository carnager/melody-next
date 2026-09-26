package com.melody.next

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * ADR-0234: which engine the phone follows -- whose player, queue, outputs
 * and notification it shows. The main one until something is played on
 * another, or another starts playing while the followed one is idle.
 */
class Following<E : Any>(
    main: E,
    private val playing: (E) -> Boolean,
    private val stop: (E) -> Unit,
) {
    private val _followed = MutableStateFlow(main)
    val followed: StateFlow<E> = _followed

    /**
     * About to play on `target`: it is followed from now on, and -- one
     * engine plays at a time (ADR-0227) -- the one followed before stops.
     */
    fun playOn(target: E) {
        val before = _followed.value
        if (before != target && playing(before)) stop(before)
        _followed.value = target
    }

    /** `engine` started playing: followed, unless the followed one plays. */
    fun started(engine: E) {
        val now = _followed.value
        if (engine != now && !playing(now)) _followed.value = engine
    }

    /** `engine` is gone: the main one is followed again if it was. */
    fun gone(engine: E, main: E) {
        if (_followed.value == engine) _followed.value = main
    }
}
