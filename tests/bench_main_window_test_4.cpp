// SPDX-License-Identifier: GPL-3.0-only

#include "bench_main_window_test.hpp"

#include "uicommon/library_tree_view.hpp"

#include <QPainter>
#include <QStandardItemModel>

namespace trackknife::bench {

// The desktop says dark only after the window is up, and the scheme's
// palette comes later: the status bar's painted icons -- album shuffle --
// are painted again in its colours, not left in the light palette's dark.
// ADR-0311: Identify… from the Edit menu and a list's Tools: the tag editor
// on the selected tracks, or with none selected the whole list, Identify
// open in it once the files are read.
void BenchMainWindowTest::identifyOpensTheTagEditorWithIdentify() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    std::vector<std::string> paths;
    for (const auto* name : {"one.flac", "two.flac"}) {
        const auto path = media.filePath(QLatin1String(name));
        QVERIFY(materialize_audio_fixture(QStringLiteral("tagged-tone-flac.b64"), path));
        paths.push_back(QFile::encodeName(path).toStdString());
    }
    BenchMainWindow window;
    window.setProperty("trackknife-identify-hold-lookup", true);
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    window.openLocalPaths(paths);
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    auto* list_model = qobject_cast<LocalListModel*>(view->model());
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rowCount(), 2, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    auto* identify = window.findChild<QAction*>(QStringLiteral("action-identify"));
    QVERIFY(identify != nullptr);
    QCOMPARE(identify->text(), QStringLiteral("Identify…"));
    QVERIFY(window.findChild<QAction*>(QStringLiteral("action-identify-albums")) == nullptr);
    view->selectionModel()->clearSelection();
    window.refreshSelectionStatus();
    QTRY_VERIFY(identify->isEnabled());

    const auto open = [&window, identify](const QString& title) {
        identify->trigger();
        MetadataPropertiesDialog* editor = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                for (auto* candidate : window.findChildren<MetadataPropertiesDialog*>()) {
                    if (candidate->isVisible()) {
                        editor = candidate;
                    }
                }
                return editor != nullptr;
            }(),
            10'000);
        QCOMPARE(editor->windowTitle(), title);
        QVERIFY(editor->findChild<QPushButton*>(
                    QStringLiteral("bench-metadata-identify-albums")) == nullptr);
        QDialog* dialog = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((dialog = editor->findChild<QDialog*>(
                                      QStringLiteral("bench-musicbrainz-identify"))) != nullptr &&
                                     dialog->isVisible(),
                                 10'000);
        dialog->close();
        editor->close();
        QTRY_VERIFY(!editor->isVisible());
    };
    // None selected: the whole list.
    open(QStringLiteral("Edit tags · 2 tracks"));
    if (QTest::currentTestFailed()) {
        return;
    }
    // One selected: that one.
    view->selectionModel()->select(list_model->index(1, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    open(QStringLiteral("Edit tags · 1 track"));
}

void BenchMainWindowTest::paintedStatusIconsFollowThePalette() {
    const auto before = QGuiApplication::palette();
    BenchMainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* album_shuffle = window.findChild<QAction*>(QStringLiteral("action-local-album-random"));
    QVERIFY(album_shuffle != nullptr);
    // The colour of the icon's strokes: its most opaque pixel.
    const auto ink = [album_shuffle] {
        const auto image = album_shuffle->icon().pixmap(24, 24).toImage();
        QColor strongest{Qt::transparent};
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const auto pixel = image.pixelColor(x, y);
                if (pixel.alpha() > strongest.alpha()) {
                    strongest = pixel;
                }
            }
        }
        return strongest;
    };
    auto later = before;
    later.setColor(QPalette::Window, QColor{0x1e, 0x1f, 0x22});
    later.setColor(QPalette::ButtonText, QColor{0xff, 0x00, 0x00});
    QGuiApplication::setPalette(later);
    QTRY_VERIFY2(ink().red() > 200 && ink().green() < 60 && ink().blue() < 60,
                 qPrintable(ink().name(QColor::HexArgb)));
    QGuiApplication::setPalette(before);
}

void BenchMainWindowTest::musicBrainzIdentifyStagesChosenVersion_data() {
    QTest::addColumn<bool>("untagged");
    QTest::newRow("tagged") << false;
    QTest::newRow("untagged-manual-mapping") << true;
}

