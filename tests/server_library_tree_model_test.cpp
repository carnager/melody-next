// SPDX-License-Identifier: GPL-3.0-only

#include "ui/server_library_tree_model.hpp"

#include <QSignalSpy>
#include <QSortFilterProxyModel>
#include <QtTest>

#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] trackknife::mpd::Track makeTrack(std::string uri, std::string artist,
                                               std::string album, std::string date,
                                               std::string disc, std::string track,
                                               std::string title) {
    using trackknife::mpd::Pair;
    std::vector<Pair> fields{
        {.name = "AlbumArtist", .value = std::move(artist)},
        {.name = "Album", .value = std::move(album)},
        {.name = "Date", .value = std::move(date)},
        {.name = "Disc", .value = std::move(disc)},
        {.name = "Track", .value = std::move(track)},
        {.name = "Title", .value = std::move(title)},
    };
    trackknife::mpd::Metadata metadata{std::move(fields)};
    return {.uri = std::move(uri),
            .metadata = metadata,
            .musicbrainz = trackknife::mpd::project_musicbrainz(metadata),
            .queue_id = std::nullopt,
            .queue_position = std::nullopt,
            .duration = std::chrono::minutes{3},
            .last_modified = std::nullopt,
            .audio_format = std::nullopt,
            .priority = std::nullopt,
            .unknown_structural_pairs = {}};
}

} // namespace

class ServerLibraryTreeModelTest final : public QObject {
    Q_OBJECT

  private slots:
    void buildsDefaultHierarchyLazily();
    void albumCountsArriveWithoutExpandingAndIgnoreStaleResults();
    void filtersDescendantsAndServerMatches();
    void roundTripsDefinition();
    void rejectsBrokenExpression();
    void expandsMultiValueGrouping();
    void ordersRootsByNewestRanking();
};

void ServerLibraryTreeModelTest::albumCountsArriveWithoutExpandingAndIgnoreStaleResults() {
    using Model = trackknife::ui::ServerLibraryTreeModel;
    Model model;
    QSignalSpy roots{&model, &Model::rootRequested};
    QSignalSpy branches{&model, &Model::branchRequested};
    model.reload();
    const auto token = roots.back().front().toULongLong();
    model.acceptRoot(token, QStringLiteral("AlbumArtist"),
                     {QStringLiteral("A"), QStringLiteral("B")}, {});
    model.acceptAlbumCounts(token, {{"A", 2}, {"B", 1}}, {});
    QCOMPARE(model.index(0, 0).data(Model::SecondaryTextRole).toString(),
             QStringLiteral("2 albums"));
    QCOMPARE(model.index(1, 0).data(Model::SecondaryTextRole).toString(),
             QStringLiteral("1 album"));
    QCOMPARE(branches.size(), 0);
    QCOMPARE(model.rowCount(model.index(0, 0)), 0);
    QVERIFY(model.canFetchMore(model.index(0, 0)));
    model.reload();
    const auto refreshed = roots.back().front().toULongLong();
    model.acceptRoot(refreshed, QStringLiteral("AlbumArtist"), {QStringLiteral("A")}, {});
    model.acceptAlbumCounts(token, {{"A", 99}}, {});
    QCOMPARE(model.index(0, 0).data(Model::SecondaryTextRole).toString(),
             QStringLiteral("Counting albums…"));
    model.acceptAlbumCounts(refreshed, {}, QStringLiteral("Not supported"));
    QCOMPARE(model.index(0, 0).data(Model::SecondaryTextRole).toString(),
             QStringLiteral("Album count unavailable"));
}

