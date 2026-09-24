package com.melody.next.ui

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.melody.next.MelodyApp
import com.melody.next.engine.ConnectionState
import com.melody.next.engine.EntryKind
import com.melody.next.engine.LibraryEntry
import com.melody.next.engine.QueueEntry
import com.melody.next.engine.toQueueEntry
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch

/** Where the library is: its levels, as a back stack. */
sealed interface LibraryLevel {
    data object Artists : LibraryLevel
    /** Newest first, across the whole library. */
    data object Latest : LibraryLevel
    data class Albums(val artist: LibraryEntry) : LibraryLevel
    data class Tracks(val album: LibraryEntry) : LibraryLevel
    /** Albums kept on the phone: there with no engine in reach. */
    data object Offline : LibraryLevel
    data class OfflineAlbum(val key: String) : LibraryLevel
}

/** What an action applies to: an album, or some tracks. */
sealed interface Target {
    val title: String
    data class Album(val album: LibraryEntry) : Target {
        override val title get() = album.album.ifEmpty { album.label }
    }
    data class Tracks(val tracks: List<LibraryEntry>, val albumArtist: String = "") : Target {
        override val title get() = tracks.singleOrNull()?.let { it.title.ifEmpty { it.label } } ?: "${tracks.size} tracks"
    }
}

class MainViewModel : ViewModel() {
    val app = MelodyApp.instance
    val client = app.client

    // --- The library ---
    val levels = mutableStateListOf<LibraryLevel>(LibraryLevel.Artists)
    val level: LibraryLevel get() = levels.last()
    var entries by mutableStateOf<List<LibraryEntry>>(emptyList())
        private set
    var loading by mutableStateOf(false)
        private set
    var libraryError by mutableStateOf("")
        private set
    private var loadingJob: Job? = null

    // --- Search ---
    var searchText by mutableStateOf("")
        private set
    var foundAlbums by mutableStateOf<List<LibraryEntry>>(emptyList())
        private set
    var foundTracks by mutableStateOf<List<LibraryEntry>>(emptyList())
        private set
    var searching by mutableStateOf(false)
        private set
    private var searchJob: Job? = null

    /** The action sheet's subject, when it is open. */
    var acting by mutableStateOf<Target?>(null)
        private set

    var message by mutableStateOf<String?>(null)
        private set

    init {
        // A connection made -- or made again, to another engine -- starts
        // the library afresh; what was shown belongs to the old one.
        viewModelScope.launch {
            client.connection
                .map { (it as? ConnectionState.Connected)?.name }
                .distinctUntilChanged()
                .collect { name ->
                    if (name != null) {
                        app.covers.forgetMissing()
                        reload()
                        if (searchText.isNotBlank()) search(searchText)
                    }
                }
        }
        viewModelScope.launch { client.problems.collect { message = it } }
        // A rating set anywhere shows on what is open.
        viewModelScope.launch {
            client.ratings.collect { change ->
                entries = entries.map { if (it.ratingHash == change.hash) it.copy(rating = change.rating) else it }
                levels.replaceAll { level ->
                    if (level is LibraryLevel.Tracks && level.album.ratingHash == change.hash) {
                        LibraryLevel.Tracks(level.album.copy(rating = change.rating))
                    } else {
                        level
                    }
                }
            }
        }
        viewModelScope.launch {
            app.offline.problems.collect { problem ->
                if (problem != null) {
                    message = problem
                    app.offline.problems.value = null
                }
            }
        }
    }

    fun messageShown() {
        message = null
    }

    fun open(next: LibraryLevel) {
        levels.add(next)
        reload()
    }

    fun back(): Boolean {
        if (levels.size <= 1) return false
        levels.removeAt(levels.lastIndex)
        reload()
        return true
    }

    /** The library's ways in: artists, the newest albums, or what is kept here. */
    fun showTop(top: LibraryLevel) {
        levels.clear()
        levels.add(top)
        reload()
    }