void BenchMainWindowTest::musicBrainzIdentifyStagesChosenVersion() {
    QFETCH(bool, untagged);
    // Its covers are another test's (ADR-0316).
    QSettings{}.setValue(QStringLiteral("identify/fetch-covers"), false);
    const auto field = [](std::string name, std::vector<std::string> values) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = std::move(values),
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    const auto make_source = [&field, untagged](const QString& label, const QString& title,
                                                const QString& track_number) {
        return MetadataPropertiesSource{
            .source =
                metadata::StagedMetadataSource{
                    .raw_path = "/music/" + label.toStdString() + ".flac",
                    .source_revision = std::nullopt,
                    .baseline =
                        metadata::MetadataDocument{
                            .fields = untagged ? std::vector<metadata::MetadataField>{}
                                               : std::vector{field("ALBUM", {"Alpha"}),
                                                             field("ARTIST", {"Band"}),
                                                             field("TITLE", {title.toStdString()}),
                                                             field("TRACKNUMBER",
                                                                   {track_number.toStdString()})},
                            .unsupported_native_objects = {},
                        },
                },
            .track_label = label,
        };
    };
    const std::vector sources{
        make_source(untagged ? QStringLiteral("10-second") : QStringLiteral("one"),
                    QStringLiteral("One"), QStringLiteral("1")),
        make_source(untagged ? QStringLiteral("2-first") : QStringLiteral("two"),
                    QStringLiteral("Two"), QStringLiteral("2"))};

    static constexpr auto search_body = R"json({
      "count": 1,
      "releases": [{
        "id": "11111111-2222-3333-4444-555555555555",
        "score": 100, "title": "Alpha", "status": "Official",
        "date": "1999-09-09", "country": "DE", "track-count": 2,
        "artist-credit": [{"name": "Band",
          "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "name": "Band"}}],
        "release-group": {"id": "99999999-8888-7777-6666-555555555555"},
        "media": [{"format": "CD", "track-count": 2}]
      }]
    })json";
    static constexpr auto lookup_body = R"json({
      "id": "11111111-2222-3333-4444-555555555555",
      "title": "Alpha", "status": "Official", "date": "1999-09-09", "country": "DE",
      "release-group": {"id": "99999999-8888-7777-6666-555555555555"},
      "artist-credit": [{"name": "Band",
        "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "name": "Band"}}],
      "media": [{"position": 1, "format": "CD", "track-count": 2, "tracks": [
        {"id": "aaaa1111-0000-0000-0000-000000000001", "position": 1, "number": "1",
         "title": "One", "length": 61000,
         "recording": {"id": "bbbb1111-0000-0000-0000-000000000001", "title": "One"}},
        {"id": "aaaa1111-0000-0000-0000-000000000002", "position": 2, "number": "2",
         "title": "Two", "length": 59000,
         "recording": {"id": "bbbb1111-0000-0000-0000-000000000002", "title": "Two"}}
      ]}]
    })json";
    int fetches = 0;
    QString search_url;
    const MusicBrainzLookupService service{
        .fetch =
            [&fetches, &search_url](const QString& url,
                                    std::function<void(core::Result<QByteArray>)> completion) {
                ++fetches;
                if (url.contains(QStringLiteral("?query="))) {
                    search_url = QUrl::fromPercentEncoding(url.toUtf8());
                    completion(QByteArray{search_body});
                    return;
                }
                if (url.contains(QStringLiteral("11111111-2222-3333-4444-555555555555?inc="))) {
                    completion(QByteArray{lookup_body});
                    return;
                }
                completion(std::unexpected(core::Error{
                    .code = core::ErrorCode::invalid_argument,
                    .message = "unexpected url",
                    .context = {},
                }));
            },
        .fingerprint = {},
        .acoustid_lookup = {},
    };

    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {}, {}, {}, {}, {}, {}, {}, nullptr, {}, service);
    properties->show();

    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* identify = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-identify"));
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    QVERIFY(grid_model != nullptr);
    QVERIFY(identify != nullptr);
    QVERIFY(status != nullptr);
    files->selectAll();
    QTRY_VERIFY(identify->isEnabled());
    QTest::mouseClick(identify, Qt::LeftButton);

    // The in-app search surface opens pre-filled from the selection — no
    // MusicBrainz tags exist anywhere on these files.
    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = properties->findChild<QDialog*>(
                     QStringLiteral("bench-musicbrainz-identify"))) != nullptr);
    auto* artist =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-musicbrainz-identify-artist"));
    auto* album =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-musicbrainz-identify-release"));
    auto* search =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-search"));
    auto* results =
        dialog->findChild<QTreeWidget*>(QStringLiteral("bench-musicbrainz-identify-results"));
    auto* use = dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-apply"));
    QVERIFY(artist && album);
    QCOMPARE(artist->text(), untagged ? QString{} : QStringLiteral("Band"));
    QCOMPARE(album->text(), untagged ? QString{} : QStringLiteral("Alpha"));
    artist->setText(QStringLiteral("Band"));
    album->setText(QStringLiteral("Alpha"));
    QVERIFY(search != nullptr);
    QVERIFY(results != nullptr);
    QVERIFY(use != nullptr);

    QTest::mouseClick(search, Qt::LeftButton);
    QTRY_COMPARE(results->topLevelItemCount(), 1);
    QVERIFY(search_url.contains(QStringLiteral("artist:")));
    QVERIFY(search_url.contains(QStringLiteral("release:")));
    QVERIFY(!search_url.contains(QStringLiteral("tracks:")));
    QCOMPARE(results->topLevelItem(0)->text(0), QStringLiteral("Band — Alpha"));
    QVERIFY(results->topLevelItem(0)
                ->data(0, Qt::UserRole + 1)
                .toString()
                .contains(QStringLiteral("1999-09-09")));
    // The version chosen is previewed beneath it: its pairing, to change
    // there; Apply stages it (ADR-0311).
    QTRY_VERIFY(dialog->findChild<QListWidget*>(QStringLiteral("bench-musicbrainz-match-board")) !=
                nullptr);
    auto* stage_matches = use;
    if (untagged) {
        auto* board =
            dialog->findChild<QListWidget*>(QStringLiteral("bench-musicbrainz-match-board"));
        auto* up = dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-up"));
        auto* down =
            dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-down"));
        auto* unmatch =
            dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-unmatch"));
        auto* sort =
            dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-sort"));
        QVERIFY(board && up && down && unmatch && sort);
        QTRY_COMPARE(matchRowCount(board), 2);
        sort->click();
        QVERIFY(matchRow(board, 0)->text().endsWith(QStringLiteral("2-first.flac")));
        const auto first_album_track = matchTrack(matchRow(board, 0));
        const auto second_album_track = matchTrack(matchRow(board, 1));
        QVERIFY(first_album_track.contains(QStringLiteral("One")));
        QVERIFY(second_album_track.contains(QStringLiteral("Two")));
        board->setCurrentItem(matchRow(board, 0));
        QVERIFY(!up->isEnabled());
        QVERIFY(down->isEnabled());
        QVERIFY(matchRow(board, 0)->toolTip().contains(QStringLiteral("/music/2-first.flac")));
        // A file carried between files, by the board's own drop handler and
        // the payload its model makes: dropped below the second file.
        std::unique_ptr<QMimeData> mime{board->model()->mimeData(
            {board->indexFromItem(matchRow(board, 0))})};
        const auto rect = board->visualItemRect(matchRow(board, 1));
        const QPoint position{rect.left() + 30, rect.bottom() - 1};
        QDragEnterEvent enter{dragPosition(position), Qt::MoveAction, mime.get(), Qt::LeftButton,
                              Qt::NoModifier};
        QApplication::sendEvent(board->viewport(), &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop{QPointF{position}, Qt::MoveAction, mime.get(), Qt::LeftButton,
                        Qt::NoModifier};
        QApplication::sendEvent(board->viewport(), &drop);
        QVERIFY(drop.isAccepted());
        // A payload from before the board changed is not taken.
        QDragEnterEvent stale{dragPosition(position), Qt::MoveAction, mime.get(), Qt::LeftButton,
                              Qt::NoModifier};
        QApplication::sendEvent(board->viewport(), &stale);
        QVERIFY(!stale.isAccepted());
        QVERIFY(matchRow(board, 1)->text().endsWith(QStringLiteral("2-first.flac")));
        QCOMPARE(matchTrack(matchRow(board, 0)), first_album_track);
        QCOMPARE(matchTrack(matchRow(board, 1)), second_album_track);
        QVERIFY(!down->isEnabled());
        board->setFocus();
        QTest::keyClick(board, Qt::Key_Up, Qt::AltModifier);
        QVERIFY(matchRow(board, 0)->text().endsWith(QStringLiteral("2-first.flac")));
        unmatch->click();
        QCOMPARE(matchRowCount(board), 3);
        QVERIFY(matchRow(board, 0)->text().isEmpty());
        QVERIFY(matchTrack(matchRow(board, 2)).contains(QStringLiteral("No tags")));
        QVERIFY(!unmatch->isEnabled());
        up->click();
        up->click();
        QCOMPARE(matchRowCount(board), 3);
        // ADR-0330: the file on no track, put back on the empty track.
        matchClickOnto(board, 2, 0);
        QTRY_COMPARE(matchRowCount(board), 2);
        QVERIFY(matchRow(board, 0)->text().endsWith(QStringLiteral("2-first.flac")));
        QCOMPARE(matchTrack(matchRow(board, 0)), first_album_track);
        QCOMPARE(matchTrack(matchRow(board, 1)), second_album_track);
    }
    QTRY_VERIFY(stage_matches->isEnabled());
    const auto screenshot_dir = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
    if (untagged && !screenshot_dir.isEmpty()) {
        QVERIFY(dialog->grab().save(screenshot_dir + QStringLiteral("/musicbrainz-matching.png")));
    }
    QTest::mouseClick(stage_matches, Qt::LeftButton);

    // The chosen version stages as one ordinary colored draft transaction.
    QTRY_VERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-musicbrainz-identify")) ==
                nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(grid_model->patches().patch_count() > 0U, 5'000);
    QTRY_VERIFY(status->text().contains(QStringLiteral("MusicBrainz")));
    // Tagged, searched by its tags as it opened too, and its best version
    // previewed: its track list (ADR-0311).
    QCOMPARE(fetches, untagged ? 2 : 4);
    if (untagged) {
        const auto title = grid_model->fieldColumn(QStringLiteral("Title"));
        QVERIFY(title);
        QCOMPARE(grid_model->index(0, *title).data(metadata_cell_values_role).toStringList(),
                 QStringList{QStringLiteral("Two")});
        QCOMPARE(grid_model->index(1, *title).data(metadata_cell_values_role).toStringList(),
                 QStringList{QStringLiteral("One")});
    }
    const auto album_id_column = grid_model->fieldColumn(QStringLiteral("MUSICBRAINZ_ALBUMID"));
    QVERIFY(album_id_column.has_value());
    QCOMPARE(
        grid_model->index(0, *album_id_column).data(metadata_cell_staged_source_role).toString(),
        QStringLiteral("MusicBrainz"));
    const auto date_column = grid_model->fieldColumn(QStringLiteral("Date"));
    QVERIFY(album_id_column.has_value());
    QVERIFY(date_column.has_value());
    for (int row = 0; row < grid_model->rowCount(); ++row) {
        QCOMPARE(
            grid_model->index(row, *album_id_column).data(metadata_cell_values_role).toStringList(),
            QStringList{QStringLiteral("11111111-2222-3333-4444-555555555555")});
        QCOMPARE(
            grid_model->index(row, *date_column).data(metadata_cell_values_role).toStringList(),
            QStringList{QStringLiteral("1999-09-09")});
    }
    delete properties;
}

// ADR-0310: the release the files are tagged with, looked up as Identify
// opens; and a release named by its link where the album is typed, looked
// up rather than searched for -- each with its album's other versions
// beneath it, to change to; an album's link lists them all.
void BenchMainWindowTest::musicBrainzIdentifyLooksUpNamedReleases() {
    const auto field = [](std::string name, std::vector<std::string> values) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = std::move(values),
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    const auto make_source = [&field](const QString& title, const QString& track_number) {
        return MetadataPropertiesSource{
            .source =
                metadata::StagedMetadataSource{
                    .raw_path = "/music/" + title.toLower().toStdString() + ".flac",
                    .source_revision = std::nullopt,
                    .baseline =
                        metadata::MetadataDocument{
                            .fields = {field("ALBUM", {"Alpha"}), field("ARTIST", {"Band"}),
                                       field("TITLE", {title.toStdString()}),
                                       field("TRACKNUMBER", {track_number.toStdString()}),
                                       field("MUSICBRAINZ_ALBUMID",
                                             {"11111111-2222-3333-4444-555555555555"})},
                            .unsupported_native_objects = {},
                        },
                },
            .track_label = title,
        };
    };
    const std::vector sources{make_source(QStringLiteral("One"), QStringLiteral("1")),
                              make_source(QStringLiteral("Two"), QStringLiteral("2"))};
    const auto release_body = [](const char* id, const char* title) {
        return QStringLiteral(R"json({
          "id": "%1", "title": "%2", "status": "Official", "date": "1999-09-09",
          "country": "DE", "release-group": {"id": "99999999-8888-7777-6666-555555555555"},
          "artist-credit": [{"name": "Band",
            "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "name": "Band"}}],
          "media": [{"position": 1, "format": "CD", "track-count": 2, "tracks": [
            {"id": "aaaa1111-0000-0000-0000-000000000001", "position": 1, "number": "1",
             "title": "One", "length": 61000,
             "recording": {"id": "bbbb1111-0000-0000-0000-000000000001", "title": "One"}},
            {"id": "aaaa1111-0000-0000-0000-000000000002", "position": 2, "number": "2",
             "title": "Two", "length": 59000,
             "recording": {"id": "bbbb1111-0000-0000-0000-000000000002", "title": "Two"}}
          ]}]
        })json")
            .arg(QLatin1String{id}, QLatin1String{title})
            .toUtf8();
    };
    // The album's versions: the two above, and one of three tracks.
    static constexpr auto group_body = R"json({
      "release-count": 3, "release-offset": 0,
      "releases": [
        {"id": "33333333-2222-3333-4444-555555555555", "title": "Alpha", "date": "1998",
         "media": [{"format": "Vinyl", "track-count": 3}]},
        {"id": "22222222-2222-3333-4444-555555555555", "title": "Alpha (Deluxe)",
         "date": "2005", "media": [{"format": "CD", "track-count": 2}]},
        {"id": "11111111-2222-3333-4444-555555555555", "title": "Alpha", "date": "1999-09-09",
         "media": [{"format": "CD", "track-count": 2}]}
      ]
    })json";
    QStringList asked;
    const MusicBrainzLookupService service{
        .fetch =
            [&asked, &release_body](const QString& url,
                                    std::function<void(core::Result<QByteArray>)> completion) {
                asked << url.section(QLatin1Char('?'), 0, 0).section(QLatin1Char('/'), -1);
                if (url.contains(QStringLiteral(
                        "release?release-group=99999999-8888-7777-6666-555555555555&"))) {
                    completion(QByteArray{group_body});
                    return;
                }
                if (url.contains(QStringLiteral("11111111-2222-3333-4444-555555555555?inc="))) {
                    completion(release_body("11111111-2222-3333-4444-555555555555", "Alpha"));
                    return;
                }
                if (url.contains(QStringLiteral("22222222-2222-3333-4444-555555555555?inc="))) {
                    completion(
                        release_body("22222222-2222-3333-4444-555555555555", "Alpha (Deluxe)"));
                    return;
                }
                completion(std::unexpected(core::Error{
                    .code = core::ErrorCode::invalid_argument,
                    .message = "unexpected url",
                    .context = {},
                }));
            },
        .fingerprint = {},
        .acoustid_lookup = {},
    };

    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {}, {}, {}, {}, {}, {}, {}, nullptr, {}, service);
    properties->show();
    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* identify = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-identify"));
    QVERIFY(grid_model != nullptr && identify != nullptr);
    files->selectAll();
    QTRY_VERIFY(identify->isEnabled());
    QTest::mouseClick(identify, Qt::LeftButton);
    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = properties->findChild<QDialog*>(
                     QStringLiteral("bench-musicbrainz-identify"))) != nullptr);
    auto* album =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-musicbrainz-identify-release"));
    auto* search =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-search"));
    auto* results =
        dialog->findChild<QTreeWidget*>(QStringLiteral("bench-musicbrainz-identify-results"));
    auto* use = dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-apply"));
    QVERIFY(album && search && results && use);

    const auto rows = [results] {
        QStringList shown;
        for (int row = 0; row < results->topLevelItemCount(); ++row) {
            // The album, and how it was found and how many tracks it has.
            shown << results->topLevelItem(row)->text(0).section(QStringLiteral(" — "), -1) +
                         QStringLiteral(": ") +
                         results->topLevelItem(row)
                             ->data(0, Qt::UserRole + 1)
                             .toString()
                             .split(QStringLiteral(" · "))
                             .filter(QRegularExpression{QStringLiteral(
                                 "^(Tagged release|Release named|same track count|\\d+ tracks?)$")})
                             .join(QStringLiteral(" · "));
        }
        return shown;
    };
    // The tagged release, looked up by its id -- no search -- and its
    // album's other versions beneath it, those of as many tracks first.
    QTRY_VERIFY(search->isEnabled() && results->topLevelItemCount() == 3);
    QCOMPARE(rows(), (QStringList{QStringLiteral("Alpha: Tagged release · 2 tracks"),
                                  QStringLiteral("Alpha (Deluxe): same track count · 2 tracks"),
                                  QStringLiteral("Alpha: 3 tracks")}));
    QCOMPARE(asked, (QStringList{QStringLiteral("11111111-2222-3333-4444-555555555555"),
                                 QStringLiteral("release")}));
    QCOMPARE(results->currentItem(), results->topLevelItem(0));

    // Another, by its link where the album is typed.
    album->setText(QStringLiteral(
        " https://musicbrainz.org/release/22222222-2222-3333-4444-555555555555/discids "));
    QTest::mouseClick(search, Qt::LeftButton);
    QTRY_VERIFY(search->isEnabled() && results->topLevelItemCount() == 3);
    QCOMPARE(rows(), (QStringList{QStringLiteral("Alpha (Deluxe): Release named · 2 tracks"),
                                  QStringLiteral("Alpha: same track count · 2 tracks"),
                                  QStringLiteral("Alpha: 3 tracks")}));
    QCOMPARE(asked.size(), 4);
    QCOMPARE(asked[2], QStringLiteral("22222222-2222-3333-4444-555555555555"));

    // The album by its link: every version of it.
    album->setText(QStringLiteral(
        "https://musicbrainz.org/release-group/99999999-8888-7777-6666-555555555555"));
    QTest::mouseClick(search, Qt::LeftButton);
    // Listed, and the first previewed: its track list.
    QTRY_VERIFY(search->isEnabled() && asked.size() == 6);
    QTRY_COMPARE(rows(), (QStringList{QStringLiteral("Alpha: same track count · 2 tracks"),
                                      QStringLiteral("Alpha (Deluxe): same track count · 2 tracks"),
                                      QStringLiteral("Alpha: 3 tracks")}));

    // Another version chosen, previewed and applied as a found one is.
    results->setCurrentItem(results->topLevelItem(1));
    QTRY_VERIFY(dialog->findChild<QLabel*>(QStringLiteral("bench-musicbrainz-identify-match-heading"))
                    ->text()
                    .contains(QStringLiteral("Alpha (Deluxe)")));
    QTRY_VERIFY(use->isEnabled());
    QTest::mouseClick(use, Qt::LeftButton);
    QTRY_VERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-musicbrainz-identify")) ==
                nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(grid_model->patches().patch_count() > 0U, 5'000);
    const auto album_column = grid_model->fieldColumn(QStringLiteral("Album"));
    QVERIFY(album_column.has_value());
    QCOMPARE(grid_model->index(0, *album_column).data(metadata_cell_values_role).toStringList(),
             QStringList{QStringLiteral("Alpha (Deluxe)")});
    delete properties;
}