void ServerLibraryTreeModelTest::buildsDefaultHierarchyLazily() {
    trackknife::ui::ServerLibraryTreeModel model;
    QSignalSpy roots{&model, &trackknife::ui::ServerLibraryTreeModel::rootRequested};
    QSignalSpy branches{&model, &trackknife::ui::ServerLibraryTreeModel::branchRequested};
    QSignalSpy artwork{&model, &trackknife::ui::ServerLibraryTreeModel::artworkRequested};
    model.setArtworkEnabled(true);

    model.reload();
    QCOMPARE(roots.size(), 1);
    const auto root_token = roots.front().at(0).toULongLong();
    model.acceptRoot(root_token, QStringLiteral("AlbumArtist"),
                     {QStringLiteral("Zulu"), QStringLiteral("Alpha")}, {});
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(model.index(0, 0).data().toString(), QStringLiteral("Alpha"));
    QCOMPARE(model.index(1, 0).data().toString(), QStringLiteral("Zulu"));
    QCOMPARE(model.columnCount(), 1);
    QCOMPARE(model.index(1, 0)
                 .data(trackknife::ui::ServerLibraryTreeModel::SecondaryTextRole)
                 .toString(),
             QStringLiteral("Counting albums…"));

    const auto artist = model.index(1, 0);
    QVERIFY(model.hasChildren(artist));
    QSortFilterProxyModel proxy;
    proxy.setSourceModel(&model);
    proxy.setFilterKeyColumn(0);
    proxy.setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy.setRecursiveFilteringEnabled(true);
    proxy.setFilterFixedString(QStringLiteral("Zulu"));
    const auto filtered_artist = proxy.mapFromSource(artist);
    QVERIFY(filtered_artist.isValid());
    QVERIFY(proxy.hasChildren(filtered_artist));
    QVERIFY(model.canFetchMore(artist));
    model.fetchMore(artist);
    QCOMPARE(branches.size(), 1);
    QCOMPARE(branches.front().at(1).toString(), QStringLiteral("AlbumArtist"));
    QCOMPARE(branches.front().at(2).toString(), QStringLiteral("Zulu"));
    const auto branch_token = branches.front().at(0).toULongLong();

    const std::vector<trackknife::mpd::Track> tracks{
        makeTrack("late/01.flac", "Zulu", "Late", "2022", "1", "1", "Later"),
        makeTrack("early/d2.flac", "Zulu", "Early", "2020", "2", "1", "Encore"),
        makeTrack("early/d1.flac", "Zulu", "Early", "2020", "1", "1", "Opening"),
    };
    model.acceptBranch(branch_token, tracks, {});
    QCOMPARE(artwork.size(), 1);

    QCOMPARE(model.rowCount(artist), 2);
    const auto early = model.index(0, 0, artist);
    const auto late = model.index(1, 0, artist);
    QCOMPARE(artist.data(trackknife::ui::ServerLibraryTreeModel::SecondaryTextRole).toString(),
             QStringLiteral("2 albums"));
    QVERIFY(!artist.data(trackknife::ui::ServerLibraryTreeModel::AlbumRole).toBool());
    QCOMPARE(early.data().toString(), QStringLiteral("Early (2020)"));
    QCOMPARE(late.data().toString(), QStringLiteral("Late (2022)"));
    QVERIFY(early.data(trackknife::ui::ServerLibraryTreeModel::AlbumRole).toBool());
    QCOMPARE(early.data(trackknife::ui::ServerLibraryTreeModel::SecondaryTextRole).toString(),
             QStringLiteral("2 Tracks (6:00)"));
    QCOMPARE(model.rowCount(early), 2); // Multi-disc release keeps its disc level.
    QCOMPARE(model.index(0, 0, early).data().toString(), QStringLiteral("Disc 1"));
    QCOMPARE(model.index(1, 0, early).data().toString(), QStringLiteral("Disc 2"));
    QCOMPARE(model.rowCount(late), 1); // One disc is omitted, leaving the track directly below.
    QCOMPARE(model.index(0, 0, late).data().toString(), QStringLiteral("01. Later"));
    QVERIFY(
        !model.index(0, 0, late).data(trackknife::ui::ServerLibraryTreeModel::AlbumRole).toBool());
    QCOMPARE(model.index(0, 0, late)
                 .data(trackknife::ui::ServerLibraryTreeModel::SecondaryTextRole)
                 .toString(),
             QStringLiteral("3:00"));
    QVERIFY(!model.index(0, 0, late).data(Qt::DecorationRole).value<QIcon>().isNull());
    QCOMPARE(model.tracks(early).size(), 2U);
    QCOMPARE(model.tracks(model.index(0, 0, late)).front().uri, std::string{"late/01.flac"});
    const auto artist_tracks = model.tracks(artist);
    QCOMPARE(artist_tracks.size(), 3U);
    QCOMPARE(artist_tracks[0].uri, std::string{"early/d1.flac"});
    QCOMPARE(artist_tracks[1].uri, std::string{"early/d2.flac"});
    QCOMPARE(artist_tracks[2].uri, std::string{"late/01.flac"});
    QImage cover{8, 8, QImage::Format_ARGB32_Premultiplied};
    cover.fill(Qt::blue);
    model.acceptArtwork(artwork.front().front().toULongLong(), cover);
    QCOMPARE(artwork.size(), 2); // Album art is requested serially, never as a burst.
}

