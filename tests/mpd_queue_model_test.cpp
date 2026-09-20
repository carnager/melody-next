// SPDX-License-Identifier: GPL-3.0-only

#include "quick/mpd_browser_model.hpp"
#include "quick/mpd_output_model.hpp"
#include "quick/mpd_probe_controller.hpp"
#include "quick/mpd_queue_model.hpp"
#include "quick/mpd_search_result_model.hpp"
#include "uicommon/rating_stars.hpp"

#include <QBrush>
#include <QItemSelectionModel>
#include <QMetaProperty>
#include <QSignalSpy>
#include <QTest>

#include <chrono>
#include <cstdint>
#include <vector>

namespace trackknife::quick {
namespace {

[[nodiscard]] mpd::Track queue_track(const std::uint32_t id, const std::uint32_t position,
                                     std::string title = {}) {
    mpd::Metadata metadata;
    if (!title.empty()) {
        metadata = mpd::Metadata{{{"Title", std::move(title)}}};
    }
    return mpd::Track{
        .uri = "track-" + std::to_string(id) + ".flac",
        .metadata = std::move(metadata),
        .musicbrainz = {},
        .queue_id = id,
        .queue_position = position,
        .duration = std::nullopt,
        .last_modified = std::nullopt,
        .audio_format = std::nullopt,
        .priority = std::nullopt,
        .rating = std::nullopt,
        .melody_song_id = std::nullopt,
        .unknown_structural_pairs = {},
    };
}

[[nodiscard]] mpd::Track search_track(std::string uri, std::string album, std::string title,
                                      std::string track_number, std::string release_id,
                                      std::string date = "2000") {
    mpd::MusicBrainzIdentity identity;
    identity.release_ids.push_back(std::move(release_id));
    return mpd::Track{
        .uri = std::move(uri),
        .metadata = mpd::Metadata{{
            {"Artist", "Artist"},
            {"AlbumArtist", "Artist"},
            {"Album", std::move(album)},
            {"Date", std::move(date)},
            {"Track", std::move(track_number)},
            {"Title", std::move(title)},
        }},
        .musicbrainz = std::move(identity),
        .queue_id = std::nullopt,
        .queue_position = std::nullopt,
        .duration = std::chrono::milliseconds{180'000},
        .last_modified = std::nullopt,
        .audio_format = std::nullopt,
        .priority = std::nullopt,
        .rating = std::nullopt,
        .melody_song_id = std::nullopt,
        .unknown_structural_pairs = {},
    };
}

} // namespace

class MpdQueueModelTest final : public QObject {
    Q_OBJECT