// ADR-0311: several albums chosen are identified one at a time -- each its
// own search, versions and matching -- beside a list of them; those ahead
// looked up meanwhile; each one's pairing chosen, or the album skipped, and
// the next shown; Apply stages every album chosen.
void BenchMainWindowTest::musicBrainzIdentifyStepsThroughAlbums() {
    const auto field = [](std::string name, std::vector<std::string> values) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = std::move(values),
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    const auto make_source = [&field](const std::string& album, const std::string& title,
                                      const std::string& track, const std::string& release) {
        std::vector fields{field("ALBUM", {album}), field("ARTIST", {"Band"}),
                           field("TITLE", {title}), field("TRACKNUMBER", {track})};
        if (!release.empty()) {
            fields.push_back(field("MUSICBRAINZ_ALBUMID", {release}));
        }
        return MetadataPropertiesSource{
            .source =
                metadata::StagedMetadataSource{
                    .raw_path = "/music/" + album + "/" + track + ".flac",
                    .source_revision = std::nullopt,
                    .baseline = metadata::MetadataDocument{.fields = std::move(fields),
                                                           .unsupported_native_objects = {}},
                },
            .track_label = QString::fromStdString(title),
        };
    };
    const std::string alpha_id = "11111111-2222-3333-4444-555555555555";
    const std::vector sources{make_source("Alpha", "One", "1", alpha_id),
                              make_source("Alpha", "Two", "2", alpha_id),
                              make_source("Beta", "Three", "1", {}),
                              make_source("Beta", "Four", "2", {})};
    const auto release_body = [](const char* id, const char* title, const char* group,
                                 const char* first, const char* second) {
        return QStringLiteral(R"json({
          "id": "%1", "title": "%2", "status": "Official", "date": "1999-09-09",
          "country": "DE", "release-group": {"id": "%3"},
          "artist-credit": [{"name": "Band",
            "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "name": "Band"}}],
          "media": [{"position": 1, "format": "CD", "track-count": 2, "tracks": [
            {"id": "aaaa1111-0000-0000-0000-000000000001", "position": 1, "number": "1",
             "title": "%4", "length": 61000,
             "recording": {"id": "bbbb1111-0000-0000-0000-000000000001", "title": "%4"}},
            {"id": "aaaa1111-0000-0000-0000-000000000002", "position": 2, "number": "2",
             "title": "%5", "length": 59000,
             "recording": {"id": "bbbb1111-0000-0000-0000-000000000002", "title": "%5"}}
          ]}]
        })json")
            .arg(QLatin1String{id}, QLatin1String{title}, QLatin1String{group},
                 QLatin1String{first}, QLatin1String{second})
            .toUtf8();
    };
    static constexpr auto beta_search = R"json({
      "count": 1,
      "releases": [{
        "id": "44444444-2222-3333-4444-555555555555",
        "score": 100, "title": "Beta", "status": "Official", "track-count": 2,
        "artist-credit": [{"name": "Band",
          "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "name": "Band"}}],
        "release-group": {"id": "88888888-8888-7777-6666-555555555555"},
        "media": [{"format": "CD", "track-count": 2}]
      }]
    })json";
    QStringList asked;
    const MusicBrainzLookupService service{
        .fetch =
            [&asked, &release_body](const QString& url,
                                    std::function<void(core::Result<QByteArray>)> completion) {
                const auto plain = QUrl::fromPercentEncoding(url.toUtf8());
                if (plain.contains(QStringLiteral("?query="))) {
                    asked << QStringLiteral("search");
                    completion(plain.contains(QStringLiteral("Beta")) ? QByteArray{beta_search}
                                                                      : QByteArray{});
                    return;
                }
                if (plain.contains(QStringLiteral("release?release-group="))) {
                    asked << QStringLiteral("versions");
                    completion(QByteArray{R"json({"release-count": 0, "releases": []})json"});
                    return;
                }
                if (plain.contains(QStringLiteral("11111111-2222-3333-4444-555555555555?inc="))) {
                    asked << QStringLiteral("alpha");
                    completion(release_body("11111111-2222-3333-4444-555555555555", "Alpha",
                                            "99999999-8888-7777-6666-555555555555", "One",
                                            "Two"));
                    return;
                }
                if (plain.contains(QStringLiteral("44444444-2222-3333-4444-555555555555?inc="))) {
                    asked << QStringLiteral("beta");
                    completion(release_body("44444444-2222-3333-4444-555555555555", "Beta",
                                            "88888888-8888-7777-6666-555555555555", "Three",
                                            "Four"));
                    return;
                }
                completion(std::unexpected(core::Error{
                    .code = core::ErrorCode::invalid_argument,
                    .message = "unexpected url",
                    .context = {},
                }));
            },
        .fingerprint = {},
        .acoustid_lookup = {},
    };

    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {}, {}, {}, {}, {}, {}, {}, nullptr, {}, service);
    properties->show();
    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* identify = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-identify"));
    QVERIFY(grid_model != nullptr && identify != nullptr);
    files->selectAll();
    QTRY_VERIFY(identify->isEnabled());
    QTest::mouseClick(identify, Qt::LeftButton);
    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = properties->findChild<QDialog*>(
                     QStringLiteral("bench-musicbrainz-identify"))) != nullptr);
    QPointer<QDialog> open = dialog;

    // The two albums, listed; the first shown, the second looked up
    // meanwhile -- by its tags, having no release id.
    auto* albums =
        dialog->findChild<QTreeWidget*>(QStringLiteral("bench-musicbrainz-identify-albums"));
    auto* place = dialog->findChild<QLabel*>(QStringLiteral("bench-musicbrainz-identify-place"));
    QVERIFY(albums != nullptr && place != nullptr);
    QVERIFY(albums->isVisible());
    QCOMPARE(albums->topLevelItemCount(), 2);
    QCOMPARE(albums->topLevelItem(0)->text(0), QStringLiteral("Band — Alpha"));
    QCOMPARE(albums->topLevelItem(1)->text(0), QStringLiteral("Band — Beta"));
    // The second's versions there; the first's best version previewed,
    // and with it chosen.
    QTRY_COMPARE(albums->topLevelItem(1)->data(0, Qt::UserRole + 1).toString(), QStringLiteral("1 version"));
    QTRY_COMPARE(albums->topLevelItem(0)->data(0, Qt::UserRole + 1).toString(), QStringLiteral("✓ Alpha · CD"));
    QVERIFY(place->text().startsWith(QStringLiteral("Album 1 of 2")));
    auto* apply = dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-apply"));
    auto* skip = dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-skip"));
    auto* next = dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-next"));
    QVERIFY(apply != nullptr && skip != nullptr && next != nullptr);
    QTRY_COMPARE(apply->text(), QStringLiteral("Apply 1 album"));
    QVERIFY(apply->isEnabled());

    // Next: the second, its best version previewed too.
    next->click();
    QTRY_VERIFY(place->text().startsWith(QStringLiteral("Album 2 of 2")));
    QTRY_COMPARE(albums->topLevelItem(1)->data(0, Qt::UserRole + 1).toString(), QStringLiteral("✓ Beta · CD"));
    QTRY_COMPARE(apply->text(), QStringLiteral("Apply 2 albums"));
    QCOMPARE(grid_model->patches().patch_count(), 0U);
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(dialog->grab().save(directory + QStringLiteral("/identify-albums-steps.png")));
    }
    // Skipped, then not.
    skip->click();
    QCOMPARE(albums->topLevelItem(1)->data(0, Qt::UserRole + 1).toString(), QStringLiteral("Skipped"));
    QCOMPARE(apply->text(), QStringLiteral("Apply 1 album"));
    QCOMPARE(skip->text(), QStringLiteral("Don't skip"));
    skip->click();
    QCOMPARE(apply->text(), QStringLiteral("Apply 2 albums"));

    // Applied: both staged, and done.
    apply->click();
    QTRY_VERIFY(open.isNull());
    QCOMPARE(asked.count(QStringLiteral("search")), 1);
    const auto title = grid_model->fieldColumn(QStringLiteral("Album"));
    const auto release = grid_model->fieldColumn(QStringLiteral("MUSICBRAINZ_ALBUMID"));
    QVERIFY(title.has_value() && release.has_value());
    QTRY_COMPARE(grid_model->index(3, *release).data(metadata_cell_values_role).toStringList(),
                 QStringList{QStringLiteral("44444444-2222-3333-4444-555555555555")});
    QCOMPARE(grid_model->index(0, *release).data(metadata_cell_values_role).toStringList(),
             QStringList{QString::fromStdString(alpha_id)});
    QCOMPARE(grid_model->index(2, *title).data(metadata_cell_values_role).toStringList(),
             QStringList{QStringLiteral("Beta")});
    delete properties;
}

void BenchMainWindowTest::musicBrainzFingerprintScanRanksAndStages() {
    const auto field = [](std::string name, std::vector<std::string> values) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = std::move(values),
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    // Only titles and track numbers — the fingerprint path must work with
    // no album, artist, or MusicBrainz tags at all.
    const auto make_source = [&field](const QString& label, const QString& title,
                                      const QString& track_number) {
        return MetadataPropertiesSource{
            .source =
                metadata::StagedMetadataSource{
                    .raw_path = "/music/" + label.toStdString() + ".flac",
                    .source_revision = std::nullopt,
                    .baseline =
                        metadata::MetadataDocument{
                            .fields = {field("TITLE", {title.toStdString()}),
                                       field("TRACKNUMBER", {track_number.toStdString()})},
                            .unsupported_native_objects = {},
                        },
                },
            .track_label = label,
        };
    };
    const std::vector sources{
        make_source(QStringLiteral("one"), QStringLiteral("One"), QStringLiteral("1")),
        make_source(QStringLiteral("two"), QStringLiteral("Two"), QStringLiteral("2"))};

    static constexpr auto lookup_body = R"json({
      "id": "11111111-2222-3333-4444-555555555555",
      "title": "Alpha", "status": "Official", "date": "1999-09-09", "country": "DE",
      "release-group": {"id": "99999999-8888-7777-6666-555555555555"},
      "artist-credit": [{"name": "Band",
        "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "name": "Band"}}],
      "media": [{"position": 1, "format": "CD", "track-count": 2, "tracks": [
        {"id": "aaaa1111-0000-0000-0000-000000000001", "position": 1, "number": "1",
         "title": "One", "length": 61000,
         "recording": {"id": "bbbb1111-0000-0000-0000-000000000001", "title": "One"}},
        {"id": "aaaa1111-0000-0000-0000-000000000002", "position": 2, "number": "2",
         "title": "Two", "length": 59000,
         "recording": {"id": "bbbb1111-0000-0000-0000-000000000002", "title": "Two"}}
      ]}]
    })json";
    static constexpr auto acoustid_one = R"json({
      "status": "ok",
      "results": [{"id": "x", "score": 0.98, "recordings": [
        {"id": "bbbb1111-0000-0000-0000-000000000001",
         "releases": [{"id": "11111111-2222-3333-4444-555555555555"}]}]}]
    })json";
    static constexpr auto acoustid_two = R"json({
      "status": "ok",
      "results": [{"id": "y", "score": 0.97, "recordings": [
        {"id": "bbbb1111-0000-0000-0000-000000000002",
         "releases": [{"id": "11111111-2222-3333-4444-555555555555"},
                      {"id": "22222222-2222-3333-4444-555555555555"}]}]}]
    })json";
    int fingerprints = 0;
    int lookups = 0;
    const MusicBrainzLookupService service{
        .fetch =
            [](const QString& url, std::function<void(core::Result<QByteArray>)> completion) {
                if (url.contains(QStringLiteral("11111111-2222-3333-4444-555555555555?inc="))) {
                    completion(QByteArray{lookup_body});
                    return;
                }
                completion(std::unexpected(core::Error{
                    .code = core::ErrorCode::not_found,
                    .message = "no such release",
                    .context = {},
                }));
            },
        .fingerprint =
            [&fingerprints](const QString& file_path,
                            std::function<void(core::Result<AcoustIdFingerprint>)> completion) {
                ++fingerprints;
                completion(AcoustIdFingerprint{
                    .duration_seconds = file_path.contains(QStringLiteral("one")) ? 61U : 59U,
                    .fingerprint = QStringLiteral("AQAD-fake-") + file_path,
                });
            },
        .acoustid_lookup =
            [&lookups](const AcoustIdFingerprint& fingerprint,
                       std::function<void(core::Result<QByteArray>)> completion) {
                ++lookups;
                completion(QByteArray{fingerprint.fingerprint.contains(QStringLiteral("one"))
                                          ? acoustid_one
                                          : acoustid_two});
            },
    };

    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {}, {}, {}, {}, {}, {}, {}, nullptr, {}, service);
    properties->show();

    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* identify = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-identify"));
    QVERIFY(grid_model != nullptr);
    QVERIFY(identify != nullptr);
    files->selectAll();
    QTRY_VERIFY(identify->isEnabled());
    QTest::mouseClick(identify, Qt::LeftButton);

    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = properties->findChild<QDialog*>(
                     QStringLiteral("bench-musicbrainz-identify"))) != nullptr);
    auto* scan = dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-scan"));
    auto* results =
        dialog->findChild<QTreeWidget*>(QStringLiteral("bench-musicbrainz-identify-results"));
    auto* use = dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-apply"));
    QVERIFY(scan != nullptr && scan->isEnabled());
    QVERIFY(results != nullptr);
    QVERIFY(use != nullptr);
    QTest::mouseClick(scan, Qt::LeftButton);

    // Both files fingerprinted and looked up; the release both matched
    // ranks first with its coverage shown, and the unloadable second
    // candidate drops out instead of blocking.
    QTRY_COMPARE(results->topLevelItemCount(), 1);
    QCOMPARE(fingerprints, 2);
    QCOMPARE(lookups, 2);
    QVERIFY(results->topLevelItem(0)->text(0).endsWith(QStringLiteral("Alpha")));
    QVERIFY(results->topLevelItem(0)
                ->data(0, Qt::UserRole + 1)
                .toString()
                .startsWith(QStringLiteral("2/2 files")));

    QTRY_VERIFY(use->isEnabled());
    QTest::mouseClick(use, Qt::LeftButton);
    QTRY_VERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-musicbrainz-identify")) ==
                nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(grid_model->patches().patch_count() > 0U, 5'000);
    const auto album_column = grid_model->fieldColumn(QStringLiteral("Album"));
    QVERIFY(album_column.has_value());
    QCOMPARE(grid_model->index(0, *album_column).data(metadata_cell_values_role).toStringList(),
             QStringList{QStringLiteral("Alpha")});
    delete properties;
}