void ServerLibraryTreeModelTest::filtersDescendantsAndServerMatches() {
    using trackknife::ui::ServerLibraryFilterModel;
    using trackknife::ui::ServerLibraryTreeModel;
    ServerLibraryTreeModel model;
    QSignalSpy roots{&model, &ServerLibraryTreeModel::rootRequested};
    QSignalSpy branches{&model, &ServerLibraryTreeModel::branchRequested};
    model.reload();
    model.acceptRoot(roots.front().front().toULongLong(), QStringLiteral("AlbumArtist"),
                     {QStringLiteral("Alpha"), QStringLiteral("Zulu")}, {});
    QCOMPARE(model.activeRootTag(), QStringLiteral("AlbumArtist"));

    ServerLibraryFilterModel proxy;
    proxy.setSourceModel(&model);

    // A query naming an unloaded descendant matches nothing locally…
    proxy.setFilterFixedString(QStringLiteral("Nebula"));
    QCOMPARE(proxy.rowCount(), 0);

    // …until the bounded server-side search proves which roots contain
    // matching descendants; those roots stay visible with their arrows.
    proxy.setServerMatches({QStringLiteral("Zulu")});
    QCOMPARE(proxy.rowCount(), 1);
    const auto revealed = proxy.index(0, 0);
    QCOMPARE(revealed.data().toString(), QStringLiteral("Zulu"));
    QVERIFY(proxy.hasChildren(revealed));

    // Loading the branch lets the recursive filter match the album row itself,
    // and a matched album never drags its sibling albums into the results.
    const auto source_artist = proxy.mapToSource(revealed);
    model.fetchMore(source_artist);
    model.acceptBranch(
        branches.front().front().toULongLong(),
        {makeTrack("nebula/01.flac", "Zulu", "Nebula", "2021", "1", "1", "Dust"),
         makeTrack("starlight/01.flac", "Zulu", "Starlight", "2019", "1", "1", "Glow")},
        {});
    proxy.clearServerMatches();
    QCOMPARE(proxy.rowCount(), 1);
    const auto artist = proxy.index(0, 0);
    QCOMPARE(proxy.rowCount(artist), 1);
    const auto album = proxy.index(0, 0, artist);
    QCOMPARE(album.data().toString(), QStringLiteral("Nebula (2021)"));
    // The album's own tracks stay visible because track rows carry their
    // descriptive tags in the filter haystack.
    QCOMPARE(proxy.rowCount(album), 1);
    QCOMPARE(proxy.index(0, 0, album).data().toString(), QStringLiteral("01. Dust"));

    // An artist query keeps the artist's complete chain: every track carries
    // the artist tag, so albums and tracks match on their own merit.
    proxy.setFilterFixedString(QStringLiteral("Zulu"));
    QCOMPARE(proxy.rowCount(), 1);
    QCOMPARE(proxy.rowCount(proxy.index(0, 0)), 2);

    // Track titles under a loaded branch match as well, keeping only their
    // own album chain.
    proxy.setFilterFixedString(QStringLiteral("Dust"));
    QCOMPARE(proxy.rowCount(), 1);
    QCOMPARE(proxy.index(0, 0).data().toString(), QStringLiteral("Zulu"));
    QCOMPARE(proxy.rowCount(proxy.index(0, 0)), 1);

    // Secondary hint text is not searchable: "Expand to browse" must never
    // satisfy a query against the unloaded Alpha root.
    proxy.setFilterFixedString(QStringLiteral("Expand"));
    QCOMPARE(proxy.rowCount(), 0);

    // Clearing the server matches while the query has no local match hides the
    // root again, so stale reveals cannot outlive their query.
    proxy.setFilterFixedString(QStringLiteral("no-such-text"));
    QCOMPARE(proxy.rowCount(), 0);
}

void ServerLibraryTreeModelTest::roundTripsDefinition() {
    const auto expected = trackknife::ui::defaultLibraryTreeDefinition();
    QString error;
    const auto decoded = trackknife::ui::deserializeLibraryTreeDefinition(
        trackknife::ui::serializeLibraryTreeDefinition(expected), &error);
    QVERIFY2(decoded.has_value(), qPrintable(error));
    QCOMPARE(*decoded, expected);
}

void ServerLibraryTreeModelTest::rejectsBrokenExpression() {
    trackknife::ui::ServerLibraryTreeModel model;
    auto definition = trackknife::ui::defaultLibraryTreeDefinition();
    definition.levels[1].label_expression = QStringLiteral("%album");
    QVERIFY(model.setDefinition(std::move(definition)).contains(QStringLiteral("Album label")));
}

