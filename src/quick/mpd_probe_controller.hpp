// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "quick/mpd_browser_model.hpp"
#include "quick/mpd_output_model.hpp"
#include "quick/mpd_queue_model.hpp"
#include "trackknife/mpd/session.hpp"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <cstdint>
#include <memory>
#include <optional>

class QTimer;

namespace trackknife::quick {

enum class QueueAddMode { append, next, replace };

// Bridges the Qt-free persistent MPD session into UI-thread state consumed by
// the native Widgets shell. The historical public method name `probe()` starts
// the live session and is retained until connection profiles replace this M2 API.
class MpdProbeController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString details READ details NOTIFY stateChanged)
    Q_PROPERTY(QString nowPlaying READ nowPlaying NOTIFY stateChanged)
    Q_PROPERTY(QString nowPlayingTitle READ nowPlayingTitle NOTIFY stateChanged)
    Q_PROPERTY(QString nowPlayingDetail READ nowPlayingDetail NOTIFY stateChanged)
    Q_PROPERTY(QString outputSummary READ outputSummary NOTIFY stateChanged)
    Q_PROPERTY(int queueCount READ queueCount NOTIFY stateChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY stateChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY stateChanged)
    Q_PROPERTY(bool commandBusy READ commandBusy NOTIFY stateChanged)
    Q_PROPERTY(qint64 elapsedMs READ elapsedMs NOTIFY stateChanged)
    Q_PROPERTY(qint64 durationMs READ durationMs NOTIFY stateChanged)
    Q_PROPERTY(int volume READ volume NOTIFY stateChanged)
    Q_PROPERTY(int outputCount READ outputCount NOTIFY stateChanged)
    Q_PROPERTY(QAbstractItemModel* queueModel READ queueModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* libraryModel READ libraryModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* browserModel READ browserModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* browserPlaylistModel READ browserPlaylistModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* outputModel READ outputModel CONSTANT)
    Q_PROPERTY(bool libraryBusy READ libraryBusy NOTIFY stateChanged)
    Q_PROPERTY(QString libraryStatus READ libraryStatus NOTIFY stateChanged)
    Q_PROPERTY(bool hasMoreSearchResults READ hasMoreSearchResults NOTIFY stateChanged)
    Q_PROPERTY(QString lastSearchQuery READ lastSearchQuery NOTIFY stateChanged)
    Q_PROPERTY(bool browserBusy READ browserBusy NOTIFY stateChanged)
    Q_PROPERTY(QString browserStatus READ browserStatus NOTIFY stateChanged)
    Q_PROPERTY(QString browserPath READ browserPath NOTIFY stateChanged)
    Q_PROPERTY(bool browserShowingPlaylist READ browserShowingPlaylist NOTIFY stateChanged)
    Q_PROPERTY(bool repeatEnabled READ repeatEnabled NOTIFY stateChanged)
    Q_PROPERTY(bool randomEnabled READ randomEnabled NOTIFY stateChanged)
    Q_PROPERTY(int singleMode READ singleMode NOTIFY stateChanged)
    Q_PROPERTY(int consumeMode READ consumeMode NOTIFY stateChanged)
    Q_PROPERTY(QString replayGainMode READ replayGainMode NOTIFY stateChanged)
    Q_PROPERTY(bool supportsReplayGain READ supportsReplayGain NOTIFY stateChanged)
    Q_PROPERTY(QString activeOutputName READ activeOutputName NOTIFY stateChanged)
    Q_PROPERTY(QString profileId READ profileId NOTIFY stateChanged)

  public:
    explicit MpdProbeController(QObject* parent = nullptr);
    ~MpdProbeController() override;

    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] bool connected() const noexcept { return connected_; }
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] QString details() const { return details_; }
    [[nodiscard]] QString nowPlaying() const { return now_playing_; }
    [[nodiscard]] QString nowPlayingTitle() const { return now_playing_title_; }
    [[nodiscard]] QString nowPlayingDetail() const { return now_playing_detail_; }
    [[nodiscard]] QString nowPlayingArtist() const { return now_playing_artist_; }
    [[nodiscard]] QString nowPlayingAlbum() const { return now_playing_album_; }
    [[nodiscard]] QString nowPlayingUri() const { return now_playing_uri_; }
    [[nodiscard]] QString outputSummary() const { return output_summary_; }
    [[nodiscard]] int queueCount() const { return queue_model_.rowCount(); }
    [[nodiscard]] bool playing() const noexcept {
        return presentedPlaybackState() == mpd::PlaybackState::playing;
    }
    [[nodiscard]] bool paused() const noexcept {
        return presentedPlaybackState() == mpd::PlaybackState::paused;
    }
    [[nodiscard]] bool commandBusy() const noexcept { return !pending_commands_.isEmpty(); }
    [[nodiscard]] qint64 elapsedMs() const noexcept { return elapsed_ms_; }
    [[nodiscard]] qint64 durationMs() const noexcept { return duration_ms_; }
    [[nodiscard]] int volume() const noexcept { return volume_; }
    [[nodiscard]] int outputCount() const { return output_model_.rowCount(); }
    [[nodiscard]] QAbstractItemModel* queueModel() noexcept { return &queue_model_; }
    [[nodiscard]] QAbstractItemModel* libraryModel() noexcept { return &library_model_; }
    [[nodiscard]] std::vector<mpd::Track> libraryTracksSnapshot() const {
        return library_model_.tracksSnapshot();
    }
    [[nodiscard]] std::vector<mpd::AlbumSummary> libraryAlbumsSnapshot() const {
        return library_albums_;
    }
    [[nodiscard]] QAbstractItemModel* browserModel() noexcept { return &browser_model_; }
    [[nodiscard]] QAbstractItemModel* browserPlaylistModel() noexcept {
        return &browser_playlist_model_;
    }
    [[nodiscard]] std::vector<mpd::Track> browserPlaylistTracksSnapshot() const {
        return browser_playlist_model_.tracksSnapshot();
    }
    [[nodiscard]] QAbstractItemModel* outputModel() noexcept { return &output_model_; }
    [[nodiscard]] bool libraryBusy() const noexcept { return pending_library_query_.has_value(); }
    [[nodiscard]] QString libraryStatus() const { return library_status_; }
    [[nodiscard]] bool hasMoreSearchResults() const noexcept { return search_has_more_; }
    [[nodiscard]] QString lastSearchQuery() const { return last_library_query_; }
    [[nodiscard]] bool browserBusy() const noexcept { return pending_browser_query_.has_value(); }
    [[nodiscard]] QString browserStatus() const { return browser_status_; }
    [[nodiscard]] QString browserPath() const { return browser_path_; }
    [[nodiscard]] bool browserShowingPlaylist() const noexcept { return browser_showing_playlist_; }
    [[nodiscard]] bool repeatEnabled() const noexcept {
        return optimistic_repeat_.value_or(repeat_enabled_);
    }
    [[nodiscard]] bool randomEnabled() const noexcept {
        return optimistic_random_.value_or(random_enabled_);
    }
    [[nodiscard]] int singleMode() const noexcept;
    [[nodiscard]] int consumeMode() const noexcept;
    [[nodiscard]] QString replayGainMode() const;
    [[nodiscard]] bool supportsReplayGain() const noexcept { return supports_replay_gain_; }
    [[nodiscard]] bool supportsCommand(const QString& command) const {
        return advertised_commands_.contains(command.toLower());
    }
    [[nodiscard]] bool supportsTag(const QString& tag) const {
        return advertised_tag_types_.contains(tag.toLower());
    }
    [[nodiscard]] QString activeOutputName() const { return active_output_name_; }
    [[nodiscard]] QString profileId() const { return profile_id_; }

    Q_INVOKABLE void probe(const QString& host, int port, const QString& password,
                           const QString& music_root);
    Q_INVOKABLE void probeProfile(const QString& profile_id, const QString& host, int port,
                                  const QString& password, const QString& music_root);
    Q_INVOKABLE void disconnectFromServer();
    Q_INVOKABLE void playPause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    Q_INVOKABLE void playQueueItem(int row);
    Q_INVOKABLE void removeQueueItem(int row);
    Q_INVOKABLE void removeQueueItems(const QVariantList& rows);
    Q_INVOKABLE void cropQueueToItems(const QVariantList& rows);
    Q_INVOKABLE void clearQueue();
    // Asks MPD to update its database below uri; empty updates everything.
    Q_INVOKABLE void updateDatabase(const QString& uri);
    // Requests the newest-first root ordering for the library's Latest
    // mode; the result arrives on newestRootOrderLoaded.
    Q_INVOKABLE void loadNewestRootOrder(const QString& tag);
    Q_INVOKABLE void moveQueueItem(int row, int target_row);
    Q_INVOKABLE void moveQueueItems(const QVariantList& rows, int insertion_row);
    Q_INVOKABLE void setQueuePriority(const QVariantList& rows, int priority);
    // ADR-0179: 0-10 track/album ratings. The backend is picked from the
    // advertised commands: Melody's native rate extension when getrating is
    // advertised, the interoperable rating sticker otherwise.
    [[nodiscard]] bool supportsRatings() const {
        return supportsCommand(QStringLiteral("getrating")) ||
               supportsCommand(QStringLiteral("sticker"));
    }
    [[nodiscard]] bool supportsAlbumRatings() const {
        return supportsCommand(QStringLiteral("albumrate"));
    }
    Q_INVOKABLE void setTrackRating(const QVariantList& rows, int rating);
    Q_INVOKABLE void setMelodyAlbumRating(const QString& album_artist, const QString& album,
                                          const QString& date, int rating);
    // The explicitly stored Melody album rating for a queue album group;
    // 0 when unrated or not yet loaded.
    [[nodiscard]] unsigned melodyStoredAlbumRating(const QString& group_key) const {
        return melody_album_stored_ratings_.value(group_key, 0U);
    }
    // Server-translated structured queries (tkq_melody.hpp) need the newer
    // Melody filter vocabulary, advertised alongside searchalbums.
    // ADR-0187: stored playlists are independently playable contexts.
    [[nodiscard]] bool supportsPlaybackContexts() const {
        return connected_ && supportsCommand(QStringLiteral("melody_context"));
    }
    [[nodiscard]] QString activeContextName() const { return active_context_; }
    // Queue position of the playing song, -1 when nothing plays.
    [[nodiscard]] int songPosition() const { return song_position_; }
    [[nodiscard]] bool supportsServerQueries() const {
        return connected_ && supportsCommand(QStringLiteral("searchalbums"));
    }
    using ServerQueryCompletion = std::function<void(core::Result<std::vector<mpd::Track>>)>;
    // Runs a raw filter expression; the completion fires on the UI thread
    // with the bounded track list or the failure.
    void searchServerExpression(const QString& expression, const QString& sort,
                                ServerQueryCompletion completion);
    Q_INVOKABLE void searchLibrary(const QString& query);
    Q_INVOKABLE void continueSearch();
    Q_INVOKABLE void addLibraryItem(int row);
    Q_INVOKABLE void addLibraryItemNext(int row);
    Q_INVOKABLE void addLibraryItems(const QVariantList& rows, bool next);
    Q_INVOKABLE void addUris(const QStringList& uris, bool next);
    Q_INVOKABLE void addUrisAt(const QStringList& uris, int position);
    Q_INVOKABLE void replaceQueueWithUris(const QStringList& uris);
    Q_INVOKABLE void replaceQueueWithUrisAndPlayAt(const QStringList& uris, int row);
    // row < 0 resumes where the list was left.
    Q_INVOKABLE void playStoredPlaylistContext(const QString& name, int row);
    Q_INVOKABLE void playQueueContext(int row);
    // ADR-0188: play a working tab's tracks as a context — the queue is
    // stashed rather than replaced.
    Q_INVOKABLE void playTrackListContext(const QStringList& uris, int row);
    void addAlbum(mpd::AlbumFilter album, QueueAddMode mode);
    void loadSearchAlbum(quint64 token, const mpd::AlbumFilter& album);
    Q_INVOKABLE void browseDirectory(const QString& uri);
    Q_INVOKABLE void browseTag(const QString& tag);
    Q_INVOKABLE void loadServerLibraryRoot(quint64 token, const QString& preferred_tag);
    Q_INVOKABLE void loadServerLibraryBranch(quint64 token, const QString& tag,
                                             const QString& value);
    Q_INVOKABLE void loadServerLibraryArtwork(quint64 token, const QString& uri);
    Q_INVOKABLE void searchServerLibraryFilter(quint64 token, const QString& preferred_tag,
                                               const QString& query);
    Q_INVOKABLE void browseStoredPlaylists();
    Q_INVOKABLE void browseParentDirectory();
    Q_INVOKABLE void activateBrowserItem(int row);
    Q_INVOKABLE void addBrowserItemNext(int row);
    Q_INVOKABLE void addBrowserItems(const QVariantList& rows, bool next);
    Q_INVOKABLE void addBrowserPlaylistItems(const QVariantList& rows, bool next);
    Q_INVOKABLE void openStoredPlaylist(const QString& name);
    Q_INVOKABLE void saveQueueAsPlaylist(const QString& name);
    Q_INVOKABLE void loadStoredPlaylistIntoQueue(const QString& name);
    Q_INVOKABLE void addToStoredPlaylist(const QString& name, const QStringList& uris,
                                         int insertion_row = -1);
    Q_INVOKABLE void removeStoredPlaylistItems(const QString& name, const QVariantList& rows);
    Q_INVOKABLE void moveStoredPlaylistItem(const QString& name, int row, int target_row);
    Q_INVOKABLE void clearStoredPlaylist(const QString& name);
    Q_INVOKABLE void renameStoredPlaylist(const QString& from, const QString& to);
    Q_INVOKABLE void deleteStoredPlaylist(const QString& name);
    Q_INVOKABLE void setRepeatEnabled(bool enabled);
    Q_INVOKABLE void setRandomEnabled(bool enabled);
    Q_INVOKABLE void setSingleMode(int mode);
    Q_INVOKABLE void setConsumeMode(int mode);
    Q_INVOKABLE void setReplayGainMode(const QString& mode);
    Q_INVOKABLE void seekTo(qint64 position_ms);
    Q_INVOKABLE void setVolume(int volume);
    Q_INVOKABLE void setOutputEnabled(quint32 output_id, bool enabled);

  signals:
    void stateChanged();
    void notificationRequested(const QString& message);
    void searchFinished(const QString& query, bool success);
    void searchAlbumLoaded(quint64 token, const std::vector<mpd::Track>& tracks,
                           const QString& error);
    void storedPlaylistLoaded(const QString& name);
    void storedPlaylistRenamed(const QString& from, const QString& to);
    void storedPlaylistDeleted(const QString& name);
    void tagListLoaded(const QString& tag, const QStringList& values);
    void newestRootOrderLoaded(const QStringList& values, const QString& error);
    void serverLibraryRootLoaded(quint64 token, const QString& tag, const QStringList& values,
                                 const QString& error);
    void serverLibraryAlbumCountsLoaded(quint64 token,
                                        const std::vector<mpd::ArtistAlbumCount>& counts,
                                        const QString& error);
    void serverLibraryBranchLoaded(quint64 token, const std::vector<mpd::Track>& tracks,
                                   const QString& error);
    void serverLibraryArtworkLoaded(quint64 token, const QByteArray& data);
    void serverLibraryFilterLoaded(quint64 token, const QStringList& root_values,
                                   const QString& error);
    void serverDatabaseChanged();
    void storedPlaylistsChanged();
    void storedPlaylistListLoaded(const QStringList& names);
    void artworkLoaded(const QString& uri, const QByteArray& data);

  private:
    void applyState(std::uint64_t token, mpd::SessionState state);
    void applySnapshot(std::uint64_t token, mpd::SessionSnapshot snapshot);
    void applyCommandResult(std::uint64_t token, mpd::SessionCommandResult result);
    void submitTransport(mpd::TransportAction action);
    void requestMelodyAlbumRatings(const std::vector<mpd::Track>& queue);
    void beginOptimisticPlayback(std::uint64_t command_id, mpd::PlaybackState state);
    void enqueueUri(std::string uri, bool next);
    void enqueueUris(std::vector<std::string> uris, bool next);
    void enqueueUrisAt(std::vector<std::string> uris, std::optional<unsigned> first_position);
    void reloadStoredPlaylist(const QString& name);
    void updateElapsedTimer();
    [[nodiscard]] mpd::PlaybackState presentedPlaybackState() const noexcept;
    void clearSessionState();

    bool busy_{false};
    bool connected_{false};
    bool legacy_melody_output_restore_{false};
    bool supports_replay_gain_{false};
    QSet<QString> advertised_commands_;
    QSet<QString> advertised_tag_types_;
    std::uint64_t connection_token_{0U};
    std::uint64_t generation_{0U};
    QString status_{QStringLiteral("Not connected")};
    QString details_;
    QString now_playing_{QStringLiteral("Nothing playing")};
    QString now_playing_title_{QStringLiteral("Nothing playing")};
    QString now_playing_detail_{QStringLiteral("Connect to MPD or Melody")};
    QString now_playing_artist_;
    QString now_playing_album_;
    QString now_playing_uri_;
    QString output_summary_;
    QString active_output_name_{QStringLiteral("No output")};
    QString profile_id_;
    mpd::PlaybackState playback_state_{mpd::PlaybackState::unknown};
    std::optional<mpd::PlaybackState> optimistic_playback_state_;
    bool repeat_enabled_{false};
    bool random_enabled_{false};
    mpd::PlaybackModeState single_mode_{mpd::PlaybackModeState::unknown};
    mpd::PlaybackModeState consume_mode_{mpd::PlaybackModeState::unknown};
    mpd::ReplayGainMode replay_gain_mode_{mpd::ReplayGainMode::unknown};
    QString active_context_;
    int song_position_{-1};
    std::optional<bool> optimistic_repeat_;
    std::optional<bool> optimistic_random_;
    std::optional<mpd::PlaybackModeState> optimistic_single_;
    std::optional<mpd::PlaybackModeState> optimistic_consume_;
    std::optional<mpd::ReplayGainMode> optimistic_replay_gain_;
    std::optional<std::uint64_t> pending_playback_command_;
    qint64 elapsed_ms_{0};
    qint64 duration_ms_{0};
    int volume_{-1};
    std::optional<std::uint32_t> current_song_id_;
    std::optional<std::uint64_t> pending_library_query_;
    QHash<quint64, QueueAddMode> pending_album_adds_;
    // ADR-0179: Melody album ratings cached per album group identity and
    // invalidated when a rating mutation succeeds. The display map falls
    // back to the server's computed mean; the stored map holds only the
    // explicit album rating that Rate menus check against.
    QHash<QString, unsigned> melody_album_ratings_;
    QHash<QString, unsigned> melody_album_stored_ratings_;
    QHash<std::uint64_t, QString> pending_album_rating_queries_;
    QHash<std::uint64_t, ServerQueryCompletion> pending_expression_searches_;
    QHash<quint64, quint64> pending_search_albums_;
    QString pending_library_query_text_;
    QString last_library_query_;
    bool pending_search_append_{false};
    bool search_has_more_{false};
    std::optional<std::uint64_t> pending_browser_query_;
    std::optional<std::uint64_t> pending_newest_order_;
    std::optional<std::uint64_t> pending_tag_query_;
    std::optional<std::uint64_t> pending_stored_playlists_query_;
    QString pending_tag_name_;
    struct PendingLibraryTreeRoot {
        quint64 token{0U};
        QString tag;
    };
    QHash<quint64, PendingLibraryTreeRoot> pending_library_tree_roots_;
    QHash<quint64, quint64> pending_library_tree_branches_;
    QHash<quint64, quint64> pending_library_album_counts_;
    QHash<quint64, PendingLibraryTreeRoot> pending_library_tree_filters_;
    QHash<quint64, quint64> pending_library_tree_artwork_;
    std::optional<std::uint64_t> pending_artwork_query_;
    QString current_artwork_uri_;
    QHash<quint64, QString> pending_playlist_queries_;
    enum class PlaylistMutationKind { save, load, edit, rename, remove };
    struct PendingPlaylistMutation {
        PlaylistMutationKind kind{PlaylistMutationKind::edit};
        QString name;
        QString target_name;
    };
    QHash<quint64, PendingPlaylistMutation> pending_playlist_mutations_;
    QSet<quint64> pending_commands_;
    MpdQueueModel queue_model_;
    MpdQueueModel library_model_;
    std::vector<mpd::AlbumSummary> library_albums_;
    MpdBrowserModel browser_model_;
    MpdQueueModel browser_playlist_model_;
    MpdOutputModel output_model_;
    QString library_status_{QStringLiteral("Search albums and tracks")};
    QString browser_status_{QStringLiteral("No folder loaded")};
    QString browser_path_;
    QString pending_browser_path_;
    bool browser_showing_playlist_{false};
    QTimer* elapsed_timer_{nullptr};
    QTimer* playback_confirmation_timer_{nullptr};
    std::unique_ptr<mpd::Session> session_;
};

} // namespace trackknife::quick