void BenchMainWindowTest::closingATabReturnsToThePreviousOne() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_VERIFY(!window.list_tabs_.empty());
    auto* origin = window.list_tabs_.front()->view;
    const auto make = [&window](const char* name) {
        return window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                           .kind = persistence::ListKind::scratch,
                                                           .name = name,
                                                           .pinned = false,
                                                           .dirty = false,
                                                           .items = {}},
                                 true);
    };
    auto* first = make("First");
    auto* second = make("Second");
    QVERIFY(first != nullptr && second != nullptr);

    // Visit the origin, then the second list: closing it goes back to the
    // origin, even though "First" sits right beside it.
    tabs->setCurrentWidget(origin);
    tabs->setCurrentWidget(second->view);
    window.closeTabAt(tabs->indexOf(second->view));
    QTRY_COMPARE(tabs->currentWidget(), static_cast<QWidget*>(origin));
    QVERIFY(tabs->indexOf(first->view) >= 0);
}

// Delete and Play act on the tab you are looking at, not on whichever list
// last held a selection.
void BenchMainWindowTest::selectionActionsFollowTheActiveTab() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_VERIFY(!window.list_tabs_.empty());
    auto& origin = *window.list_tabs_.front();
    LocalTrackRow row;
    row.raw_path = "/one.flac";
    row.title = "One";
    row.probed = true;
    origin.model->replaceRows({row});
    tabs->setCurrentWidget(origin.view);
    QCOMPARE(window.activeTrackView(), origin.view);

    auto* other =
        window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                    .kind = persistence::ListKind::scratch,
                                                    .name = "Other",
                                                    .pinned = false,
                                                    .dirty = false,
                                                    .items = {}},
                          true);
    QVERIFY(other != nullptr);
    QCOMPARE(tabs->currentWidget(), other->view);
    QCOMPARE(window.activeTrackView(), other->view);

    // A selection in the list on screen is the one Delete would remove, and
    // one in a list that is not on screen is not.
    origin.view->selectRow(0);
    QVERIFY(other->view->selectionModel()->selectedRows().isEmpty());
    window.refreshSelectionActions();
    QVERIFY(!window.remove_selected_action_->isEnabled());
    tabs->setCurrentWidget(origin.view);
    window.refreshSelectionActions();
    QCOMPARE(window.activeTrackView(), origin.view);
    QVERIFY(window.remove_selected_action_->isEnabled());
}

