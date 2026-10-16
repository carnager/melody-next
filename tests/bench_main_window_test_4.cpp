// SPDX-License-Identifier: GPL-3.0-only

#include "bench_main_window_test.hpp"

namespace trackknife::bench {

// The desktop says dark only after the window is up, and the scheme's
// palette comes later: the status bar's painted icons -- album shuffle --
// are painted again in its colours, not left in the light palette's dark.
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

// ADR-0261: albums that fit more than one release wait for the person, who
// goes through them from the keyboard.
// ADR-0262: Identify albums… straight from Tools, without the tag editor:
// the selected tracks, or with none selected the whole list.
void BenchMainWindowTest::identifyAlbumsOpensFromTools() {
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
    auto* identify = window.findChild<QAction*>(QStringLiteral("action-identify-albums"));
    QVERIFY(identify != nullptr);
    view->selectionModel()->clearSelection();
    window.refreshSelectionStatus();
    QTRY_VERIFY(identify->isEnabled());

    const auto open = [&window, identify] {
        identify->trigger();
        IdentifyAlbumsDialog* dialog = nullptr;
        for (auto* candidate : window.findChildren<IdentifyAlbumsDialog*>()) {
            if (candidate->isVisible()) {
                dialog = candidate;
            }
        }
        return dialog;
    };
    // None selected: the whole list, read and grouped -- and no tag editor.
    auto* whole = open();
    QVERIFY(whole != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(whole->session() != nullptr, 10'000);
    QCOMPARE(whole->session()->fileCount(), 2U);
    QCOMPARE(whole->session()->order().size(), 1U);
    QVERIFY(window.findChildren<MetadataPropertiesDialog*>().isEmpty());
    delete whole;

    // One selected: that one.
    view->selectionModel()->select(list_model->index(1, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    auto* chosen = open();
    QVERIFY(chosen != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(chosen->session() != nullptr, 10'000);
    QCOMPARE(chosen->session()->fileCount(), 1U);
    QCOMPARE(chosen->tagger()->itemSource(0U)->raw_path, paths[1]);

    // Closed, it is asked once whether it may: QDialog's own closeEvent
    // goes through reject() too, which asked a second time.
    struct CloseCounter final : QObject {
        int closes = 0;
        bool eventFilter(QObject* watched, QEvent* event) override {
            if (event->type() == QEvent::Close) {
                ++closes;
            }
            return QObject::eventFilter(watched, event);
        }
    } counter;
    chosen->installEventFilter(&counter);
    chosen->close();
    QTRY_VERIFY(!chosen->isVisible());
    QCOMPARE(counter.closes, 1);
    delete chosen;
}

// Albums staged one after another while an automatic script re-stages its
// edits after each: no stage is refused for a draft that moved under it,
// and the script's edit lands on every file.
void BenchMainWindowTest::identifyAlbumsStagesAlongsideAutomaticScripts() {
    const auto field = [](std::string name, std::vector<std::string> values) {
        return metadata::MetadataField{.canonical_name = metadata::canonicalize_field_name(name),
                                       .native_name = std::move(name),
                                       .values = std::move(values),
                                       .qualifier = {},
                                       .provenance = metadata::FieldProvenance::embedded};
    };
    const auto make_source = [&field](std::string path, std::string artist, std::string title,
                                      std::string number, std::int64_t length) {
        return MetadataPropertiesSource{
            .source =
                metadata::StagedMetadataSource{
                    .raw_path = std::move(path),
                    .source_revision = std::nullopt,
                    .baseline =
                        metadata::MetadataDocument{
                            .fields = {field("ALBUM", {"Alpha"}), field("ARTIST", {artist}),
                                       field("TITLE", {title}), field("TRACKNUMBER", {number}),
                                       field("DATE", {"2001-05-05"}),
                                       // Totals twice, under both their names.
                                       field("TRACKTOTAL", {"2"}), field("TOTALTRACKS", {"2"}),
                                       field("DISCTOTAL", {"1"}), field("TOTALDISCS", {"1"})},
                            .unsupported_native_objects = {}}},
            .track_label = {},
            .duration_ms = length,
        };
    };
    std::vector<MetadataPropertiesSource> sources;
    for (const auto* artist : {"Band", "Cover", "Third"}) {
        sources.push_back(make_source(std::string{"/music/"} + artist + "/Alpha/01.flac", artist,
                                      "One", "1", 61'000));
        sources.push_back(make_source(std::string{"/music/"} + artist + "/Alpha/02.flac", artist,
                                      "Two", "2", 59'000));
    }
    const auto release = [](const QString& artist, const QString& id) {
        return QStringLiteral(R"json({"id": "%1", "score": 100, "title": "Alpha",
          "status": "Official", "date": "1999-09-09", "country": "DE", "track-count": 2,
          "artist-credit": [{"name": "%2",
            "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "name": "%2"}}],
          "release-group": {"id": "99999999-8888-7777-6666-555555555555"},
          "media": [{"position": 1, "format": "CD", "track-count": 2, "tracks": [
            {"id": "aaaa1111-0000-0000-0000-000000000001", "position": 1, "number": "1",
             "title": "One", "length": 61000,
             "recording": {"id": "bbbb1111-0000-0000-0000-000000000001", "title": "One"}},
            {"id": "aaaa1111-0000-0000-0000-000000000002", "position": 2, "number": "2",
             "title": "Two", "length": 59000,
             "recording": {"id": "bbbb1111-0000-0000-0000-000000000002", "title": "Two"}}
          ]}]})json")
            .arg(id, artist);
    };
    const std::map<QString, QString> ids{
        {QStringLiteral("Band"), QStringLiteral("11111111-0000-0000-0000-000000000001")},
        {QStringLiteral("Cover"), QStringLiteral("22222222-0000-0000-0000-000000000002")},
        {QStringLiteral("Third"), QStringLiteral("33333333-0000-0000-0000-000000000003")}};
    const MusicBrainzLookupService service{
        .fetch =
            [&](const QString& url, std::function<void(core::Result<QByteArray>)> completion) {
                const auto asked = QUrl::fromPercentEncoding(url.toUtf8());
                QString artist = QStringLiteral("Band");
                for (const auto& [name, id] : ids) {
                    if (asked.contains(name) || asked.contains(id)) {
                        artist = name;
                    }
                }
                const auto body = url.contains(QStringLiteral("?query="))
                                      ? QStringLiteral(R"({"count": 1, "releases": [%1]})")
                                            .arg(release(artist, ids.at(artist)))
                                            .toUtf8()
                                      : release(artist, ids.at(artist)).toUtf8();
                QTimer::singleShot(
                    1, [body, completion = std::move(completion)] { completion(body); });
            },
        .fingerprint = {},
        .acoustid_lookup = {},
    };
    // Every date to its year, after each stage.
    const std::vector chains{persistence::SavedMetadataTransformationChain{
        .id = core::StableId::random(),
        .chain =
            metadata::MetadataTransformationChain{
                .schema_version = 1U,
                .name = "Year only",
                .actions = {metadata::MetadataFormatValueAction{
                    .target_field = "date", .dialect = {}, .source = "$left(%date%,4)"}},
            },
        .automatic = true,
    }};
    const MetadataTransformationStore scripts{
        .load =
            [chains](MetadataTransformationStore::LoadCompletion completion) {
                completion(chains, {});
            },
        .save = {},
        .remove = {},
    };
    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {}, {}, {}, scripts, {}, {}, {}, nullptr, {}, service);
    properties->show();
    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* tagger = properties->findChild<TaggerSession*>();
    auto* open =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-identify-albums"));
    QTRY_VERIFY(open->isEnabled());
    open->click();
    auto* dialog =
        properties->findChild<IdentifyAlbumsDialog*>(QStringLiteral("bench-identify-albums"));
    auto* session = dialog->session();
    QTRY_COMPARE_WITH_TIMEOUT(session->count(AlbumBatchSession::State::staged), std::size_t{3U},
                              10'000);
    QVERIFY2(!tagger->status().contains(QStringLiteral("rejected")), qPrintable(tagger->status()));
    // The script's year on every file, over MusicBrainz's full date.
    QTRY_VERIFY_WITH_TIMEOUT(
        [&] {
            const auto date = grid_model->selection().field_index("date");
            if (!date) {
                return false;
            }
            for (std::size_t item = 0U; item < sources.size(); ++item) {
                const auto* patch = grid_model->patches().patch(item, *date);
                if (patch == nullptr || patch->values != std::vector<std::string>{"1999"}) {
                    return false;
                }
            }
            return true;
        }(),
        10'000);
    delete properties;
}

void BenchMainWindowTest::identifyAlbumsReviewsWhatNeedsYou() {
    const auto field = [](std::string name, std::vector<std::string> values) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = std::move(values),
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    const auto make_source = [&field](std::string path, std::string artist, std::string title,
                                      std::string number, std::int64_t length) {
        return MetadataPropertiesSource{
            .source =
                metadata::StagedMetadataSource{
                    .raw_path = std::move(path),
                    .source_revision = std::nullopt,
                    .baseline =
                        metadata::MetadataDocument{
                            .fields = {field("ALBUM", {"Alpha"}), field("ARTIST", {artist}),
                                       field("TITLE", {title}), field("TRACKNUMBER", {number})},
                            .unsupported_native_objects = {}},
                },
            .track_label = {},
            .duration_ms = length,
        };
    };
    const std::vector sources{
        make_source("/music/Band/Alpha/01.flac", "Band", "One", "1", 61'000),
        make_source("/music/Band/Alpha/02.flac", "Band", "Two", "2", 59'000),
        make_source("/music/Cover/Alpha/01.flac", "Cover", "One", "1", 61'000),
        make_source("/music/Cover/Alpha/02.flac", "Cover", "Two", "2", 59'000),
    };
    // Two editions, track for track the same: both fit clearly.
    const auto release = [](const char* id) {
        return QStringLiteral(R"json({"id": "%1", "score": 100, "title": "Alpha",
          "status": "Official", "date": "1999-09-09", "country": "DE", "track-count": 2,
          "artist-credit": [{"name": "Band",
            "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "name": "Band"}}],
          "release-group": {"id": "99999999-8888-7777-6666-555555555555"},
          "media": [{"position": 1, "format": "CD", "track-count": 2, "tracks": [
            {"id": "aaaa1111-0000-0000-0000-000000000001", "position": 1, "number": "1",
             "title": "One", "length": 61000,
             "recording": {"id": "bbbb1111-0000-0000-0000-000000000001", "title": "One"}},
            {"id": "aaaa1111-0000-0000-0000-000000000002", "position": 2, "number": "2",
             "title": "Two", "length": 59000,
             "recording": {"id": "bbbb1111-0000-0000-0000-000000000002", "title": "Two"}}
          ]}]})json")
            .arg(QString::fromLatin1(id));
    };
    const auto first = "11111111-0000-0000-0000-000000000001";
    const auto second = "22222222-0000-0000-0000-000000000002";
    const MusicBrainzLookupService service{
        .fetch =
            [&](const QString& url, std::function<void(core::Result<QByteArray>)> completion) {
                QByteArray body;
                if (url.contains(QStringLiteral("?query="))) {
                    body = QStringLiteral(R"({"count": 2, "releases": [%1, %2]})")
                               .arg(release(first), release(second))
                               .toUtf8();
                } else {
                    body = release(url.contains(QString::fromLatin1(second)) ? second : first)
                               .toUtf8();
                }
                QTimer::singleShot(
                    1, [body, completion = std::move(completion)] { completion(body); });
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
    auto* open =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-identify-albums"));
    QTRY_VERIFY(open->isEnabled());
    open->click();
    auto* dialog =
        properties->findChild<IdentifyAlbumsDialog*>(QStringLiteral("bench-identify-albums"));
    auto* session = dialog->session();
    QCOMPARE(session->albums().size(), 2U);
    auto* review_next =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-identify-albums-review-next"));
    // Looked up at once; the first album needing a person opens by itself.
    auto* heading =
        dialog->findChild<QLabel*>(QStringLiteral("bench-identify-albums-review-heading"));
    auto* versions =
        dialog->findChild<QTreeWidget*>(QStringLiteral("bench-identify-albums-versions"));
    QTRY_VERIFY_WITH_TIMEOUT(heading->isVisible(), 5'000);
    QVERIFY(heading->text().contains(QStringLiteral("Band — Alpha")));
    QCOMPARE(versions->topLevelItemCount(), 2);
    // Told apart by what differs; the title column only when titles do.
    QCOMPARE(versions->topLevelItem(0)->text(1), QStringLiteral("1999-09-09"));
    QCOMPARE(versions->topLevelItem(0)->text(5), QStringLiteral("1×CD"));
    QVERIFY(versions->isColumnHidden(0));
    // The file buttons once a file is chosen.
    QVERIFY(!dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-up"))
                 ->isVisibleTo(dialog));
    QTRY_COMPARE_WITH_TIMEOUT(session->count(AlbumBatchSession::State::needs_choice),
                              std::size_t{2U}, 5'000);
    // ↓ chooses the other version; the matcher follows.
    QTest::keyClick(versions, Qt::Key_Down);
    QCOMPARE(versions->indexOfTopLevelItem(versions->currentItem()), 1);
    auto* accept =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-identify-albums-review-accept"));
    QTRY_VERIFY(accept->isEnabled());
    QCOMPARE(
        dialog->findChild<QLabel*>(QStringLiteral("bench-identify-albums-review-status"))->text(),
        QStringLiteral("2 of 2 files paired"));
    // The matcher's own heading and Stage are the window's here.
    QVERIFY(!dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-stage"))
                 ->isVisible());
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(dialog->grab().save(directory + QStringLiteral("/identify-albums-review.png")));
    }
    // Enter accepts, and the next album needing a person opens.
    QTest::keyClick(versions, Qt::Key_Return);
    QTRY_VERIFY(heading->text().contains(QStringLiteral("Cover — Alpha")));
    QTRY_COMPARE_WITH_TIMEOUT(session->count(AlbumBatchSession::State::staged), std::size_t{1U},
                              5'000);
    QVERIFY(grid_model->patches().patch_count() > 0U);
    // S passes it over, and with none left the list is back.
    QTest::keyClick(versions, Qt::Key_S);
    QCOMPARE(session->count(AlbumBatchSession::State::skipped), std::size_t{1U});
    QTRY_VERIFY(!heading->isVisible());
    // Back in the list, nothing left to review: not offered.
    QVERIFY(!review_next->isVisibleTo(dialog));
    delete properties;
}

// ADR-0265: a file grouped into another album -- here by a release id of
// its own, one folder with its album -- is offered in the matcher, put on
// its track, and leaves the album it was grouped into.
void BenchMainWindowTest::identifyAlbumsTakesAFileGroupedApart() {
    const auto field = [](std::string name, std::vector<std::string> values) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = std::move(values),
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    const auto first = "11111111-0000-0000-0000-000000000001";
    const auto stray = "22222222-0000-0000-0000-000000000002";
    const auto make_source = [&field](std::string path, std::string release, std::string title,
                                      std::string number, std::int64_t length) {
        return MetadataPropertiesSource{
            .source =
                metadata::StagedMetadataSource{
                    .raw_path = std::move(path),
                    .source_revision = std::nullopt,
                    .baseline =
                        metadata::MetadataDocument{
                            .fields = {field("ALBUM", {"Suffer"}), field("ARTIST", {"Band"}),
                                       field("TITLE", {title}), field("TRACKNUMBER", {number}),
                                       field("MUSICBRAINZ_ALBUMID", {release})},
                            .unsupported_native_objects = {}},
                },
            .track_label = {},
            .duration_ms = length,
        };
    };
    const std::vector sources{
        make_source("/music/Band/Suffer/01.flac", first, "One", "1", 61'000),
        make_source("/music/Band/Suffer/02.flac", first, "Two", "2", 59'000),
        make_source("/music/Band/Suffer/03.flac", stray, "Three", "3", 75'000),
    };
    const auto release = [](const char* id) {
        return QStringLiteral(R"json({"id": "%1", "score": 100, "title": "Suffer",
          "status": "Official", "date": "1988", "country": "US", "track-count": 3,
          "artist-credit": [{"name": "Band",
            "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "name": "Band"}}],
          "release-group": {"id": "99999999-8888-7777-6666-555555555555"},
          "media": [{"position": 1, "format": "CD", "track-count": 3, "tracks": [
            {"id": "aaaa1111-0000-0000-0000-000000000001", "position": 1, "number": "1",
             "title": "One", "length": 61000,
             "recording": {"id": "bbbb1111-0000-0000-0000-000000000001", "title": "One"}},
            {"id": "aaaa1111-0000-0000-0000-000000000002", "position": 2, "number": "2",
             "title": "Two", "length": 59000,
             "recording": {"id": "bbbb1111-0000-0000-0000-000000000002", "title": "Two"}},
            {"id": "aaaa1111-0000-0000-0000-000000000003", "position": 3, "number": "3",
             "title": "Three", "length": 75000,
             "recording": {"id": "bbbb1111-0000-0000-0000-000000000003", "title": "Three"}}
          ]}]})json")
            .arg(QString::fromLatin1(id));
    };
    const MusicBrainzLookupService service{
        .fetch =
            [&](const QString& url, std::function<void(core::Result<QByteArray>)> completion) {
                QByteArray body;
                if (url.contains(QStringLiteral("?query="))) {
                    body = QStringLiteral(R"({"count": 1, "releases": [%1]})")
                               .arg(release(first))
                               .toUtf8();
                } else {
                    body =
                        release(url.contains(QString::fromLatin1(stray)) ? stray : first).toUtf8();
                }
                QTimer::singleShot(
                    1, [body, completion = std::move(completion)] { completion(body); });
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
    auto* open =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-identify-albums"));
    QTRY_VERIFY(open->isEnabled());
    open->click();
    auto* dialog =
        properties->findChild<IdentifyAlbumsDialog*>(QStringLiteral("bench-identify-albums"));
    auto* session = dialog->session();
    // Grouped apart by its release id.
    QCOMPARE(session->albums().size(), 2U);
    QCOMPARE(session->albums()[0].items.size(), 2U);
    QCOMPARE(session->albums()[1].items.size(), 1U);
    // Two files for three tracks: the review opens on the first album.
    auto* heading =
        dialog->findChild<QLabel*>(QStringLiteral("bench-identify-albums-review-heading"));
    QTRY_VERIFY_WITH_TIMEOUT(heading->isVisible(), 5'000);
    TrackMatchSession* match = nullptr;
    QTRY_VERIFY((match = dialog->findChild<TrackMatchSession*>()) != nullptr && match->ready());
    // Its own files on their tracks, a gap for the third, and the other
    // album's file offered below -- not placed, not counted.
    auto* rows = dialog->findChild<QTreeWidget*>(QStringLiteral("bench-musicbrainz-match-files"));
    auto* tracks =
        dialog->findChild<QTreeWidget*>(QStringLiteral("bench-musicbrainz-match-tracks"));
    QTRY_COMPARE(rows->topLevelItemCount(), 4);
    QCOMPARE(match->fileCount(), 2U);
    QCOMPARE(match->pairedCount(), 2U);
    QVERIFY(match->rows()[2].gap);
    QVERIFY(match->isGap(2U));
    QVERIFY(match->rows()[3].other);
    QCOMPARE(rows->topLevelItem(3)->text(0), QStringLiteral("03.flac"));
    QVERIFY(tracks->topLevelItem(3)->text(0).contains(QStringLiteral("Stays in")));
    QVERIFY(rows->topLevelItem(3)->font(0).italic());
    // Dragged onto track 3: the gap filled, nothing else moved.
    {
        std::unique_ptr<QMimeData> mime{rows->model()->mimeData({rows->model()->index(3, 0)})};
        QVERIFY(mime != nullptr);
        const auto at = tracks->visualItemRect(tracks->topLevelItem(2)).center();
        QDragEnterEvent enter{dragPosition(at), Qt::MoveAction, mime.get(), Qt::LeftButton,
                              Qt::NoModifier};
        QCoreApplication::sendEvent(tracks->viewport(), &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop{QPointF{at}, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier};
        QCoreApplication::sendEvent(tracks->viewport(), &drop);
        QVERIFY(drop.isAccepted());
    }
    QCOMPARE(rows->topLevelItemCount(), 3);
    QCOMPARE(rows->topLevelItem(0)->text(0), QStringLiteral("01.flac"));
    QCOMPARE(rows->topLevelItem(1)->text(0), QStringLiteral("02.flac"));
    QCOMPARE(rows->topLevelItem(2)->text(0), QStringLiteral("03.flac"));
    QCOMPARE(match->pairedCount(), 3U);
    QVERIFY(match->status().contains(QStringLiteral("1 taken from other albums")));
    // Onto a track with a file: the two change places.
    match->placeOn(0U, 1U);
    QCOMPARE(rows->topLevelItem(0)->text(0), QStringLiteral("02.flac"));
    QCOMPARE(rows->topLevelItem(1)->text(0), QStringLiteral("01.flac"));
    match->placeOn(0U, 1U);
    QCOMPARE(rows->topLevelItem(0)->text(0), QStringLiteral("01.flac"));
    // Accepted: the album has all three; the one it came from is gone.
    auto* accept =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-identify-albums-review-accept"));
    QTRY_VERIFY(accept->isEnabled());
    accept->click();
    QTRY_COMPARE_WITH_TIMEOUT(session->count(AlbumBatchSession::State::staged), std::size_t{1U},
                              5'000);
    QCOMPARE(session->albums()[0].items, (std::vector<std::size_t>{0U, 1U, 2U}));
    QCOMPARE(session->albums()[1].state, AlbumBatchSession::State::merged);
    QCOMPARE(session->order(), (std::vector<std::size_t>{0U}));
    // The stray's release id is the album's now, staged in the draft.
    bool stray_staged = false;
    for (const auto& patch : grid_model->patches().patches()) {
        if (patch.item_index == 2U) {
            stray_staged = true;
        }
    }
    QVERIFY(stray_staged);
    delete properties;
}

void BenchMainWindowTest::musicBrainzIdentifyStagesChosenVersion_data() {
    QTest::addColumn<bool>("untagged");
    QTest::newRow("tagged") << false;
    QTest::newRow("untagged-manual-mapping") << true;
}

void BenchMainWindowTest::musicBrainzIdentifyStagesChosenVersion() {
    QFETCH(bool, untagged);
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
    auto* use = dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-use"));
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
    QCOMPARE(results->topLevelItem(0)->text(1), QStringLiteral("Alpha"));
    QCOMPARE(results->topLevelItem(0)->text(2), QStringLiteral("Band"));
    QVERIFY(results->topLevelItem(0)->text(5).contains(QStringLiteral("1999-09-09")));
    QTRY_VERIFY(use->isEnabled());
    QTest::mouseClick(use, Qt::LeftButton);
    auto* stage_matches =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-stage"));
    QVERIFY(stage_matches);
    if (untagged) {
        auto* pairs =
            dialog->findChild<QTreeWidget*>(QStringLiteral("bench-musicbrainz-match-files"));
        auto* up = dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-up"));
        auto* down =
            dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-down"));
        auto* unmatch =
            dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-unmatch"));
        auto* sort =
            dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-sort"));
        QVERIFY(pairs && up && down && unmatch && sort);
        auto* tracks =
            dialog->findChild<QTreeWidget*>(QStringLiteral("bench-musicbrainz-match-tracks"));
        QVERIFY(tracks);
        QTRY_COMPARE(pairs->topLevelItemCount(), 2);
        sort->click();
        QVERIFY(pairs->topLevelItem(0)->text(0).endsWith(QStringLiteral("2-first.flac")));
        const auto first_album_track = tracks->topLevelItem(0)->text(0);
        const auto second_album_track = tracks->topLevelItem(1)->text(0);
        QVERIFY(first_album_track.contains(QStringLiteral("One")));
        QVERIFY(second_album_track.contains(QStringLiteral("Two")));
        pairs->setCurrentItem(pairs->topLevelItem(0));
        QVERIFY(!up->isEnabled());
        QVERIFY(down->isEnabled());
        QCOMPARE(tracks->currentItem(), tracks->topLevelItem(0));
        QCOMPARE(pairs->topLevelItem(0)->text(2), QStringLiteral("→ 1.1"));
        QVERIFY(pairs->topLevelItem(0)->font(2).bold());
        QCOMPARE(pairs->topLevelItem(0)->background(0), tracks->topLevelItem(0)->background(0));
        QCOMPARE(pairs->topLevelItem(0)->toolTip(0), QStringLiteral("/music/2-first.flac"));
        // Exercise the actual drop handler using this view's model-generated payload.
        std::unique_ptr<QMimeData> mime{pairs->model()->mimeData({pairs->model()->index(0, 0)})};
        const auto rect = pairs->visualItemRect(pairs->topLevelItem(1));
        const QPoint position{rect.center().x(), rect.bottom() - 1};
        QDragEnterEvent enter{dragPosition(position), Qt::MoveAction, mime.get(), Qt::LeftButton,
                              Qt::NoModifier};
        QApplication::sendEvent(pairs->viewport(), &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop{QPointF{position}, Qt::MoveAction, mime.get(), Qt::LeftButton,
                        Qt::NoModifier};
        QApplication::sendEvent(pairs->viewport(), &drop);
        QVERIFY(drop.isAccepted());
        QDragEnterEvent stale{dragPosition(position), Qt::MoveAction, mime.get(), Qt::LeftButton,
                              Qt::NoModifier};
        QApplication::sendEvent(pairs->viewport(), &stale);
        QVERIFY(!stale.isAccepted());
        QVERIFY(pairs->topLevelItem(1)->text(0).endsWith(QStringLiteral("2-first.flac")));
        QCOMPARE(tracks->topLevelItem(0)->text(0), first_album_track);
        QCOMPARE(tracks->topLevelItem(1)->text(0), second_album_track);
        QVERIFY(!down->isEnabled());
        pairs->setFocus();
        QTest::keyClick(pairs, Qt::Key_Up, Qt::AltModifier);
        QVERIFY(pairs->topLevelItem(0)->text(0).endsWith(QStringLiteral("2-first.flac")));
        unmatch->click();
        QCOMPARE(pairs->topLevelItemCount(), 3);
        QCOMPARE(pairs->topLevelItem(0)->text(0), QStringLiteral("No local file"));
        QVERIFY(pairs->topLevelItem(2)->text(2).contains(QStringLiteral("Unmatched")));
        QVERIFY(!unmatch->isEnabled());
        up->click();
        up->click();
        QCOMPARE(pairs->topLevelItemCount(), 3);
        // The missing slot can move too, returning the extra file to the album.
        pairs->setCurrentItem(pairs->topLevelItem(2));
        up->click();
        QCOMPARE(pairs->topLevelItemCount(), 2);
        QVERIFY(pairs->topLevelItem(0)->text(0).endsWith(QStringLiteral("2-first.flac")));
        QCOMPARE(tracks->topLevelItem(0)->text(0), first_album_track);
        QCOMPARE(tracks->topLevelItem(1)->text(0), second_album_track);
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
    QCOMPARE(fetches, 2);
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
    auto* use = dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-use"));
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
    QCOMPARE(results->topLevelItem(0)->text(0), QStringLiteral("2/2 files"));
    QCOMPARE(results->topLevelItem(0)->text(1), QStringLiteral("Alpha"));

    QTRY_VERIFY(use->isEnabled());
    QTest::mouseClick(use, Qt::LeftButton);
    auto* stage_matches =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-stage"));
    QVERIFY(stage_matches);
    QTRY_VERIFY(stage_matches->isEnabled());
    QTest::mouseClick(stage_matches, Qt::LeftButton);
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

void BenchMainWindowTest::upNextPanelAnimationAndSettings() {
    QSettings{}.setValue(QStringLiteral("appearance/panel-animations"), false);
    QSettings{}.setValue(QStringLiteral("up-next/visible"), false);
    QSettings{}.setValue(QStringLiteral("up-next/width"), 380);
    BenchMainWindow window;
    window.resize(1320, 760);
    window.show();
    QTRY_VERIFY(window.up_next_restored_);
    auto* dock = window.up_next_dock_;
    auto* animation = dock->findChild<QVariantAnimation*>();
    QVERIFY(animation);
    auto* toggle = window.findChild<QAction*>(QStringLiteral("action-show-up-next"));
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
    QVERIFY(window.up_next_view_->isVisible());
    window.resizeDocks({dock}, {460}, Qt::Horizontal);
    QTRY_COMPARE(QSettings{}.value(QStringLiteral("up-next/width")).toInt(), 460);
    toggle->trigger();
    QTRY_VERIFY(!dock->isVisible());
    QCOMPARE(QSettings{}.value(QStringLiteral("up-next/width")).toInt(), 460);
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
        window.up_next_dock_->setVisible(true);
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