  private slots:
    void projectsOrderedMetadataAndQueueIdentity();
    void avoidsResetForAnUnchangedSnapshot();
    void reconcilesSingleInsertionWithoutReset();
    void reconcilesSingleRemovalWithoutReset();
    void keepsSelectionOnStableIdAcrossMove();
    void resetsForAComplexReorder();
    void loadsGroupedQueueArtworkSerially();
    void loadsArtworkPastTheRequestBudget();
    void exposesMelodyOutputState();
    void projectsHeterogeneousBrowserEntries();
    void exposesOutputCountToQml();
    void groupsLiveSearchAlbumsAndTracks();
    void acceptsOneCharacterSearchAndReportsDisconnectedAlbums();
};

void MpdQueueModelTest::projectsOrderedMetadataAndQueueIdentity() {
    MpdQueueModel model;
    mpd::Track track{
        .uri = "Slayer/Divine Intervention/01.flac",
        .metadata = mpd::Metadata{{
            {"Artist", "Slayer"},
            {"Artist", "Guest"},
            {"Title", "Killing Fields"},
            {"Album", "Divine Intervention"},
            {"Date", "1994"},
            {"Track", "1"},
        }},
        .musicbrainz = {},
        .queue_id = 73U,
        .queue_position = 0U,
        .duration = std::chrono::milliseconds{220'900},
        .last_modified = std::nullopt,
        .audio_format = std::nullopt,
        .priority = 192U,
        .rating = std::nullopt,
        .melody_song_id = std::nullopt,
        .unknown_structural_pairs = {},
    };
    model.replaceTracks({std::move(track)});

    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.columnCount(), MpdQueueModel::column_count);
    for (int column = 0; column < ui::track_column_count; ++column) {
        QCOMPARE(
            model.headerData(column, Qt::Horizontal, Qt::DisplayRole).toString(),
            QString::fromLatin1(ui::track_column_headers.at(static_cast<std::size_t>(column))));
    }
    QVERIFY(model.data(model.index(0, ui::track_artwork_column)).toString().isEmpty());
    QCOMPARE(model.data(model.index(0, ui::track_artist_column)).toString(),
             QStringLiteral("Slayer, Guest"));
    QCOMPARE(model.data(model.index(0, ui::track_number_column)).toString(), QStringLiteral("1"));
    QCOMPARE(model.data(model.index(0, ui::track_title_column)).toString(),
             QStringLiteral("Killing Fields"));
    QCOMPARE(model.data(model.index(0, ui::track_length_column)).toString(),
             QStringLiteral("3:40"));
    const auto duration_alignment =
        model.data(model.index(0, ui::track_length_column), Qt::TextAlignmentRole)
            .value<Qt::Alignment>();
    QVERIFY(duration_alignment.testFlag(Qt::AlignRight));
    QVERIFY(duration_alignment.testFlag(Qt::AlignVCenter));
    QCOMPARE(model.data(model.index(0, 0), MpdQueueModel::QueueIdRole).toUInt(), 73U);
    QCOMPARE(model.data(model.index(0, 0), MpdQueueModel::QueuePositionRole).toUInt(), 0U);
    QCOMPARE(model.data(model.index(0, 0), MpdQueueModel::DurationMsRole).toLongLong(), 220'900);
    QCOMPARE(model.data(model.index(0, 0), MpdQueueModel::PriorityRole).toUInt(), 192U);
    // ADR-0179: no Melody line and no sticker means unrated; a sticker map
    // fills the value, and a track's own X-Rating projection wins over it.
    QCOMPARE(model.data(model.index(0, 0), MpdQueueModel::RatingRole).toUInt(), 0U);
    QCOMPARE(model.data(model.index(0, ui::track_rating_column)).toString(), QString{});
    model.setStickerRatings({{QStringLiteral("Slayer/Divine Intervention/01.flac"), 7U}});
    QCOMPARE(model.data(model.index(0, 0), MpdQueueModel::RatingRole).toUInt(), 7U);
    QCOMPARE(model.data(model.index(0, ui::track_rating_column)).toString(),
             QStringLiteral("★★★½"));
    QCOMPARE(model.ratingAt(0), 7U);
    QCOMPARE(model.data(model.index(0, ui::track_rating_column), Qt::ForegroundRole)
                 .value<QBrush>()
                 .color(),
             ui::ratingStarColor());
    mpd::Track melody_rated = *model.trackAt(0);
    melody_rated.rating = 4U;
    melody_rated.melody_song_id = 9'001U;
    MpdQueueModel melody_model;
    melody_model.replaceTracks({melody_rated});
    melody_model.setStickerRatings({{QStringLiteral("Slayer/Divine Intervention/01.flac"), 7U}});
    QCOMPARE(melody_model.data(melody_model.index(0, 0), MpdQueueModel::RatingRole).toUInt(), 4U);
    // Album ratings key on the delegate's group identity and paint over the
    // group's cover from the artwork column.
    melody_model.setAlbumRatings({{MpdQueueModel::albumGroupKey(melody_rated), 9U}});
    QCOMPARE(melody_model
                 .data(melody_model.index(0, ui::track_artwork_column), ui::track_album_rating_role)
                 .toUInt(),
             9U);
    QCOMPARE(model.totalDurationMs(), 220'900);
    QVERIFY(!model.data(model.index(0, 0), MpdQueueModel::CurrentRole).toBool());
    QCOMPARE(model.data(model.index(0, 0), MpdQueueModel::AlbumArtistRole).toString(),
             QStringLiteral("Slayer, Guest"));
    model.setCurrentSongId(73U);
    QVERIFY(model.data(model.index(0, 0), MpdQueueModel::CurrentRole).toBool());
    QCOMPARE(model.queueIdAt(0), std::optional<std::uint32_t>{73U});
    QCOMPARE(model.queueIdAt(1), std::nullopt);
    QCOMPARE(model.rowForQueueId(73U), std::optional<int>{0});
    QCOMPARE(model.rowForQueueId(999U), std::nullopt);
    QCOMPARE(model.uriAt(0), std::optional<std::string>{"Slayer/Divine Intervention/01.flac"});
    QCOMPARE(model.uriAt(1), std::nullopt);
    QVERIFY(model.flags(model.index(0, 0)).testFlag(Qt::ItemIsDragEnabled));
}

void MpdQueueModelTest::avoidsResetForAnUnchangedSnapshot() {
    MpdQueueModel model;
    std::vector<mpd::Track> tracks;
    tracks.push_back(mpd::Track{
        .uri = "unchanged.flac",
        .metadata = {},
        .musicbrainz = {},
        .queue_id = 1U,
        .queue_position = 0U,
        .duration = std::nullopt,
        .last_modified = std::nullopt,
        .audio_format = std::nullopt,
        .priority = std::nullopt,
        .rating = std::nullopt,
        .melody_song_id = std::nullopt,
        .unknown_structural_pairs = {},
    });
    model.replaceTracks(tracks);
    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);

    model.replaceTracks(std::move(tracks));

    QCOMPARE(reset.count(), 0);
}

void MpdQueueModelTest::reconcilesSingleInsertionWithoutReset() {
    MpdQueueModel model;
    model.replaceTracks({queue_track(1U, 0U), queue_track(3U, 1U)});
    QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);

    model.replaceTracks({queue_track(1U, 0U), queue_track(2U, 1U), queue_track(3U, 2U)});

    QCOMPARE(inserted.count(), 1);
    QCOMPARE(reset.count(), 0);
    QCOMPARE(model.queueIdAt(1), std::optional<std::uint32_t>{2U});
    QCOMPARE(model.data(model.index(2, 0), MpdQueueModel::QueuePositionRole).toUInt(), 2U);
}

void MpdQueueModelTest::reconcilesSingleRemovalWithoutReset() {
    MpdQueueModel model;
    model.replaceTracks({queue_track(1U, 0U), queue_track(2U, 1U), queue_track(3U, 2U)});
    QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);