void BenchMainWindowTest::localArtworkSurvivesInvalidation() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto flac = media.filePath(QStringLiteral("one.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), flac));
    {
        QImage cover{32, 32, QImage::Format_RGB32};
        cover.fill(Qt::darkRed);
        QVERIFY(cover.save(media.filePath(QStringLiteral("cover.png")), "PNG"));
    }

    BenchMainWindow window;
    window.show();
    window.openLocalPaths({QFile::encodeName(flac).toStdString()});
    QTableView* view = nullptr;
    LocalListModel* model = nullptr;
    QTRY_VERIFY([&] {
        for (auto* candidate : window.findChildren<QTableView*>()) {
            if (qobject_cast<LocalListModel*>(candidate->model()) &&
                candidate->model()->rowCount() == 1) {
                view = candidate;
                model = qobject_cast<LocalListModel*>(candidate->model());
                return true;
            }
        }
        return false;
    }());
    if (model == nullptr) {
        QFAIL("the list's model is found");
    }
    QTRY_VERIFY(model->rows().front().probed);
    const auto artwork = [&] {
        return model->data(model->index(0, 0), static_cast<int>(ui::track_album_artwork_role))
            .value<QImage>();
    };
    QTRY_VERIFY(!artwork().isNull());

    window.invalidateArtwork(model->rows().front().raw_path);
    QTRY_VERIFY(!artwork().isNull());
}

void BenchMainWindowTest::convertDialogPlansAndConvertsSelection() {
    QTemporaryDir media;
    QTemporaryDir destination;
    QVERIFY(media.isValid());
    QVERIFY(destination.isValid());
    const auto loud_path = media.filePath(QStringLiteral("loud.wav"));
    const auto quiet_path = media.filePath(QStringLiteral("quiet.wav"));
    write_sine_wav_fixture(loud_path, 0.8);
    write_sine_wav_fixture(quiet_path, 0.2);

    const auto field = [](std::string name, std::vector<std::string> values) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = std::move(values),
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    const auto make_item = [&field](const QString& path, const QString& title,
                                    const QString& track) {
        const auto encoded = QFile::encodeName(path);
        return ConvertDialogItem{
            .raw_path = std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())},
            .selection = {},
            .segment = {},
            .source_revision = {},
            .metadata =
                metadata::MetadataDocument{
                    .fields = {field("TITLE", {title.toStdString()}),
                               field("ALBUM", {"Converted Album"}),
                               field("TRACKNUMBER", {track.toStdString()})},
                    .unsupported_native_objects = {},
                },
            .label = title,
            .fetch = {},
        };
    };
    const auto saved_layouts = std::vector{persistence::SavedOutputLayoutProfile{
        .id = core::StableId::random(),
        .profile =
            operations::OutputLayoutProfile{
                .schema_version = 1U,
                .name = "Saved album layout",
                .dialect = {},
                .relative_directory_expression = "saved/%album%",
                .basename_expression = "%title%",
                .sanitization_policy = {"linux", 1U},
            },
    }};
    const auto saved_destinations = std::vector{persistence::SavedDestinationProfile{
        .id = core::StableId::random(),
        .profile =
            operations::DestinationProfile{
                .schema_version = 1U,
                .name = "Saved root",
                .root_raw_path = destination.path().toStdString(),
                .containment_policy = {"lexical-beneath-root", 1U},
            },
    }};
    std::vector<persistence::SavedEncoderPreset> preset_catalog;
    ConvertPresetStore preset_store{
        .load =
            [&preset_catalog](ConvertPresetStore::LoadCompletion completion) {
                completion(preset_catalog, QString{});
            },
        .save =
            [&preset_catalog](persistence::SavedEncoderPreset preset,
                              ConvertPresetStore::Completion completion) {
                preset_catalog.push_back(std::move(preset));
                completion(QString{});
            },
        .remove =
            [&preset_catalog](const core::StableId id, ConvertPresetStore::Completion completion) {
                std::erase_if(preset_catalog, [&id](const auto& entry) { return entry.id == id; });
                completion(QString{});
            },
    };
    auto* dialog = new ConvertDialog(
        {make_item(loud_path, QStringLiteral("Loud"), QStringLiteral("01")),
         make_item(quiet_path, QStringLiteral("Quiet"), QStringLiteral("02"))},
        [&saved_layouts, &saved_destinations](auto completion) {
            completion(saved_layouts, saved_destinations, QString{});
        },
        preset_store);
    dialog->show();

    auto* preset = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-preset"));
    auto* root = dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-destination"));
    auto* directories =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-directory-expression"));
    auto* names =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-basename-expression"));
    auto* preview = dialog->findChild<QListWidget*>(QStringLiteral("bench-convert-preview"));
    auto* run = dialog->findChild<QPushButton*>(QStringLiteral("bench-convert-run"));
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("bench-convert-status"));
    auto* problems = dialog->findChild<QPlainTextEdit*>(QStringLiteral("bench-convert-problems"));
    auto* channels = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-channels"));
    auto* gain_mode = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-gain"));
    QVERIFY(preset != nullptr && root != nullptr && directories != nullptr && names != nullptr);
    QVERIFY(preview != nullptr && run != nullptr && status != nullptr && problems != nullptr);
    QVERIFY(channels != nullptr && gain_mode != nullptr);
    // Processing choices restore from persisted settings; this test's items
    // carry no ReplayGain, so pin the gain policy off explicitly.
    channels->setCurrentIndex(channels->findData(0));
    gain_mode->setCurrentIndex(gain_mode->findData(0));
    // Problem reports scroll inside a bounded pane instead of stretching
    // the dialog; without problems it stays hidden.
    QVERIFY(!problems->isVisible());
    QVERIFY(problems->maximumHeight() <= 200);

    // The app's saved naming layouts and destination roots are offered
    // directly; picking them fills the editable fields.
    auto* layout_choice = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-layout"));
    auto* root_choice =
        dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-destination-choice"));
    QVERIFY(layout_choice != nullptr && root_choice != nullptr);
    QTRY_COMPARE(layout_choice->count(), 2);
    QTRY_COMPARE(root_choice->count(), 2);
    layout_choice->setCurrentIndex(1);
    emit layout_choice->activated(1);
    QCOMPARE(directories->text(), QStringLiteral("saved/%album%"));
    QCOMPARE(names->text(), QStringLiteral("%title%"));
    root_choice->setCurrentIndex(1);
    emit root_choice->activated(1);
    QCOMPARE(root->text(), destination.path());
    // Hand-editing drifts the choice back to Custom.
    QTest::keyClick(names, Qt::Key_X);
    QCOMPARE(layout_choice->currentIndex(), 0);

    // A new encoder preset saves from the editor, joins the combo as a
    // selectable profile, and deletes again; built-ins are untouched.
    const auto builtin_count = preset->count();
    auto* preset_new = dialog->findChild<QPushButton*>(QStringLiteral("bench-convert-preset-new"));
    auto* preset_export =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-convert-preset-export"));
    auto* preset_delete =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-convert-preset-delete"));
    QVERIFY(preset_export != nullptr);
    QVERIFY(preset_new != nullptr && preset_delete != nullptr);
    QVERIFY(!preset_delete->isVisible());
    QTest::mouseClick(preset_new, Qt::LeftButton);
    auto* editor = dialog->findChild<QDialog*>(QStringLiteral("bench-preset-editor"));
    QVERIFY(editor != nullptr);
    auto* editor_name = editor->findChild<QLineEdit*>(QStringLiteral("bench-preset-editor-name"));
    auto* editor_format =
        editor->findChild<QComboBox*>(QStringLiteral("bench-preset-editor-format"));
    auto* editor_bitrate =
        editor->findChild<QSpinBox*>(QStringLiteral("bench-preset-editor-bitrate"));
    auto* editor_buttons =
        editor->findChild<QDialogButtonBox*>(QStringLiteral("bench-preset-editor-buttons"));
    QVERIFY(editor_name != nullptr && editor_format != nullptr && editor_bitrate != nullptr &&
            editor_buttons != nullptr);
    QVERIFY(!editor_buttons->button(QDialogButtonBox::Save)->isEnabled());
    editor_name->setText(QStringLiteral("Phone Opus"));
    editor_format->setCurrentIndex(1);
    editor_bitrate->setValue(96);
    channels->setCurrentIndex(1);
    gain_mode->setCurrentIndex(1);
    QVERIFY(editor_buttons->button(QDialogButtonBox::Save)->isEnabled());
    QTest::mouseClick(editor_buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
    QTRY_COMPARE(preset_catalog.size(), 1U);
    QCOMPARE(preset_catalog.front().preset.codec_name, std::string{"libopus"});
    QCOMPARE(preset_catalog.front().preset.bit_rate, std::optional<std::int64_t>{96'000});
    // Separator + the saved preset, selected as the current choice.
    QTRY_COMPARE(preset->count(), builtin_count + 2);
    QCOMPARE(preset->currentText(), QStringLiteral("Phone Opus"));
    QTRY_VERIFY(preset_delete->isVisible());
    // Job settings restore, but permanent gain is never restored (including
    // legacy presets that still contain a gain field).
    const auto gain_key =
        QStringLiteral("convert/job-presets/%1/gain").arg(preset->currentData().toString());
    QVERIFY(!QSettings{}.contains(gain_key));
    QSettings{}.setValue(gain_key, 2);
    preset->setCurrentIndex(0);
    channels->setCurrentIndex(0);
    gain_mode->setCurrentIndex(1);
    preset->setCurrentIndex(preset->count() - 1);
    QCOMPARE(channels->currentData().toInt(), 1);
    QCOMPARE(gain_mode->currentData().toInt(), 0);
    QTest::mouseClick(preset_delete, Qt::LeftButton);
    QTRY_VERIFY(preset_catalog.empty());
    QTRY_COMPARE(preset->count(), builtin_count);
    QTRY_VERIFY(!preset_delete->isVisible());

    preset->setCurrentIndex(preset->findData(QStringLiteral("opus-192")));
    // Preset selection cannot enable permanent gain.
    gain_mode->setCurrentIndex(gain_mode->findData(0));
    root->setText(destination.path());
    directories->setText(QStringLiteral("%album%"));
    names->setText(QStringLiteral("%tracknumber% - %title%"));
    QTRY_VERIFY(preview->count() > 0 &&
                preview->item(0)->text() == QStringLiteral("Converted Album/01 - Loud.opus"));
    QCOMPARE(preview->count(), 2);
    QCOMPARE(preview->item(1)->text(), QStringLiteral("Converted Album/02 - Quiet.opus"));
    QTRY_VERIFY(run->isEnabled());

    // A colliding layout blocks Convert; the explanation scrolls in the
    // bounded problems pane while the status stays a one-line count.
    names->setText(QStringLiteral("same"));
    QTRY_VERIFY(!run->isEnabled());
    QTRY_VERIFY(problems->isVisible() &&
                problems->toPlainText().contains(QStringLiteral("target")));
    QVERIFY(status->text().contains(QStringLiteral("problem")));
    names->setText(QStringLiteral("%tracknumber% - %title%"));
    QTRY_VERIFY(run->isEnabled());
    QTRY_VERIFY(!problems->isVisible());

    QTest::mouseClick(run, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith(QStringLiteral("Converted 2 of 2 files.")),
                             15'000);
    const auto album_dir = QDir{destination.path()}.filePath(QStringLiteral("Converted Album"));
    QVERIFY(QFileInfo::exists(album_dir + QStringLiteral("/01 - Loud.opus")));
    QVERIFY(QFileInfo::exists(album_dir + QStringLiteral("/02 - Quiet.opus")));
    const auto encoded = QFile::encodeName(album_dir + QStringLiteral("/02 - Quiet.opus"));
    const auto reread = metadata::read_local_metadata(
        std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())});
    QVERIFY(reread.has_value());
    QCOMPARE(reread->document.first_effective_value("title"), std::optional<std::string>{"Quiet"});

    // Artwork carriage is a persisted opt-out (ADR-0131), enabled by default.
    auto* embed_artwork = dialog->findChild<QCheckBox*>(QStringLiteral("bench-convert-artwork"));
    QVERIFY(embed_artwork != nullptr);
    QVERIFY(embed_artwork->isChecked());

    // The resampling and bit-depth choices reach the pipeline: FLAC at a
    // forced 96 kHz stored as dithered 16-bit.
    auto* resample = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-resample"));
    auto* bit_depth = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-bit-depth"));
    QVERIFY(resample != nullptr && bit_depth != nullptr);
    QCOMPARE(resample->currentData().toInt(), 0);
    QCOMPARE(bit_depth->currentData().toInt(), 0);
    resample->setCurrentIndex(resample->findData(96'000));
    bit_depth->setCurrentIndex(bit_depth->findData(16));
    preset->setCurrentIndex(preset->findData(QStringLiteral("flac")));
    names->setText(QStringLiteral("%tracknumber% - %title% 96k"));
    QTRY_VERIFY(preview->count() > 0 &&
                preview->item(0)->text() == QStringLiteral("Converted Album/01 - Loud 96k.flac"));
    QTRY_VERIFY(run->isEnabled());
    QTest::mouseClick(run, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith(QStringLiteral("Converted 2 of 2 files.")),
                             15'000);
    const auto resampled_path = album_dir + QStringLiteral("/01 - Loud 96k.flac");
    QVERIFY(QFileInfo::exists(resampled_path));
    const auto resampled_encoded = QFile::encodeName(resampled_path);
    auto resampled = formats::AudioDecoder::open(std::string{
        resampled_encoded.constData(), static_cast<std::size_t>(resampled_encoded.size())});
    QVERIFY(resampled.has_value());
    QCOMPARE(resampled->output_format().sample_rate, 96'000);
    const auto probed = formats::probe_local_media(std::string{
        resampled_encoded.constData(), static_cast<std::size_t>(resampled_encoded.size())});
    QVERIFY(probed.has_value() && probed->best_audio_stream.has_value());
    QCOMPARE(
        probed->audio_streams[static_cast<std::size_t>(*probed->best_audio_stream)].sample_format,
        std::string{"s16"});

    // Mirror mode (ADR-0132) disables the expressions and previews the
    // source structure below the inferred common root.
    auto* mirror = dialog->findChild<QCheckBox*>(QStringLiteral("bench-convert-mirror"));
    auto* directory_field =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-directory-expression"));
    QVERIFY(mirror != nullptr && directory_field != nullptr);
    QVERIFY(!mirror->isChecked());
    mirror->setChecked(true);
    QVERIFY(!directory_field->isEnabled());
    QTRY_VERIFY(preview->count() >= 3 && preview->item(0)->text().startsWith(
                                             QStringLiteral("Recreating full source paths")));

    // ADR-0154: mirror recreates the complete source path beneath the
    // destination, so single-album selections keep their folders too.
    const auto media_relative = QString{media.path()}.mid(1) + QStringLiteral("/loud.flac");
    QTRY_VERIFY(preview->count() >= 2 && preview->item(1)->text() == media_relative);
    mirror->setChecked(false);
    QVERIFY(directory_field->isEnabled());
    delete dialog;
}

// ADR-0237 stage 6: a file of an engine elsewhere is fetched first and
// converted from the copy; the preview and the output are as for any file.
void BenchMainWindowTest::convertDialogFetchesFilesItCannotReach() {
    QTemporaryDir media;
    QTemporaryDir destination;
    QVERIFY(media.isValid());
    QVERIFY(destination.isValid());
    const auto original = media.filePath(QStringLiteral("original.wav"));
    write_sine_wav_fixture(original, 0.5);
    std::vector<std::filesystem::path> fetched_to;
    ConvertDialogItem item{
        .raw_path = "/srv/elsewhere/Album/fetched.wav",
        .selection = {},
        .segment = {},
        .source_revision = std::nullopt,
        .metadata =
            metadata::MetadataDocument{
                .fields = {metadata::MetadataField{.canonical_name = "title",
                                                   .native_name = "TITLE",
                                                   .values = {"Fetched"},
                                                   .qualifier = {},
                                                   .provenance =
                                                       metadata::FieldProvenance::embedded}},
                .unsupported_native_objects = {},
            },
        .label = QStringLiteral("Fetched"),
        .fetch = [&fetched_to, original](const std::filesystem::path& to,
                                         const core::CancellationToken&) -> core::Result<void> {
            fetched_to.push_back(to);
            std::error_code copied;
            std::filesystem::copy_file(QFile::encodeName(original).toStdString(), to, copied);
            if (copied) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::io, .message = copied.message(), .context = {}});
            }
            return {};
        }};
    auto* dialog = new ConvertDialog({std::move(item)},
                                     [](auto completion) { completion({}, {}, QString{}); }, {});
    dialog->show();
    auto* preset = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-preset"));
    auto* root = dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-destination"));
    auto* directories =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-directory-expression"));
    auto* names =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-basename-expression"));
    auto* mirror = dialog->findChild<QCheckBox*>(QStringLiteral("bench-convert-mirror"));
    auto* gain_mode = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-gain"));
    auto* preview = dialog->findChild<QListWidget*>(QStringLiteral("bench-convert-preview"));
    auto* run = dialog->findChild<QPushButton*>(QStringLiteral("bench-convert-run"));
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("bench-convert-status"));
    QVERIFY(preset != nullptr && root != nullptr && directories != nullptr && names != nullptr &&
            mirror != nullptr && gain_mode != nullptr && preview != nullptr && run != nullptr &&
            status != nullptr);
    mirror->setChecked(false);
    gain_mode->setCurrentIndex(gain_mode->findData(0));
    preset->setCurrentIndex(preset->findData(QStringLiteral("flac")));
    root->setText(destination.path());
    directories->setText({});
    names->setText(QStringLiteral("%title%"));
    // Planned from what the engine knows of it, before anything is fetched.
    QTRY_VERIFY(preview->count() > 0 && preview->item(0)->text() == QStringLiteral("Fetched.flac"));
    QVERIFY(fetched_to.empty());
    QTRY_VERIFY(run->isEnabled());
    QTest::mouseClick(run, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith(QStringLiteral("Converted 1 of 1 files.")),
                             15'000);
    QCOMPARE(fetched_to.size(), 1U);
    QCOMPARE(fetched_to.front().filename().string(), std::string{"fetched.wav"});
    QVERIFY(QFileInfo::exists(QDir{destination.path()}.filePath(QStringLiteral("Fetched.flac"))));
    // The copy goes once converted.
    QVERIFY(!std::filesystem::exists(fetched_to.front()));
    delete dialog;
}

// ADR-0173: the dialog's Gain choice permanently applies the item's
// ReplayGain metadata to the encoded PCM, exactly like the core path.
void BenchMainWindowTest::convertDialogAppliesPermanentReplayGain() {
    QSettings{}.setValue(QStringLiteral("convert/gain"), 2);
    QTemporaryDir media;
    QTemporaryDir destination;
    QVERIFY(media.isValid());
    QVERIFY(destination.isValid());
    const auto source_path = media.filePath(QStringLiteral("gained.wav"));
    write_sine_wav_fixture(source_path, 0.8);

    const auto field = [](std::string name, std::vector<std::string> values) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = std::move(values),
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    const auto encoded_source = QFile::encodeName(source_path);
    ConvertDialogItem item{
        .raw_path = std::string{encoded_source.constData(),
                                static_cast<std::size_t>(encoded_source.size())},
        .selection = {},
        .segment = {},
        .source_revision = {},
        .metadata =
            metadata::MetadataDocument{
                .fields = {field("TITLE", {"Gained"}),
                           field("REPLAYGAIN_TRACK_GAIN", {"-12.04 dB"}),
                           field("REPLAYGAIN_TRACK_PEAK", {"0.8"})},
                .unsupported_native_objects = {},
            },
        .label = QStringLiteral("Gained"),
        .fetch = {},
    };
    ConvertPresetStore preset_store{
        .load = [](ConvertPresetStore::LoadCompletion completion) { completion({}, QString{}); },
        .save = [](persistence::SavedEncoderPreset,
                   ConvertPresetStore::Completion completion) { completion(QString{}); },
        .remove = [](core::StableId,
                     ConvertPresetStore::Completion completion) { completion(QString{}); },
    };
    auto* dialog = new ConvertDialog(
        {std::move(item)}, [](auto completion) { completion({}, {}, QString{}); }, preset_store);
    dialog->show();
    auto* preset = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-preset"));
    auto* root = dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-destination"));
    auto* directories =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-directory-expression"));
    auto* names =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-basename-expression"));
    auto* gain_mode = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-gain"));
    auto* run = dialog->findChild<QPushButton*>(QStringLiteral("bench-convert-run"));
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("bench-convert-status"));
    auto* mirror = dialog->findChild<QCheckBox*>(QStringLiteral("bench-convert-mirror"));
    QVERIFY(preset != nullptr && root != nullptr && names != nullptr && gain_mode != nullptr);
    QVERIFY(run != nullptr && status != nullptr && directories != nullptr && mirror != nullptr);
    QCOMPARE(gain_mode->currentData().toInt(), 0);
    auto* warning = dialog->findChild<QLabel*>(QStringLiteral("bench-convert-gain-warning"));
    QVERIFY(warning);
    mirror->setChecked(false);
    preset->setCurrentIndex(preset->findData(QStringLiteral("flac")));
    root->setText(destination.path());
    directories->setText(QString{});
    names->setText(QStringLiteral("%title%"));
    gain_mode->setCurrentIndex(gain_mode->findData(1));
    QVERIFY(warning->text().contains(QStringLiteral("permanently changes the audio samples")));
    QTRY_VERIFY(run->isEnabled());
    QTimer::singleShot(0, dialog, [dialog] {
        auto* confirmation =
            dialog->findChild<QMessageBox*>(QStringLiteral("bench-convert-gain-confirmation"));
        QVERIFY(confirmation);
        QCOMPARE(confirmation->standardButton(confirmation->defaultButton()), QMessageBox::Cancel);
        confirmation->button(QMessageBox::Cancel)->click();
    });
    QTest::mouseClick(run, Qt::LeftButton);
    QVERIFY(!QFileInfo::exists(destination.filePath(QStringLiteral("Gained.flac"))));
    QVERIFY(run->isVisible());
    QTimer::singleShot(0, dialog, [dialog] {
        auto* confirmation =
            dialog->findChild<QMessageBox*>(QStringLiteral("bench-convert-gain-confirmation"));
        QVERIFY(confirmation);
        confirmation->button(QMessageBox::Yes)->click();
    });
    QTest::mouseClick(run, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith(QStringLiteral("Converted 1 of 1 files.")),
                             15'000);

    const auto converted =
        QFile::encodeName(QDir{destination.path()}.filePath(QStringLiteral("Gained.flac")));
    auto decoder = formats::AudioDecoder::open(
        std::string{converted.constData(), static_cast<std::size_t>(converted.size())});
    QVERIFY(decoder.has_value());
    float peak = 0.0F;
    while (true) {
        auto chunk = decoder->next_chunk();
        QVERIFY(chunk.has_value());
        if (!*chunk) {
            break;
        }
        for (const auto sample : (*chunk)->interleaved_samples) {
            peak = std::max(peak, std::abs(sample));
        }
    }
    // -12.04 dB over a 0.8 peak must land near 0.2; an untouched copy at
    // 0.8 means the dialog's gain option silently did nothing.
    QVERIFY2(peak > 0.17F && peak < 0.23F, qPrintable(QString::number(static_cast<double>(peak))));
    delete dialog;
    ConvertDialog reopened{{}};
    QCOMPARE(
        reopened.findChild<QComboBox*>(QStringLiteral("bench-convert-gain"))->currentData().toInt(),
        0);
}

// ADR-0144: notifications fire only for track changes while playing in
// the background, never retro-notify, and the menu toggle persists.
void BenchMainWindowTest::desktopNotificationsNotifyBackgroundTrackChanges() {
    DesktopNotifier notifier;
    notifier.setBackgroundOnly(true);
    QStringList sent;
    notifier.setSendOverride([&sent](const QString& summary, const QString& body) {
        sent << summary + QStringLiteral("|") + body;
    });
    MprisPlaybackState playing;
    playing.status = QStringLiteral("Playing");
    playing.title = QStringLiteral("First Song");
    playing.artist = QStringLiteral("Artist");
    playing.album = QStringLiteral("Album");
    playing.track_key = QStringLiteral("key-1");
    // Disabled: tracked but silent; enabling never retro-notifies.
    QVERIFY(!notifier.publish(playing, false));
    notifier.setEnabled(true);
    QVERIFY(!notifier.publish(playing, false));
    // A background track change notifies with a markup-escaped body.
    playing.track_key = QStringLiteral("key-2");
    playing.title = QStringLiteral("Second Song");
    playing.artist = QStringLiteral("A & B");
    QVERIFY(notifier.publish(playing, false));
    QCOMPARE(notifier.sentCount(), 1ULL);
    QCOMPARE(sent.size(), 1);
#ifdef __APPLE__
    QCOMPARE(sent.front(), QStringLiteral("Second Song|A & B — Album"));
#else
    QCOMPARE(sent.front(), QStringLiteral("Second Song|A &amp; B — Album"));
#endif
    // A change while the window is active stays silent and is tracked.
    playing.track_key = QStringLiteral("key-3");
    QVERIFY(!notifier.publish(playing, true));
    QVERIFY(!notifier.publish(playing, false));
    // Pause never notifies; resuming into a new track does.
    playing.track_key = QStringLiteral("key-4");
    playing.status = QStringLiteral("Paused");
    QVERIFY(!notifier.publish(playing, false));
    playing.status = QStringLiteral("Playing");
    QVERIFY(notifier.publish(playing, false));
    QCOMPARE(notifier.sentCount(), 2ULL);
    notifier.setBackgroundOnly(false);
    playing.track_key = QStringLiteral("key-5");
    QVERIFY(notifier.publish(playing, true));
    QCOMPARE(notifier.sentCount(), 3ULL);
    QSignalSpy delivered(&notifier, &DesktopNotifier::deliveryFinished);
    notifier.setEnabled(false);
    notifier.sendTest();
    QCOMPARE(delivered.size(), 1);
    QVERIFY(delivered.front().front().toString().isEmpty());

    BenchMainWindow window;
    window.show();
    auto* action = window.findChild<QAction*>(QStringLiteral("action-desktop-notifications"));
    QVERIFY(action != nullptr);
    QVERIFY(action->isCheckable());
    QVERIFY(!action->isChecked());
    action->setChecked(true);
    QCOMPARE(QSettings{}.value(QStringLiteral("desktop/notifications"), false).toBool(), true);
    action->setChecked(false);
    QCOMPARE(QSettings{}.value(QStringLiteral("desktop/notifications"), true).toBool(), false);
}

void BenchMainWindowTest::lastFmSettingsAndTrackActions() {
    BenchMainWindow window;
    window.show();
    // The service talks to an endpoint that accepts and never answers, so no
    // request leaves this machine and no real reply races the ones fed below.
    QTcpServer silent;
    QVERIFY(silent.listen(QHostAddress::LocalHost));
    QTemporaryDir state;
    QVERIFY(state.isValid());
    delete window.lastfm_;
    window.lastfm_ =
        new LastFmService(state.filePath(QStringLiteral("lastfm.json")), &window,
                          QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(silent.serverPort())));
    const auto reply = [&window](const QString& op, const QByteArray& json) {
        emit window.lastfm_->completed(op, QJsonDocument::fromJson(json).object(), {});
    };
    auto* dialog = window.showSettingsDialog(SettingsDialog::Page::lastfm);
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("lastfm-status"));
    auto* secret = dialog->findChild<QLineEdit*>(QStringLiteral("lastfm-account-secret"));
    QVERIFY(status && secret);
    QCOMPARE(secret->echoMode(), QLineEdit::Password);
    QTRY_VERIFY(status->text().contains(QStringLiteral("Not connected")));
    auto* key = dialog->findChild<QLineEdit*>(QStringLiteral("lastfm-account-key"));
    auto* reuse = dialog->findChild<QCheckBox*>(QStringLiteral("lastfm-reuse-key"));
    auto* begin = dialog->findChild<QPushButton*>(QStringLiteral("lastfm-authorize"));
    auto* cancel = dialog->findChild<QPushButton*>(QStringLiteral("lastfm-cancel"));
    auto* poll = dialog->findChild<QTimer*>(QStringLiteral("lastfm-auth-poll"));
    auto* deadline = dialog->findChild<QTimer*>(QStringLiteral("lastfm-auth-deadline"));
    QVERIFY(key && reuse && begin && cancel && poll && deadline);
    QVERIFY(!dialog->findChild<QPushButton*>(QStringLiteral("lastfm-finish")));
    QVERIFY(reuse->isChecked());
    QVERIFY(cancel->isHidden());
    key->setText(QStringLiteral("invalid"));
    begin->click();
    QVERIFY(status->text().contains(QStringLiteral("32-character")));
    // Sharing the key with dynamic playlists is an explicit local setting.
    key->setText(QString(32, 'a'));
    secret->setText(QString(32, 'b'));
    begin->click();
    QCOMPARE(QSettings{}.value(QStringLiteral("lastfm/api-key")).toString(), QString(32, 'a'));
    auto* read_key = dialog->findChild<QLineEdit*>(QStringLiteral("bench-settings-lastfm-key"));
    QCOMPARE(read_key->text(), QString(32, 'a'));
    reuse->setChecked(false);
    key->setText(QString(32, 'c'));
    begin->click();
    QCOMPARE(QSettings{}.value(QStringLiteral("lastfm/api-key")).toString(), QString(32, 'a'));
    reply(QStringLiteral("status"),
          QByteArray(R"({"credentials_saved":true,"authorization_pending":true})"));
    QVERIFY(dialog->findChild<QWidget*>(QStringLiteral("lastfm-credentials"))->isHidden());
    QVERIFY(!poll->isActive());
    QVERIFY(begin->property("credentials-saved").toBool());
    // Feed deterministic replies without opening a real browser or contacting Last.fm.
    auto* page = dialog->findChild<QWidget*>(QStringLiteral("lastfm-settings"));
    auto* session = page->findChild<LastFmSettingsSession*>();
    QVERIFY(session != nullptr);
    session->setWaiting(true);
    reply(QStringLiteral("begin"),
          QByteArray(R"({"credentials_saved":true,"authorization_pending":true})"));
    QVERIFY(poll->isActive());
    QVERIFY(status->text().contains(QStringLiteral("Waiting for browser approval")));
    reply(QStringLiteral("finish"),
          QByteArray(R"({"credentials_saved":true,"authorization_pending":true})"));
    QVERIFY(poll->isActive());
    reply(QStringLiteral("finish"),
          QByteArray(
              R"({"credentials_saved":true,"connected":true,"user":"listener","enabled":true})"));
    QVERIFY(!poll->isActive());
    QVERIFY(!session->waiting());
    QVERIFY(dialog->findChild<QCheckBox*>(QStringLiteral("lastfm-enabled"))->isChecked());
    session->setWaiting(true);
    poll->start();
    cancel->click();
    QVERIFY(!poll->isActive());
    QVERIFY(status->text().contains(QStringLiteral("Stopped waiting")));
    // A late reply after cancellation must not restart polling.
    reply(QStringLiteral("finish"), QByteArray(R"({"authorization_pending":true})"));
    QVERIFY(!poll->isActive());
    session->setWaiting(true);
    poll->start();
    QMetaObject::invokeMethod(deadline, "timeout", Qt::DirectConnection);
    QVERIFY(!poll->isActive());
    QVERIFY(status->text().contains(QStringLiteral("timed out")));
    QVERIFY(cancel->isHidden());
    dialog->close();
    LocalListModel model;
    LocalTrackRow track;
    track.artist = "Artist";
    track.title = "Title";
    track.raw_path = "/test.flac";
    model.appendRows({track});
    QTableView view;
    view.setModel(&model);
    view.selectRow(0);
    QMenu menu;
    window.addLastFmActions(&menu, &view);
    QCOMPARE(menu.actions().size(), 1);
    auto* submenu = menu.actions().first()->menu();
    QVERIFY(submenu && submenu->isEnabled());
    QCOMPARE(submenu->actions()[1]->text(), QStringLiteral("Love track"));
    QCOMPARE(submenu->actions()[2]->text(), QStringLiteral("Unlove track"));
}