void ServerLibraryTreeModelTest::expandsMultiValueGrouping() {
    trackknife::ui::ServerLibraryTreeModel model;
    trackknife::ui::LibraryTreeDefinition definition{
        .name = QStringLiteral("Artist / genre / tracks"),
        .root_tag = QStringLiteral("Artist"),
        .levels = {{.name = QStringLiteral("Artist"),
                    .grouping_expression = QStringLiteral("%artist%"),
                    .label_expression = QStringLiteral("%artist%"),
                    .sort_expression = QStringLiteral("%artist%")},
                   {.name = QStringLiteral("Genre"),
                    .grouping_expression = QStringLiteral("$each(genre)"),
                    .label_expression = QStringLiteral("$each(genre)"),
                    .sort_expression = QStringLiteral("$each(genre)")},
                   {.name = QStringLiteral("Tracks"),
                    .grouping_expression = QStringLiteral("%uri%"),
                    .label_expression = QStringLiteral("%title%"),
                    .sort_expression = QStringLiteral("%title%")}},
    };
    QVERIFY2(model.setDefinition(std::move(definition)).isEmpty(), "definition must compile");
    QSignalSpy roots{&model, &trackknife::ui::ServerLibraryTreeModel::rootRequested};
    QSignalSpy branches{&model, &trackknife::ui::ServerLibraryTreeModel::branchRequested};
    model.reload();
    model.acceptRoot(roots.front().front().toULongLong(), QStringLiteral("Artist"),
                     {QStringLiteral("Artist")}, {});
    const auto artist = model.index(0, 0);
    model.fetchMore(artist);

    using trackknife::mpd::Pair;
    std::vector<Pair> fields{{.name = "Artist", .value = "Artist"},
                             {.name = "Genre", .value = "Rock"},
                             {.name = "Genre", .value = "Alternative"},
                             {.name = "Title", .value = "Both"}};
    trackknife::mpd::Metadata metadata{std::move(fields)};
    const trackknife::mpd::Track track{
        .uri = "both.flac",
        .metadata = metadata,
        .musicbrainz = trackknife::mpd::project_musicbrainz(metadata),
        .queue_id = std::nullopt,
        .queue_position = std::nullopt,
        .duration = std::nullopt,
        .last_modified = std::nullopt,
        .audio_format = std::nullopt,
        .priority = std::nullopt,
        .unknown_structural_pairs = {},
    };
    model.acceptBranch(branches.front().front().toULongLong(), {track}, {});
    QCOMPARE(model.rowCount(artist), 2);
    QCOMPARE(model.index(0, 0, artist).data().toString(), QStringLiteral("Alternative"));
    QCOMPARE(model.index(1, 0, artist).data().toString(), QStringLiteral("Rock"));
    QCOMPARE(model.tracks(model.index(0, 0, artist)).front().uri, std::string{"both.flac"});
    QCOMPARE(model.tracks(model.index(1, 0, artist)).front().uri, std::string{"both.flac"});
}

void ServerLibraryTreeModelTest::ordersRootsByNewestRanking() {
    trackknife::ui::ServerLibraryTreeModel model;
    QSignalSpy roots{&model, &trackknife::ui::ServerLibraryTreeModel::rootRequested};
    model.reload();
    QCOMPARE(roots.size(), 1);
    model.acceptRoot(roots.front().at(0).toULongLong(), QStringLiteral("AlbumArtist"),
                     {QStringLiteral("Zulu"), QStringLiteral("Alpha"), QStringLiteral("Mike")}, {});
    QCOMPARE(model.index(0, 0).data().toString(), QStringLiteral("Alpha"));

    // Ranked values lead in rank order; unranked artists follow
    // alphabetically.
    model.setRootOrdering({QStringLiteral("Mike"), QStringLiteral("Zulu")});
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(model.index(0, 0).data().toString(), QStringLiteral("Mike"));
    QCOMPARE(model.index(1, 0).data().toString(), QStringLiteral("Zulu"));
    QCOMPARE(model.index(2, 0).data().toString(), QStringLiteral("Alpha"));

    // A later root load keeps the Latest ordering applied.
    model.reload();
    QCOMPARE(roots.size(), 2);
    model.acceptRoot(roots.back().at(0).toULongLong(), QStringLiteral("AlbumArtist"),
                     {QStringLiteral("Alpha"), QStringLiteral("Mike")}, {});
    QCOMPARE(model.index(0, 0).data().toString(), QStringLiteral("Mike"));
    QCOMPARE(model.index(1, 0).data().toString(), QStringLiteral("Alpha"));

    model.clearRootOrdering();
    QCOMPARE(model.index(0, 0).data().toString(), QStringLiteral("Alpha"));
    QCOMPARE(model.index(1, 0).data().toString(), QStringLiteral("Mike"));
}

QTEST_MAIN(ServerLibraryTreeModelTest)
#include "server_library_tree_model_test.moc"