    fun showLatest(latest: Boolean) = showTop(if (latest) LibraryLevel.Latest else LibraryLevel.Artists)

    fun reload() {
        loadingJob?.cancel()
        val shown = level
        loadingJob = viewModelScope.launch {
            loading = true
            libraryError = ""
            try {
                entries = when (shown) {
                    LibraryLevel.Artists -> client.query(EntryKind.Artist, limit = 20_000).entries
                    LibraryLevel.Latest -> client.query(EntryKind.Album, newestFirst = true, limit = 200).entries
                    is LibraryLevel.Albums -> client.query(EntryKind.Album, artist = shown.artist.key, limit = 2_000).entries
                    is LibraryLevel.Tracks -> client.tracksOf(shown.album)
                    // Kept on the phone: read from there, not asked for.
                    LibraryLevel.Offline, is LibraryLevel.OfflineAlbum -> emptyList()
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                entries = emptyList()
                libraryError = failure.message ?: "the library could not be read"
            } finally {
                loading = false
            }
        }
    }

    /** Albums by their artist, title or year, and tracks by theirs -- as the quick pick finds them. */
    fun search(text: String) {
        searchText = text
        searchJob?.cancel()
        if (text.isBlank()) {
            foundAlbums = emptyList()
            foundTracks = emptyList()
            searching = false
            return
        }
        searchJob = viewModelScope.launch {
            delay(250)
            searching = true
            try {
                foundAlbums = client.query(EntryKind.Album, text = text, limit = 50).entries
                foundTracks = client.query(EntryKind.Track, text = text, limit = 100).entries
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                message = failure.message
            } finally {
                searching = false
            }
        }
    }

    // --- Actions ---

    fun act(on: Target) {
        acting = on
    }

    fun doneActing() {
        acting = null
    }

    enum class Action { Play, PlayNext, UpNext, Append }

    fun perform(action: Action, on: Target) {
        acting = null
        viewModelScope.launch {
            val entries = try {
                entriesOf(on)
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                message = failure.message
                return@launch
            }
            when (action) {
                Action.Play -> client.play(entries)
                Action.PlayNext -> client.request(entries, first = true)
                Action.UpNext -> client.request(entries, first = false)
                Action.Append -> client.append(entries)
            }
            if (action != Action.Play) message = "${on.title}: ${
                when (action) {
                    Action.PlayNext -> "plays next"
                    Action.UpNext -> "added to Up Next"
                    else -> "added to the queue"
                }
            }"
        }
    }

    /** A tapped track plays in its album, from where it is. */
    fun playFrom(tracks: List<LibraryEntry>, index: Int, albumArtist: String) {
        client.play(tracks.map { it.toQueueEntry(albumArtist) }, startAt = index)
    }

    private suspend fun entriesOf(on: Target): List<QueueEntry> = when (on) {
        is Target.Album -> client.tracksOf(on.album).map { it.toQueueEntry(on.album.artist) }
        is Target.Tracks -> on.tracks.map { it.toQueueEntry(on.albumArtist.ifEmpty { it.artist }) }
    }

    fun rateAlbum(album: LibraryEntry, rating: Int) = rate(album.ratingHash, true, rating) {
        levels.replaceAll { level ->
            if (level is LibraryLevel.Tracks && level.album.key == album.key) LibraryLevel.Tracks(album.copy(rating = rating)) else level
        }
    }

    fun rateTrack(track: LibraryEntry, rating: Int) = rate(track.ratingHash, false, rating) {
        entries = entries.map { if (it.key == track.key) it.copy(rating = rating) else it }
    }

    private fun rate(hash: String, album: Boolean, rating: Int, shown: () -> Unit) {
        if (hash.isEmpty()) {
            message = "The library has no rating key for this yet"
            return
        }
        viewModelScope.launch {
            try {
                client.setRating(hash, album, rating)
                shown()
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                message = failure.message
            }
        }
    }
}