void BenchMainWindowTest::muteRestoresLocalVolumeAcrossBrowsing() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(!window.list_tabs_.empty());
    QTRY_VERIFY(window.playingOnEngine());
    auto* local = window.list_tabs_.front()->view;
    window.tabs_->setCurrentWidget(local);
    // The engine's volume: what the slider shows is what the engine reports.
    const auto engine_volume = [&window] { return window.localPlayback()->state().volume_percent; };
    window.volume_->setValue(37);
    QTRY_COMPARE(engine_volume(), 37);
    QTest::mouseClick(window.mute_button_, Qt::LeftButton);
    QTRY_COMPARE(engine_volume(), 0);
    QVERIFY(window.mute_button_->isChecked());
    QVERIFY(window.mute_button_->isEnabled());
    QTest::mouseClick(window.mute_button_, Qt::LeftButton);
    QTRY_COMPARE(engine_volume(), 37);
    QVERIFY(!window.mute_button_->isChecked());
    window.volume_->setValue(0);
    QTRY_COMPARE(engine_volume(), 0);
    QVERIFY(window.mute_button_->isChecked());
    window.volume_->setValue(21);
    QTRY_COMPARE(engine_volume(), 21);
    QVERIFY(!window.mute_button_->isChecked());
}

void BenchMainWindowTest::upNextMultiSelectionEdits() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.up_next_restored_ && !window.list_tabs_.empty());
    window.tabs_->setCurrentWidget(window.list_tabs_.front()->view);
    LocalTrackRow row;
    row.raw_path = "/tmp/request.flac";
    row.title = "Duplicate request";
    window.enqueueLocalRequests({row, row, row, row, row});
    const auto original = window.up_next_display_ids_;
    QCOMPARE(original.size(), std::size_t{5});
    // The header's pill counts what waits.
    auto* count = window.findChild<QLabel*>(QStringLiteral("bench-up-next-count"));
    QVERIFY(count != nullptr && !count->isHidden());
    QCOMPARE(count->text(), QStringLiteral("5"));
    auto* view = window.up_next_view_;
    QCOMPARE(view->selectionMode(), QAbstractItemView::ExtendedSelection);
    QCOMPARE(view->dragDropMode(), QAbstractItemView::DragDrop);
    QVERIFY(view->dragEnabled());
    QVERIFY(view->acceptDrops());
    QVERIFY(view->viewport()->acceptDrops());
    QVERIFY(view->showDropIndicator());
    QVERIFY(!view->dragDropOverwriteMode());
    QCOMPARE(view->defaultDropAction(), Qt::MoveAction);
    QVERIFY(view->model()->index(0, 0).flags().testFlag(Qt::ItemIsDragEnabled));
    const auto select = [&](std::initializer_list<int> rows) {
        view->selectionModel()->clearSelection();
        for (const auto i : rows)
            view->selectionModel()->select(view->model()->index(i, 0),
                                           QItemSelectionModel::Select | QItemSelectionModel::Rows);
    };
    select({1, 3});
    window.editUpNextSelection(4, 5);
    QCOMPARE(window.up_next_display_ids_,
             (std::vector<std::uint64_t>{original[0], original[2], original[4], original[1],
                                         original[3]}));
    QCOMPARE(view->selectionModel()->selectedRows().size(), 2);
    window.editUpNextSelection(2);
    QCOMPARE(window.up_next_display_ids_,
             (std::vector<std::uint64_t>{original[0], original[2], original[1], original[3],
                                         original[4]}));
    const auto moved = window.up_next_display_ids_;
    window.editUpNextSelection(1);
    QCOMPARE(window.playback_.requests.pending().size(), std::size_t{3});
    QVERIFY(window.playback_.requests.undo());
    window.refreshUpNext();
    QCOMPARE(window.up_next_display_ids_, moved);
    const auto revision = window.playback_.requests.revision();
    QVERIFY(!window.playback_.requests.retain({original[0], original[0]}));
    QCOMPARE(window.playback_.requests.revision(), revision);
    QCOMPARE(window.up_next_display_ids_, moved);
}

