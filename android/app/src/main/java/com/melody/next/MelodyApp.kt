package com.melody.next

import android.app.Application
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.ProcessLifecycleOwner
import com.melody.next.engine.ConnectionState
import com.melody.next.engine.EngineClient
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch

/** The one engine client, the settings and the covers, for the whole app. */
class MelodyApp : Application() {
    val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    lateinit var settings: Settings
        private set
    lateinit var client: EngineClient
        private set
    lateinit var covers: Covers
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
        settings.endpoint?.let(client::connect)
        // Back in front: a connection the system dropped in the background
        // is made again now rather than on the next retry.
        ProcessLifecycleOwner.get().lifecycle.addObserver(object : DefaultLifecycleObserver {
            override fun onStart(owner: LifecycleOwner) = client.reconnectNow()
        })
    }

    companion object {
        lateinit var instance: MelodyApp
            private set
    }
}
