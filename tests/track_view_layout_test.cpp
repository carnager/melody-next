// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/track_view_layout.hpp"

#include <QTest>

#include <algorithm>

namespace trackknife::ui {

class TrackViewLayoutTest final : public QObject {
    Q_OBJECT

  private slots:
    void roundTripsPresentationAndColumns();
    void appendsNewlyRegisteredColumnsHidden();
    void migratesAlbumViewsToLeaveRepeatsToTheHeader();
    void rejectsInvalidLayouts_data();
    void rejectsInvalidLayouts();
};

void TrackViewLayoutTest::roundTripsPresentationAndColumns() {
    const TrackViewLayout expected{
        .schema_version = track_view_layout_schema_version,
        .presentation = TrackViewPresentation::albums_side_artwork,
        .columns = {{.id = QStringLiteral("artwork"), .width = 118, .visible = true},
                    {.id = QStringLiteral("artist"), .width = 190, .visible = true},
                    {.id = QStringLiteral("title"), .width = 280, .visible = true},
                    {.id = QStringLiteral("date"), .width = 72, .visible = false}},
    };
    QString error;
    const auto decoded =
        deserializeTrackViewLayout(serializeTrackViewLayout(expected),
                                   {QStringLiteral("artwork"), QStringLiteral("artist"),
                                    QStringLiteral("title"), QStringLiteral("date")},
                                   &error);
    QVERIFY2(decoded.has_value(), qPrintable(error));
    QCOMPARE(*decoded, expected);
}

void TrackViewLayoutTest::appendsNewlyRegisteredColumnsHidden() {
    // ADR-0179: a layout saved before a column was registered migrates by
    // appending the new column hidden instead of falling back wholesale.
    QString error;
    const auto decoded = deserializeTrackViewLayout(
        QByteArray{R"({"schema":1,"presentation":"plain-columns",)"
                   R"("columns":[{"id":"artist","width":100,"visible":true}]})"},
        {QStringLiteral("artist"), QStringLiteral("rating"), QStringLiteral("play-count"),
         QStringLiteral("last-played")},
        &error);
    QVERIFY2(decoded.has_value(), qPrintable(error));
    QCOMPARE(decoded->columns.size(), 4U);
    QCOMPARE(decoded->columns[1].id, QStringLiteral("rating"));
    QCOMPARE(decoded->columns[2].id, QStringLiteral("play-count"));
    QCOMPARE(decoded->columns[3].id, QStringLiteral("last-played"));
    for (std::size_t n = 1; n < decoded->columns.size(); ++n)
        QVERIFY(!decoded->columns[n].visible);
}

void TrackViewLayoutTest::migratesAlbumViewsToLeaveRepeatsToTheHeader() {
    const QStringList registered{QStringLiteral("artwork"), QStringLiteral("artist"),
                                 QStringLiteral("title"),   QStringLiteral("album"),
                                 QStringLiteral("date"),    QStringLiteral("rating")};
    const QByteArray columns{
        R"("columns":[{"id":"artwork","width":110,"visible":true},)"
        R"({"id":"artist","width":150,"visible":true},{"id":"title","width":220,"visible":true},)"
        R"({"id":"album","width":160,"visible":true},{"id":"date","width":64,"visible":true},)"
        R"({"id":"rating","width":84,"visible":true}]})"};
    // A version 1 album view: artist, album and date go; the rest stays.
    auto album = deserializeTrackViewLayout(
        R"({"schema":1,"presentation":"albums-side-artwork",)" + columns, registered);
    QVERIFY(album.has_value());
    QCOMPARE(album->schema_version, track_view_layout_schema_version);
    QStringList shown;
    for (const auto& column : album->columns) {
        if (column.visible) {
            shown << column.id;
        }
    }
    QCOMPARE(shown, (QStringList{QStringLiteral("artwork"), QStringLiteral("title"),
                                 QStringLiteral("rating")}));
    // Other presentations keep their columns, and so does a current album
    // view, where the columns were asked for.
    for (const auto& json :
         {QByteArray{R"({"schema":1,"presentation":"plain-columns",)"} + columns,
          QByteArray{R"({"schema":2,"presentation":"albums-side-artwork",)"} + columns}) {
        const auto kept = deserializeTrackViewLayout(json, registered);
        QVERIFY(kept.has_value());
        QVERIFY(std::ranges::all_of(kept->columns, &TrackViewColumnLayout::visible));
    }
}

void TrackViewLayoutTest::rejectsInvalidLayouts_data() {
    QTest::addColumn<QByteArray>("json");
    QTest::newRow("malformed") << QByteArray{"{"};
    QTest::newRow("future") << QByteArray{
        R"({"schema":3,"presentation":"plain-columns","columns":[]})"};
    QTest::newRow("unknown-presentation")
        << QByteArray{R"({"schema":1,"presentation":"tiles","columns":[]})"};
    QTest::newRow("duplicate-column") << QByteArray{
        R"({"schema":1,"presentation":"plain-columns","columns":[{"id":"artist","width":100,"visible":true},{"id":"artist","width":100,"visible":true}]})"};
    QTest::newRow("fractional-width") << QByteArray{
        R"({"schema":1,"presentation":"plain-columns","columns":[{"id":"artist","width":100.5,"visible":true},{"id":"title","width":100,"visible":true}]})"};
    QTest::newRow("narrow-column") << QByteArray{
        R"({"schema":1,"presentation":"plain-columns","columns":[{"id":"artist","width":1,"visible":true},{"id":"title","width":100,"visible":true}]})"};
    QTest::newRow("all-hidden") << QByteArray{
        R"({"schema":1,"presentation":"plain-columns","columns":[{"id":"artist","width":100,"visible":false},{"id":"title","width":100,"visible":false}]})"};
    QTest::newRow("unknown-column") << QByteArray{
        R"({"schema":1,"presentation":"plain-columns","columns":[{"id":"artist","width":100,"visible":true},{"id":"future","width":100,"visible":true}]})"};
}

void TrackViewLayoutTest::rejectsInvalidLayouts() {
    QFETCH(QByteArray, json);
    QString error;
    const auto decoded = deserializeTrackViewLayout(
        json, {QStringLiteral("artist"), QStringLiteral("title")}, &error);
    QVERIFY(!decoded.has_value());
    QVERIFY(!error.isEmpty());
}

} // namespace trackknife::ui

QTEST_APPLESS_MAIN(trackknife::ui::TrackViewLayoutTest)

#include "track_view_layout_test.moc"