// ADR-0294: Up Next under the lists, its height kept; the side panels slide
// in, unless that is turned off.
void BenchMainWindowTest::upNextPanelAnimationAndSettings() {
    QSettings{}.setValue(QStringLiteral("appearance/panel-animations"), false);
    QSettings{}.setValue(QStringLiteral("up-next/visible"), false);
    QSettings{}.setValue(QStringLiteral("up-next/height"), 240);
    QSettings{}.setValue(QStringLiteral("lyrics/visible"), false);
    QSettings{}.setValue(QStringLiteral("lyrics/width"), 380);
    BenchMainWindow window;
    window.resize(1320, 760);
    window.show();
    QTRY_VERIFY(window.up_next_restored_);

    // Up Next: a section under the lists, not a side panel.
    auto* section = window.up_next_section_;
    auto* split = window.findChild<QSplitter*>(QStringLiteral("bench-lists-split"));
    QVERIFY(split != nullptr);
    QCOMPARE(split->orientation(), Qt::Vertical);
    QCOMPARE(split->widget(0), window.tabs_);
    QCOMPARE(split->widget(1), section);
    QVERIFY(qobject_cast<QDockWidget*>(section) == nullptr);
    auto* up_next = window.findChild<QAction*>(QStringLiteral("action-show-up-next"));
    QVERIFY(!section->isVisible());
    up_next->trigger();
    QTRY_VERIFY(section->isVisible() && window.up_next_view_->isVisible());
    QTRY_VERIFY(qAbs(split->sizes().value(1) - 240) <= 2);
    QVERIFY(section->geometry().top() >= window.tabs_->geometry().bottom());
    split->setSizes({split->height() - 300, 300});
    emit split->splitterMoved(split->height() - 300, 1);
    QTRY_COMPARE(QSettings{}.value(QStringLiteral("up-next/height")).toInt(), 300);
    up_next->trigger();
    QTRY_VERIFY(!section->isVisible());
    QVERIFY(!QSettings{}.value(QStringLiteral("up-next/visible")).toBool());
    up_next->trigger();
    QTRY_VERIFY(section->isVisible());
    QTRY_VERIFY(qAbs(split->sizes().value(1) - 300) <= 2);
    up_next->trigger();
    QTRY_VERIFY(!section->isVisible());
    // With panel animations on, it slides up to that height and back down.
    QSettings{}.setValue(QStringLiteral("appearance/panel-animations"), true);
    auto* slide = split->findChild<QVariantAnimation*>();
    QVERIFY(slide != nullptr);
    up_next->trigger();
    QTRY_COMPARE(slide->state(), QAbstractAnimation::Running);
    QVERIFY(section->isVisible() && split->sizes().value(1) < 300);
    QTRY_COMPARE(slide->state(), QAbstractAnimation::Stopped);
    QTRY_VERIFY(qAbs(split->sizes().value(1) - 300) <= 2);
    up_next->trigger();
    QTRY_COMPARE(slide->state(), QAbstractAnimation::Running);
    QTRY_VERIFY(!section->isVisible());
    QSettings{}.setValue(QStringLiteral("appearance/panel-animations"), false);

    // A side panel slides in, as wide as it was left.
    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("bench-lyrics"));
    auto* animation = dock->findChild<QVariantAnimation*>();
    QVERIFY(animation);
    auto* toggle = window.findChild<QAction*>(QStringLiteral("action-show-lyrics"));
    QSettings{}.setValue(QStringLiteral("appearance/panel-animations"), true);
    toggle->trigger();
    QTRY_COMPARE(animation->state(), QAbstractAnimation::Running);
    QTest::qWait(40);
    toggle->trigger();
    QVERIFY(!toggle->isChecked());
    QTest::qWait(30);
    toggle->trigger();
    QTRY_COMPARE(animation->state(), QAbstractAnimation::Stopped);
    QVERIFY(dock->isVisible());
    QVERIFY(toggle->isChecked());
    QVERIFY(qAbs(dock->width() - 380) <= 2);
    window.resizeDocks({dock}, {460}, Qt::Horizontal);
    QTRY_COMPARE(QSettings{}.value(QStringLiteral("lyrics/width")).toInt(), 460);
    toggle->trigger();
    QTRY_VERIFY(!dock->isVisible());
    QCOMPARE(QSettings{}.value(QStringLiteral("lyrics/width")).toInt(), 460);
    auto* dialog = window.showSettingsDialog(SettingsDialog::Page::general);
    auto* checkbox =
        dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-panel-animations"));
    QVERIFY(checkbox && checkbox->isChecked());
    checkbox->setChecked(false);
    dialog->reject();
    QVERIFY(QSettings{}.value(QStringLiteral("appearance/panel-animations")).toBool());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    dialog = window.showSettingsDialog(SettingsDialog::Page::general);
    checkbox = dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-panel-animations"));
    checkbox->setChecked(false);
    auto* buttons = dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"));
    QTest::mouseClick(buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
    QVERIFY(!QSettings{}.value(QStringLiteral("appearance/panel-animations")).toBool());
    toggle->trigger();
    QVERIFY(dock->isVisible());
    QCOMPARE(animation->state(), QAbstractAnimation::Stopped);
    toggle->trigger();
    QVERIFY(!dock->isVisible());

    // The artists' initials in the library: grey, or -- set so -- on tiles
    // of their names' colours, the library redrawn once Settings is saved.
    auto* library = window.findChild<LocalLibraryPanel*>();
    QVERIFY(library != nullptr && !library->colouredInitials());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    dialog = window.showSettingsDialog(SettingsDialog::Page::general);
    auto* coloured =
        dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-coloured-initials"));
    QVERIFY(coloured && !coloured->isChecked());
    coloured->setChecked(true);
    buttons = dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"));
    QTest::mouseClick(buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
    QVERIFY(QSettings{}.value(QStringLiteral("appearance/library-coloured-initials")).toBool());
    QVERIFY(library->colouredInitials());
    // Drawn so: an artist's tile is a colour, not a grey.
    {
        ui::LibraryTreeView view;
        QStandardItemModel model;
        model.appendRow(new QStandardItem(QStringLiteral("Royal Thunder")));
        view.setModel(&model);
        ui::LibraryTreeDelegate delegate{&view, {}, [](const QModelIndex&) {
                                             return ui::LibraryTreeDelegate::Presentation{
                                                 .track = false, .album = false, .root = true,
                                                 .artist = true, .secondary = {}, .count = {},
                                                 .album_rating = 0U};
                                         }};
        const auto tile_saturation = [&view, &model, &delegate](const bool on) {
            delegate.setColouredInitials(on);
            QImage image{300, 40, QImage::Format_ARGB32};
            image.fill(Qt::white);
            QPainter painter{&image};
            QStyleOptionViewItem option;
            option.initFrom(&view);
            option.rect = image.rect();
            option.text = QStringLiteral("Royal Thunder");
            delegate.paint(&painter, option, model.index(0, 0));
            painter.end();
            // A corner of the tile, clear of its letters.
            return image.pixelColor(QPoint{7, 10}).hslSaturationF();
        };
        QVERIFY(tile_saturation(false) < 0.1F);
        QVERIFY(tile_saturation(true) > 0.25F);
    }
    dialog = window.showSettingsDialog(SettingsDialog::Page::general);
    dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-coloured-initials"))
        ->setChecked(false);
    buttons = dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"));
    QTest::mouseClick(buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
    QVERIFY(!library->colouredInitials());
    QSettings{}.remove(QStringLiteral("lyrics"));
    QSettings{}.remove(QStringLiteral("up-next"));
}

void BenchMainWindowTest::upNextEditingAndPersistence() {
    audio::RequestQueue<std::string> queue;
    QVERIFY(queue.insert({"A", "A"}, 0));
    const auto first = queue.pending()[0];
    const auto second = queue.pending()[1];
    QVERIFY(first.id != second.id);
    QVERIFY(queue.insert({"X"}, 0));
    QCOMPARE(queue.pending()[0].source, std::string{"X"});
    QVERIFY(queue.canUndo());
    QVERIFY(queue.undo());
    QCOMPARE(queue.pending().size(), 2U);
    QVERIFY(queue.move(second.id, 0));
    QCOMPARE(queue.pending()[0].id, second.id);
    queue.started(second);
    QVERIFY(!queue.canUndo());
    queue.clear();
    QVERIFY(queue.active());
    QVERIFY(queue.pending().empty());
    queue.finished();
    QVERIFY(!queue.active());
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.up_next_restored_);
    window.tabs_->setCurrentIndex(0);
    LocalTrackRow row;
    row.raw_path = std::string{"/tmp/raw-"} + char(0xff) + ".flac";
    row.title = "Request";
    window.enqueueLocalRequests({row, row});
    QCOMPARE(window.playback_.requests.pending().size(), 2U);
    QVERIFY(!window.up_next_view_->albumGroupingEnabled());
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        window.findChild<QAction*>(QStringLiteral("action-show-up-next"))->setChecked(true);
        window.resize(1320, 760);
        QTest::qWait(250);
        QVERIFY(window.grab().save(directory + QStringLiteral("/up-next.png")));
    }
    window.editUpNext(1, 0);
    QCOMPARE(window.playback_.requests.pending().size(), 1U);
    bool saved = false;
    window.persistence_->loadUiState(QStringLiteral("playback/up-next/v1"),
                                     [&](QByteArray payload, QString error) {
                                         QVERIFY(error.isEmpty());
                                         QVERIFY(payload.contains("Request"));
                                         saved = true;
                                     });
    QTRY_VERIFY(saved);
    window.playback_.requests.clear();
    window.up_next_restored_ = false;
    window.restoreUpNext();
    QTRY_VERIFY(window.up_next_restored_);
    QCOMPARE(window.playback_.requests.pending().size(), 1U);
    QCOMPARE(window.playback_.requests.pending()[0].source.raw_path, row.raw_path);
}