    model.replaceTracks({queue_track(1U, 0U), queue_track(3U, 1U)});

    QCOMPARE(removed.count(), 1);
    QCOMPARE(reset.count(), 0);
    QCOMPARE(model.queueIdAt(1), std::optional<std::uint32_t>{3U});
}

void MpdQueueModelTest::keepsSelectionOnStableIdAcrossMove() {
    MpdQueueModel model;
    model.replaceTracks({queue_track(1U, 0U), queue_track(2U, 1U), queue_track(3U, 2U)});
    QItemSelectionModel selection(&model);
    selection.setCurrentIndex(model.index(1, 0),
                              QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QSignalSpy moved(&model, &QAbstractItemModel::rowsMoved);
    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);

    model.replaceTracks({queue_track(2U, 0U), queue_track(1U, 1U), queue_track(3U, 2U)});

    QCOMPARE(moved.count(), 1);
    QCOMPARE(reset.count(), 0);
    QCOMPARE(selection.currentIndex().row(), 0);
    QCOMPARE(model.data(selection.currentIndex(), MpdQueueModel::QueueIdRole).toUInt(), 2U);
}

void MpdQueueModelTest::resetsForAComplexReorder() {
    MpdQueueModel model;
    model.replaceTracks(
        {queue_track(1U, 0U), queue_track(2U, 1U), queue_track(3U, 2U), queue_track(4U, 3U)});
    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);

    model.replaceTracks(
        {queue_track(3U, 0U), queue_track(4U, 1U), queue_track(1U, 2U), queue_track(2U, 3U)});

    QCOMPARE(reset.count(), 1);
}