// ADR-0227: this computer's engine and a remote one, side by side. Local tabs
// play here and remote tabs there, never both at once; the remote library
// fills remote tabs from its own index; and nothing mixes the two.
// Another client -- a picker, a script -- starts the remote engine while the
// window follows this computer's: the window follows the music there, and
// the remote's tab shows what plays, rather than the list it had.
// A remote that is away when the window opens has its tab made anyway, named
// by its address; when it comes back and says what it is called, the tab
// takes that name. It used to keep the address -- or an old name -- for good.
void BenchMainWindowTest::theRemoteTabTakesTheNameItsEngineAnnounces() {
    QTemporaryDir remote_state;
    QVERIFY(remote_state.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    remote.stop();

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QVERIFY(window.remoteCatalogue() != nullptr);
    auto* tab = window.remoteQueueTab();
    QVERIFY(tab != nullptr);
    const auto address = window.remoteCatalogue()->addressName();
    QCOMPARE(displayText(tab->document.name), address);

    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    QTRY_VERIFY_WITH_TIMEOUT(window.remotePlayback()->active(), 10'000);
    QTRY_VERIFY(displayText(tab->document.name) != address);
    QCOMPARE(displayText(tab->document.name), window.remoteCatalogue()->name());
}

// A remote engine over TCP whose host is off -- the NAS -- held a connect for
// about two minutes of SYN retries, on the window's thread, at startup and
// again every few seconds after. It is reached from a worker now.
void BenchMainWindowTest::anUnreachableRemoteDoesNotHoldTheWindow() {
    QSettings settings;
    // TEST-NET-1 (RFC 5737): routed nowhere, so a connect waits rather than
    // being refused.
    settings.setValue(QLatin1String(SettingsDialog::library_engine_socket_key),
                      QStringLiteral("192.0.2.1:6603"));
    settings.setValue(QLatin1String(SettingsDialog::library_engine_token_key),
                      QStringLiteral("a password"));
    settings.sync();

    QElapsedTimer opening;
    opening.start();
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QVERIFY2(opening.elapsed() < 2'000, QByteArray::number(opening.elapsed()).constData());
    QVERIFY(window.remotePlayback() != nullptr);
    QVERIFY(!window.remotePlayback()->active());

    // And across the reconnect timer's next attempt, the event loop keeps
    // turning.
    QElapsedTimer waiting;
    waiting.start();
    qint64 longest = 0;
    qint64 last = 0;
    while (waiting.elapsed() < 4'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QTest::qWait(20);
        longest = std::max(longest, waiting.elapsed() - last);
        last = waiting.elapsed();
    }
    QVERIFY2(longest < 1'000, QByteArray::number(longest).constData());
    settings.remove(QLatin1String(SettingsDialog::library_engine_socket_key));
    settings.remove(QLatin1String(SettingsDialog::library_engine_token_key));
}

// An album queued and played by another client -- melody-cli -- is in no
// list this window has shown, so its cover is not at hand when it starts.
// The notification waits for the cover rather than going out without it.
void BenchMainWindowTest::aNotificationWaitsForItsCover() {
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    // A minute long, so it is still playing when the window looks, with its
    // cover beside it as an album folder has.
    const auto path = media.filePath(QStringLiteral("track.wav"));
    write_wave(path, wave_sample_rate * 60U);
    QImage cover{64, 64, QImage::Format_RGB32};
    cover.fill(Qt::darkCyan);
    QVERIFY(cover.save(media.filePath(QStringLiteral("cover.jpg")), "JPEG"));

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    QVERIFY(window.notifier_ != nullptr);
    window.notifier_->setEnabled(true);
    window.notifier_->setBackgroundOnly(false);
    QStringList sent;
    window.notifier_->setSendOverride(
        [&sent](const QString& summary, const QString&) { sent << summary; });

    auto other = protocol::Client::connect(protocol::Endpoint{
        .socket = remote.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(other.has_value());
    const auto entry = core::StableId::random().to_string();
    auto entries = protocol::Json::array();
    entries.push_back(
        protocol::Json{{"entry", entry},
                       {"path", protocol::encode_raw_path(QFile::encodeName(path).toStdString())},
                       {"title", "With a cover"},
                       {"group", protocol::Json{{"album_artist", "Band"},
                                                {"artist", "Band"},
                                                {"album", "Covered"},
                                                {"date", "1992"}}}});
    QVERIFY((*other)
                ->call("playback.replace_queue", protocol::Json{{"entries", std::move(entries)}})
                .has_value());
    QVERIFY((*other)->call("playback.play", protocol::Json{{"entry", entry}}).has_value());

    QTRY_VERIFY_WITH_TIMEOUT(!sent.isEmpty(), 10'000);
    QCOMPARE(sent.size(), 1);
    QCOMPARE(sent.front(), QStringLiteral("With a cover"));
    QVERIFY2(!window.notifier_->lastImage().isEmpty(), "the notification shows the cover");
    QVERIFY(QFileInfo::exists(window.notifier_->lastImage()));
}

// An album melody-cli queued and played is in no list of the window's until
// its queue has come, after the entry that plays: a panel asked then was told
// no tags, and the About plugin said the track names no artist. What plays is
// the engine's to say.
void BenchMainWindowTest::aPluginPanelIsToldWhatAnotherClientStarted() {
    const auto folder = pluginFolders().value(0) + QStringLiteral("/about");
    QVERIFY(QDir{}.mkpath(folder));
    {
        QFile manifest{folder + QStringLiteral("/plugin.json")};
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write(R"({"schema": 1, "name": "About", "panels": [{"id": "artist",
                          "label": "About the artist", "run": ["./about"]}]})");
        QFile script{folder + QStringLiteral("/about")};
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(R"sh(#!/bin/sh
artist=$(cat | grep -o '"artist":"[^"]*"' | head -n 1 | cut -d '"' -f 4)
printf '{"title": "About %s", "parts": []}' "$artist"
)sh");
        script.close();
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("about")), true);

    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    const auto path = media.filePath(QStringLiteral("track.wav"));
    write_wave(path, wave_sample_rate * 60U);

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("bench-plugin-panel-about-artist"));
    QVERIFY(dock != nullptr);
    if (dock->isHidden()) {
        window.findChild<QAction*>(QStringLiteral("action-toggle-panels"))->trigger();
    }
    QTRY_VERIFY(dock->isVisible());

    auto other = protocol::Client::connect(protocol::Endpoint{
        .socket = remote.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(other.has_value());
    const auto entry = core::StableId::random().to_string();
    auto entries = protocol::Json::array();
    entries.push_back(
        protocol::Json{{"entry", entry},
                       {"path", protocol::encode_raw_path(QFile::encodeName(path).toStdString())},
                       {"title", "Over the Wall"},
                       {"group", protocol::Json{{"artist", "Testament"},
                                                {"album", "The Legacy"},
                                                {"date", "1987"}}}});
    QVERIFY((*other)
                ->call("playback.replace_queue", protocol::Json{{"entries", std::move(entries)}})
                .has_value());
    QVERIFY((*other)->call("playback.play", protocol::Json{{"entry", entry}}).has_value());

    QLabel* title = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        (title = dock->findChild<QLabel*>(QStringLiteral("bench-plugin-panel-title"))) != nullptr,
        10'000);
    // Its first answer, not one asked again once the queue had come.
    QCOMPARE(title->text(), QStringLiteral("About Testament"));
    QSettings{}.remove(QStringLiteral("plugins"));
    QDir{folder}.removeRecursively();
}

void BenchMainWindowTest::theWindowFollowsAnEngineStartedElsewhere() {
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    std::vector<std::string> paths;
    for (const auto* name : {"old.wav", "one.wav", "two.wav", "three.wav"}) {
        const auto path = media.filePath(QString::fromLatin1(name));
        write_wave(path, wave_sample_rate * 60U);
        paths.push_back(QFile::encodeName(path).toStdString());
    }

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    QVERIFY(window.transport_ == window.localPlayback());

    auto other = protocol::Client::connect(protocol::Endpoint{
        .socket = remote.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(other.has_value());
    const auto queue = [&](const std::vector<std::size_t>& which) {
        std::vector<std::string> ids;
        auto entries = protocol::Json::array();
        for (const auto index : which) {
            ids.push_back(core::StableId::random().to_string());
            entries.push_back(protocol::Json{{"entry", ids.back()},
                                             {"path", protocol::encode_raw_path(paths[index])},
                                             {"title", "Track " + std::to_string(index)},
                                             {"group", protocol::Json{{"album_artist", "Band"},
                                                                      {"artist", "Band"},
                                                                      {"album", "Record"},
                                                                      {"date", "1992"}}}});
        }
        static_cast<void>((*other)->call("playback.replace_queue",
                                         protocol::Json{{"entries", std::move(entries)}}));
        return ids;
    };
    // The server's tab already shows a list: an album played there before.
    static_cast<void>(queue({0}));
    auto* tab = window.remoteQueueTab();
    QVERIFY(tab != nullptr);

    // A picker replaces it with an album and plays its second track -- both
    // reaching the window in one report.
    const auto album = queue({1, 2, 3});
    QVERIFY((*other)->call("playback.play", protocol::Json{{"entry", album[1]}}).has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds{600});

    QTRY_VERIFY_WITH_TIMEOUT(window.transport_ == window.remotePlayback(), 10'000);
    const auto id = [](const std::string& text) { return *core::StableId::parse(text); };
    QTRY_VERIFY(tab->model->rowOfEntry(id(album[0]), -1) >= 0 &&
                tab->model->rowOfEntry(id(album[2]), -1) >= 0);
    QCOMPARE(tab->model->rowCount(), 3);
    QCOMPARE(window.playback_.anchors.document, tab->document.id);
    // Its row is marked as the one playing, and stays so -- not the row
    // where the list's old track was, and not only once the next one plays.
    const auto marked = [&](const std::string& entry) {
        const auto row = tab->model->rowOfEntry(id(entry), -1);
        return row >= 0 && tab->model->index(row, 0).data(ui::track_current_role).toBool();
    };
    QTRY_VERIFY2(marked(album[1]), "the playing row is not marked");
    QVERIFY(!marked(album[0]) && !marked(album[2]));
    QTest::qWait(1'500);
    QVERIFY2(marked(album[1]), "the playing row lost its mark");
    // The tags the other client gave are the engine's still: the window,
    // following, neither showed filenames nor sent its rows back in their
    // place -- which left a phone showing no tags for what it had queued.
    const auto held = (*other)->call("playback.queue");
    QVERIFY(held.has_value());
    QCOMPARE(held->at("entries").size(), std::size_t{3});
    for (const auto& entry : held->at("entries")) {
        QVERIFY2(entry.value("title", std::string{}).starts_with("Track "), entry.dump().c_str());
        QCOMPARE(entry.at("group").value("album", std::string{}), std::string{"Record"});
    }
    const auto row = tab->model->rowOfEntry(id(album[0]), -1);
    QCOMPARE(tab->model->rows()[static_cast<std::size_t>(row)].album, std::string{"Record"});
    static_cast<void>((*other)->call("playback.stop"));
    (*other)->close();
}

// The engine the window already follows has its list replaced by another
// client, and plays the new one's first track: that row is marked at once
// -- not the row where the old list's track was, and not only once the
// next track starts.
void BenchMainWindowTest::aListReplacedOnTheFollowedEngineMarksWhatPlays() {
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    std::vector<std::string> paths;
    for (const auto* name : {"old-a.wav", "old-b.wav", "old-c.wav", "new-a.wav", "new-b.wav"}) {
        const auto path = media.filePath(QString::fromLatin1(name));
        write_wave(path, wave_sample_rate * 60U);
        paths.push_back(QFile::encodeName(path).toStdString());
    }

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());

    auto other = protocol::Client::connect(protocol::Endpoint{
        .socket = remote.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(other.has_value());
    const auto queue = [&](const std::vector<std::size_t>& which) {
        std::vector<std::string> ids;
        auto entries = protocol::Json::array();
        for (const auto index : which) {
            ids.push_back(core::StableId::random().to_string());
            entries.push_back(protocol::Json{{"entry", ids.back()},
                                             {"path", protocol::encode_raw_path(paths[index])},
                                             {"title", "Track " + std::to_string(index)}});
        }
        static_cast<void>((*other)->call("playback.replace_queue",
                                         protocol::Json{{"entries", std::move(entries)}}));
        return ids;
    };
    const auto id = [](const std::string& text) { return *core::StableId::parse(text); };
    auto* tab = window.remoteQueueTab();
    QVERIFY(tab != nullptr);
    const auto marked = [&](const std::string& entry) {
        const auto row = tab->model->rowOfEntry(id(entry), -1);
        return row >= 0 && tab->model->index(row, 0).data(ui::track_current_role).toBool();
    };

    // The server plays the old list's second track; the window follows it.
    const auto old_list = queue({0, 1, 2});
    QVERIFY((*other)->call("playback.play", protocol::Json{{"entry", old_list[1]}}).has_value());
    QTRY_VERIFY_WITH_TIMEOUT(window.transport_ == window.remotePlayback(), 10'000);
    QTRY_VERIFY(marked(old_list[1]));

    // Replaced, and its first track played -- in one report.
    const auto album = queue({3, 4});
    QVERIFY((*other)->call("playback.play", protocol::Json{{"entry", album[0]}}).has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds{600});

    QTRY_VERIFY(tab->model->rowOfEntry(id(album[0]), -1) >= 0);
    QTRY_VERIFY2(marked(album[0]), "the new list's playing row is not marked");
    QVERIFY2(!marked(album[1]), "the row where the old track was is marked instead");
    QTest::qWait(1'500);
    QVERIFY2(marked(album[0]), "the playing row lost its mark");
    static_cast<void>((*other)->call("playback.stop"));
    (*other)->close();
}

} // namespace trackknife::bench