void MpdQueueModelTest::loadsGroupedQueueArtworkSerially() {
    MpdQueueModel model;
    QSignalSpy artwork_requests{&model, &MpdQueueModel::artworkRequested};
    model.setArtworkEnabled(true);
    model.replaceTracks({
        search_track("alpha-1.flac", "Alpha", "First", "1", "release-alpha"),
        search_track("alpha-2.flac", "Alpha", "Second", "2", "release-alpha"),
        search_track("beta-1.flac", "Beta", "Other", "1", "release-beta"),
    });

    QCOMPARE(artwork_requests.size(), 1);
    QCOMPARE(artwork_requests.front().at(1).toString(), QStringLiteral("alpha-1.flac"));
    QCOMPARE(model.data(model.index(0, 0), MpdQueueModel::AlbumArtworkUriRole).toString(),
             QStringLiteral("alpha-1.flac"));
    QCOMPARE(model.data(model.index(1, 0), MpdQueueModel::AlbumArtworkUriRole).toString(),
             QStringLiteral("alpha-1.flac"));
    QImage cover{8, 8, QImage::Format_ARGB32_Premultiplied};
    cover.fill(Qt::red);
    model.acceptArtwork(artwork_requests.front().front().toULongLong(), cover);
    QCOMPARE(artwork_requests.size(), 2);
    QCOMPARE(artwork_requests.back().at(1).toString(), QStringLiteral("beta-1.flac"));
    QCOMPARE(model.data(model.index(0, 0), MpdQueueModel::AlbumArtworkRole)
                 .value<QImage>()
                 .pixelColor(4, 4),
             QColor(Qt::red));
}

// The request budget bounds requests in flight, not the number of albums a
// list may ever load: a long list used to stop showing covers once 64
// albums had been fetched.
void MpdQueueModelTest::loadsArtworkPastTheRequestBudget() {
    MpdQueueModel model;
    QSignalSpy artwork_requests{&model, &MpdQueueModel::artworkRequested};
    model.setArtworkEnabled(true);

    constexpr int albums = 200;
    std::vector<mpd::Track> tracks;
    tracks.reserve(static_cast<std::size_t>(albums));
    for (int album = 0; album < albums; ++album) {
        const auto uri = QStringLiteral("album-%1/01.flac").arg(album);
        tracks.push_back(search_track(uri.toStdString(),
                                      QStringLiteral("Album %1").arg(album).toStdString(),
                                      QStringLiteral("Track %1").arg(album).toStdString(), "1",
                                      QStringLiteral("release-%1").arg(album).toStdString()));
    }
    model.replaceTracks(std::move(tracks));

    QImage cover{8, 8, QImage::Format_ARGB32_Premultiplied};
    cover.fill(Qt::blue);
    for (int answered = 0; answered < albums; ++answered) {
        QCOMPARE(artwork_requests.size(), answered + 1);
        model.acceptArtwork(artwork_requests.back().front().toULongLong(), cover);
    }
    QCOMPARE(artwork_requests.size(), albums);
    QCOMPARE(model.data(model.index(albums - 1, 0), MpdQueueModel::AlbumArtworkRole)
                 .value<QImage>()
                 .pixelColor(4, 4),
             QColor(Qt::blue));
}

void MpdQueueModelTest::exposesMelodyOutputState() {
    MpdOutputModel model;
    model.replaceOutputs({mpd::Output{
        .id = 4U,
        .name = "caprica",
        .enabled = true,
        .plugin = "agent",
        .attributes = {},
        .primary = true,
        .online = true,
        .stream_format = "flac",
        .maximum_bitrate = std::nullopt,
    }});

    QCOMPARE(model.rowCount(), 1);
    const auto index = model.index(0, 0);
    QCOMPARE(model.data(index, MpdOutputModel::OutputIdRole).toUInt(), 4U);
    QCOMPARE(model.data(index, MpdOutputModel::NameRole).toString(), QStringLiteral("caprica"));
    QCOMPARE(model.data(index, MpdOutputModel::EnabledRole).toBool(), true);
    QCOMPARE(model.data(index, MpdOutputModel::PrimaryRole).toBool(), true);
    QCOMPARE(model.data(index, MpdOutputModel::OnlineRole).toBool(), true);
    QCOMPARE(model.data(index, MpdOutputModel::DetailRole).toString(),
             QStringLiteral("online · primary · flac"));
}

void MpdQueueModelTest::projectsHeterogeneousBrowserEntries() {
    MpdBrowserModel model;
    model.replaceEntries({
        mpd::DatabaseDirectory{
            .uri = "Slayer", .last_modified = "2026-08-24T10:00:00Z", .unknown_pairs = {}},
        queue_track(7U, 0U, "Loose track"),
        mpd::StoredPlaylist{.name = "Road mix", .last_modified = std::nullopt, .unknown_pairs = {}},
    });

    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(model.columnCount(), MpdBrowserModel::column_count);
    QCOMPARE(model.data(model.index(0, 0)).toString(), QStringLiteral("Slayer"));
    QCOMPARE(model.data(model.index(0, 1)).toString(), QStringLiteral("Folder"));
    QCOMPARE(model.data(model.index(1, 1)).toString(), QStringLiteral("Track"));
    QCOMPARE(model.data(model.index(2, 1)).toString(), QStringLiteral("Playlist"));
    QCOMPARE(model.data(model.index(0, 0), MpdBrowserModel::ContainerRole).toBool(), true);
    QCOMPARE(model.data(model.index(1, 0), MpdBrowserModel::ContainerRole).toBool(), false);
    QCOMPARE(model.kindAt(0),
             std::optional<MpdBrowserModel::EntryKind>{MpdBrowserModel::EntryKind::directory});
    QCOMPARE(model.uriAt(2), std::optional<std::string>{"Road mix"});
}

void MpdQueueModelTest::exposesOutputCountToQml() {
    MpdProbeController controller;
    const auto property_index = controller.metaObject()->indexOfProperty("outputCount");
    const auto profile_property = controller.metaObject()->indexOfProperty("profileId");
    const auto move_method = controller.metaObject()->indexOfMethod("moveQueueItem(int,int)");
    const auto move_batch_method =
        controller.metaObject()->indexOfMethod("moveQueueItems(QVariantList,int)");
    const auto priority_method =
        controller.metaObject()->indexOfMethod("setQueuePriority(QVariantList,int)");
    const auto positioned_add_method =
        controller.metaObject()->indexOfMethod("addUrisAt(QStringList,int)");
    const auto playlist_remove_method =
        controller.metaObject()->indexOfMethod("removeStoredPlaylistItems(QString,QVariantList)");
    const auto playlist_move_method =
        controller.metaObject()->indexOfMethod("moveStoredPlaylistItem(QString,int,int)");

    QVERIFY(property_index >= 0);
    QVERIFY(profile_property >= 0);
    QVERIFY(move_method >= 0);
    QVERIFY(move_batch_method >= 0);
    QVERIFY(priority_method >= 0);
    QVERIFY(positioned_add_method >= 0);
    QVERIFY(playlist_remove_method >= 0);
    QVERIFY(playlist_move_method >= 0);
    QCOMPARE(controller.metaObject()->property(property_index).read(&controller).toInt(), 0);
}

void MpdQueueModelTest::acceptsOneCharacterSearchAndReportsDisconnectedAlbums() {
    MpdProbeController controller;
    QSignalSpy finished{&controller, &MpdProbeController::searchFinished};
    controller.searchLibrary(QStringLiteral(" X "));
    QCOMPARE(finished.size(), 1);
    QCOMPARE(finished.front().front().toString(), QStringLiteral("X"));
    QVERIFY(!finished.front()[1].toBool());
    controller.searchLibrary(QStringLiteral(" "));
    QCOMPARE(finished.size(), 1);
    QVERIFY(controller.lastSearchQuery().isEmpty());
    QSignalSpy albums{&controller, &MpdProbeController::searchAlbumLoaded};
    controller.loadSearchAlbum(42, {});
    QCOMPARE(albums.size(), 1);
    QCOMPARE(albums.front().front().toULongLong(), 42ULL);
    QVERIFY(!albums.front()[2].toString().isEmpty());
}

void MpdQueueModelTest::groupsLiveSearchAlbumsAndTracks() {
    MpdSearchResultModel model;
    QSignalSpy artwork_requests{&model, &MpdSearchResultModel::artworkRequested};
    model.setArtworkEnabled(true);
    model.replaceTracks({
        search_track("alpha-2.flac", "Alpha", "Second", "2", "release-alpha"),
        search_track("beta-1.flac", "Beta", "Other", "1", "release-beta", "1999"),
        search_track("alpha-1.flac", "Alpha", "First", "1", "release-alpha"),
    });

    QCOMPARE(model.rowCount(), 7);
    QCOMPARE(model.columnCount(), MpdSearchResultModel::column_count);
    QCOMPARE(model.kindAt(0), MpdSearchResultModel::ResultKind::section);
    QCOMPARE(model.data(model.index(0, 0)).toString(), QStringLiteral("Albums (2)"));
    QCOMPARE(model.kindAt(1), MpdSearchResultModel::ResultKind::album);
    QCOMPARE(model.data(model.index(1, 1)).toString(), QStringLiteral("Beta"));
    QCOMPARE(model.data(model.index(1, 2)).toString(), QStringLiteral("1999"));
    QCOMPARE(model.data(model.index(1, 0), MpdSearchResultModel::ArtworkUriRole).toString(),
             QStringLiteral("beta-1.flac"));
    QCOMPARE(artwork_requests.size(), 1);
    QImage artwork{8, 8, QImage::Format_ARGB32_Premultiplied};
    artwork.fill(Qt::blue);
    model.acceptArtwork(artwork_requests.front().front().toULongLong(), artwork);
    QCOMPARE(artwork_requests.size(), 2); // Covers remain serial and bounded.
    QVERIFY(!model.data(model.index(1, 0), Qt::DecorationRole).value<QImage>().isNull());
    QCOMPARE(model.data(model.index(2, 1)).toString(), QStringLiteral("Alpha"));
    const auto alpha_album = model.albumAt(2);
    QVERIFY(alpha_album.has_value());
    QCOMPARE(alpha_album->release_id, std::optional<std::string>{"release-alpha"});
    QCOMPARE(alpha_album->album, std::string{"Alpha"});
    QVERIFY(model.data(model.index(2, 3)).toString().isEmpty());
    QCOMPARE(model.urisAt(2),
             QStringList({QStringLiteral("alpha-1.flac"), QStringLiteral("alpha-2.flac")}));
    QCOMPARE(model.kindAt(3), MpdSearchResultModel::ResultKind::section);
    QCOMPARE(model.data(model.index(3, 0)).toString(), QStringLiteral("Tracks (3)"));
    QCOMPARE(model.data(model.index(4, 1)).toString(), QStringLiteral("First"));
    QCOMPARE(model.firstResultRow(), 1);
    QCOMPARE(model.nextResultRow(2, 1), 4);
    QCOMPARE(model.nextResultRow(4, -1), 2);
    QCOMPARE(model.data(model.index(1, 4), Qt::ToolTipRole).toString(),
             QStringLiteral("Append to queue (Enter)"));
    QCOMPARE(model.flags(model.index(0, 0)), Qt::NoItemFlags);

    std::vector<mpd::AlbumSummary> complete_artist_catalog;
    complete_artist_catalog.reserve(34U);
    for (auto index = 0; index < 34; ++index) {
        complete_artist_catalog.push_back(mpd::AlbumSummary{
            .filter =
                mpd::AlbumFilter{
                    .release_id = "release-" + std::to_string(index),
                    .artist = "New Model Army",
                    .album = "Album " + std::to_string(index),
                    .date = "2000",
                    .artist_is_album_artist = true,
                },
            .artist = "New Model Army",
            .album = "Album " + std::to_string(index),
            .date = "2000",
            .artwork_uri = "album-" + std::to_string(index) + ".flac",
        });
    }
    complete_artist_catalog.push_back(mpd::AlbumSummary{
        .filter = mpd::AlbumFilter{.release_id = "false-positive",
                                   .artist = "Different Artist",
                                   .album = "Unrelated Album",
                                   .date = "2001",
                                   .artist_is_album_artist = true},
        .artist = "Different Artist",
        .album = "Unrelated Album",
        .date = "2001",
        .artwork_uri = "unrelated.flac",
    });
    complete_artist_catalog = filterAlbumSearchResults(std::move(complete_artist_catalog),
                                                       QStringLiteral("New Model Army"));
    QCOMPARE(complete_artist_catalog.size(), 34U);
    model.replaceSearchResults(
        std::move(complete_artist_catalog),
        {search_track("only-track-page-result.flac", "Album 0", "Only track", "1", "release-0")});
    QCOMPARE(model.data(model.index(0, 0)).toString(), QStringLiteral("Albums (34)"));
    QCOMPARE(model.rowCount(), 37);
}

} // namespace trackknife::quick

QTEST_GUILESS_MAIN(trackknife::quick::MpdQueueModelTest)

#include "mpd_queue_model_test.moc"
