// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"
#include "bench/dynamic_playlist_dialog.hpp"
#include "bench/dynamic_playlist_service.hpp"
#include "bench/local_library_panel.hpp"
#include "bench/search_dialog.hpp"
#include "trackknife/core/sha256.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "trackknife/persistence/local_library.hpp"
#include "trackknife/persistence/rating_identity.hpp"
#include "ui/server_library_tree_view.hpp"
#include "uicommon/local_artwork.hpp"
#include "uicommon/local_files_mime_data.hpp"
#include "uicommon/queue_table_view.hpp"

#include <QAction>
#include <QBuffer>
#include <QCheckBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QtTest>

#include <sqlite3.h>
#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#include <taglib/tpropertymap.h>

#include <filesystem>
#include <fstream>

namespace trackknife::bench {
namespace {

std::string fixture(const std::filesystem::path& root, const std::string& name,
                    const std::string& title = "First song",
                    const std::string& album = "Test album",
                    const std::string& track_number = "3") {
    std::filesystem::create_directories(root);
    QFile encoded{QStringLiteral(TRACKKNIFE_AUDIO_FIXTURE_DIR "/tagged-tone-flac.b64")};
    if (!encoded.open(QIODevice::ReadOnly)) {
        return {};
    }
    const auto data = QByteArray::fromBase64(encoded.readAll());
    const auto path = (root / name).native();
    {
        std::ofstream output{std::filesystem::path{path}, std::ios::binary};
        output.write(data.data(), data.size());
    }
    TagLib::FLAC::File file{path.c_str()};
    auto properties = file.properties();
    properties.replace("TITLE", TagLib::String{title, TagLib::String::UTF8});
    properties.replace("ALBUM", TagLib::String{album, TagLib::String::UTF8});
    properties.replace("ARTIST", TagLib::String{"Björk", TagLib::String::UTF8});
    properties.replace("ALBUMARTIST", TagLib::String{"Björk", TagLib::String::UTF8});
    if (track_number.empty()) {
        properties.erase("TRACKNUMBER");
    } else {
        properties.replace("TRACKNUMBER", TagLib::String{track_number, TagLib::String::UTF8});
    }
    file.setProperties(properties);
    if (!file.save()) {
        return {};
    }
    return path;
}

persistence::LibraryQuery tracks(std::string text = {}) {
    persistence::LibraryQuery query;
    query.kind = persistence::LibraryEntryKind::track;
    query.text = std::move(text);
    return query;
}

QMenu* libraryMenu(LocalLibraryPanel* panel, const QModelIndex& index) {
    auto* tree = panel->findChild<QTreeView*>();
    tree->scrollTo(index);
    QMetaObject::invokeMethod(tree, "customContextMenuRequested", Qt::DirectConnection,
                              Q_ARG(QPoint, tree->visualRect(index).center()));
    return panel->findChild<QMenu*>(QStringLiteral("local-library-context-menu"));
}

bool triggerLibraryAction(LocalLibraryPanel* panel, const QModelIndex& index, int action) {
    auto* menu = libraryMenu(panel, index);
    if (!menu) {
        return false;
    }
    auto* command =
        menu->findChild<QAction*>(QStringLiteral("action-local-library-%1").arg(action));
    if (!command || !command->isEnabled()) {
        menu->close();
        return false;
    }
    command->trigger();
    menu->close();
    return true;
}

bool dropFiles(QTableView* view, const QMimeData* mime, const QPoint& position) {
    QDragEnterEvent enter{position, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(view->viewport(), &enter);
    if (!enter.isAccepted()) {
        return false;
    }
    QDropEvent drop{QPointF{position}, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(view->viewport(), &drop);
    return drop.isAccepted();
}

} // namespace

class LocalLibraryTest final : public QObject {
    Q_OBJECT
  private slots:
    void rootsRetainOfflineMusicAndRawPaths();
    void incrementalScanSearchAndPaging();
    void deletedSubfoldersArePrunedOnlyAfterCompleteScans();
    void missingFilesOnAnotherDeviceAreRetained();
    void deletionCleanupPagesWithoutChangingWorkingLists();
    void albumIdentityKeepsEditionsSeparate();
    void metadataAndMovesFollowTheListTransaction();
    void scanRetainsFieldRowsAndTechnicals();
    void denseMetadataDoesNotProduceFalseMissingMatches();
    void parallelScanIndexesManyFiles();
    void ratingsFollowContentIdentity();
    void migrationRoundTrip();
    void scansOnlyOnRefresh_data();
    void scansOnlyOnRefresh();
    void queryModeFiltersAndCommitsResults();
    void databaseSearchOpensCachedRowsWithoutFiles();
    void dynamicRulesFollowIndexedTagsAndKeepRawPaths();
    void locateLoadsAdditionalTreePages();
    void cachedSearchTabsLoadCovers_data();
    void cachedSearchTabsLoadCovers();
    void localViewBrowsesSearchesAndOpensFiles();
    void dragResolvesUnloadedPagesAndRawPaths();
    void trackNumbersAppearInTreeAndSearch();
    void albumCoversLoadAndRefresh();
};

void LocalLibraryTest::rootsRetainOfflineMusicAndRawPaths() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    const auto path = fixture(root, "raw-\xff.flac");
    QVERIFY(!path.empty());
    auto library = persistence::LocalLibrary::open(base / "state.sqlite");
    QVERIFY(library);
    QVERIFY(library->roots()->empty());
    QVERIFY(library->add_root(root.native()));
    QVERIFY(!library->add_root(root.native()));
    QVERIFY(!library->add_root(base.native()));
    QVERIFY(!library->add_root("relative"));
    persistence::LibraryScanProgress progress;
    QVERIFY(library->scan({}, progress));
    QCOMPARE(progress.indexed.load(), 1U);
    QCOMPARE(library->paths(tracks())->front(), path);
    QCOMPARE(library->query(tracks())->entries.front().available, 1U);
    std::filesystem::rename(root, base / "unplugged");
    persistence::LibraryScanProgress offline;
    QVERIFY(library->scan({}, offline));
    QVERIFY(!library->roots()->front().available);
    QCOMPARE(library->query(tracks())->entries.size(), 1U);
    QCOMPARE(library->query(tracks())->entries.front().available, 0U);
    QVERIFY(library->paths(tracks())->empty());
    std::filesystem::rename(base / "unplugged", root);
    persistence::LibraryScanProgress online;
    QVERIFY(library->scan({}, online));
    QCOMPARE(online.indexed.load(), 0U);
    QCOMPARE(library->query(tracks())->entries.front().available, 1U);
    QVERIFY(library->remove_root(root.native()));
    QVERIFY(library->query(tracks())->entries.empty());
    QVERIFY(std::filesystem::exists(std::filesystem::path{path}));
    // Qt's temporary-directory cleanup cannot round-trip a non-UTF-8 name.
    QVERIFY(std::filesystem::remove(std::filesystem::path{path}));
}

void LocalLibraryTest::incrementalScanSearchAndPaging() {
    QTemporaryDir temporary;
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    const auto first = fixture(root, "01.flac", "Needle in a song");
    const auto second = fixture(root, "02.flac", "Other song");
    auto library = persistence::LocalLibrary::open(base / "state.sqlite");
    QVERIFY(library && library->add_root(root.native()));
    persistence::LibraryScanProgress first_scan;
    QVERIFY(library->scan({}, first_scan));
    QCOMPARE(first_scan.indexed.load(), 2U);
    QCOMPARE(library->query({})->entries.front().albums, 1U);
    persistence::LibraryScanProgress repeat;
    QVERIFY(library->scan({}, repeat));
    QCOMPARE(repeat.indexed.load(), 0U);
    QCOMPARE(library->query(tracks("BJÖRK needle"))->entries.size(), 1U);
    QVERIFY(library->query(tracks("needle absent"))->entries.empty());
    QVERIFY(library->query(tracks("%' OR 1=1 --"))->entries.empty());
    persistence::LibraryQuery albums;
    albums.kind = persistence::LibraryEntryKind::album;
    albums.text = "needle";
    QVERIFY(library->query(albums)->entries.empty());
    albums.text = "test björk";
    QCOMPARE(library->query(albums)->entries.size(), 1U);
    QCOMPARE(library->query(albums)->entries.front().tracks, 2U);
    auto page = tracks();
    page.limit = 1;
    const auto one = library->query(page);
    QVERIFY(one && one->more);
    page.offset = 1;
    const auto two = library->query(page);
    QVERIFY(two && !two->more);
    QVERIFY(one->entries.front().key != two->entries.front().key);
    std::filesystem::remove(std::filesystem::path{second});
    core::CancellationSource cancel;
    cancel.request_cancellation();
    persistence::LibraryScanProgress stopped;
    QVERIFY(library->scan(cancel.token(), stopped)->cancelled);
    QCOMPARE(library->paths(tracks())->size(), 2U);
    persistence::LibraryScanProgress missing;
    QVERIFY(library->scan({}, missing));
    QCOMPARE(library->paths(tracks())->size(), 1U);
    QCOMPARE(library->query(tracks())->entries.size(), 1U);
    QCOMPARE(fixture(root, "01.flac", "Changed title"), first);
    persistence::LibraryScanProgress changed;
    QVERIFY(library->scan({}, changed));
    QCOMPARE(changed.indexed.load(), 1U);
    QCOMPARE(library->query(tracks("Changed"))->entries.size(), 1U);
    std::filesystem::create_symlink(std::filesystem::path{first}, root / "alias.flac");
    persistence::LibraryScanProgress symlink;
    QVERIFY(library->scan({}, symlink));
    QCOMPARE(library->paths(tracks())->size(), 1U);
}

void LocalLibraryTest::deletedSubfoldersArePrunedOnlyAfterCompleteScans() {
    QTemporaryDir temporary;
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    const auto removed = fixture(root / "removed", "01.flac", "Deleted", "Deleted album");
    const auto retained = fixture(root / "retained", "01.flac", "Kept", "Kept album");
    QVERIFY(!removed.empty() && !retained.empty());
    auto library = persistence::LocalLibrary::open(base / "state.sqlite");
    QVERIFY(library && library->add_root(root.native()));
    persistence::LibraryScanProgress initial;
    QVERIFY(library->scan({}, initial));
    QCOMPARE(library->query(tracks())->entries.size(), 2U);
    QCOMPARE(library->query({})->entries.front().albums, 2U);
    std::filesystem::remove_all(root / "removed");
    // A failed probe makes the traversal incomplete: retain unseen entries.
    {
        std::ofstream corrupt{root / "broken.flac"};
        corrupt << "not audio";
    }
    persistence::LibraryScanProgress partial;
    const auto incomplete = library->scan({}, partial);
    QVERIFY(incomplete && incomplete->incomplete);
    QCOMPARE(library->query(tracks())->entries.size(), 2U);
    std::filesystem::remove(root / "broken.flac");
    core::CancellationSource cancel;
    cancel.request_cancellation();
    persistence::LibraryScanProgress stopped;
    QVERIFY(library->scan(cancel.token(), stopped)->cancelled);
    QCOMPARE(library->query(tracks())->entries.size(), 2U);
    persistence::LibraryScanProgress complete;
    const auto scanned = library->scan({}, complete);
    QVERIFY(scanned && !scanned->incomplete);
    QVERIFY(library->query(tracks("Deleted"))->entries.empty());
    QCOMPARE(library->paths(tracks())->front(), retained);
    persistence::LibraryQuery albums;
    albums.kind = persistence::LibraryEntryKind::album;
    QCOMPARE(library->query(albums)->entries.size(), 1U);
    // Offline roots retain their cached entries and recover on reconnection.
    std::filesystem::rename(root, base / "offline");
    persistence::LibraryScanProgress offline;
    QVERIFY(library->scan({}, offline));
    QCOMPARE(library->query(tracks())->entries.front().available, 0U);
    std::filesystem::rename(base / "offline", root);
    persistence::LibraryScanProgress online;
    QVERIFY(library->scan({}, online));
    QCOMPARE(library->paths(tracks())->front(), retained);
    // Removing the last album also removes empty artist/album groups.
    std::filesystem::remove_all(root / "retained");
    persistence::LibraryScanProgress empty;
    QVERIFY(library->scan({}, empty));
    QVERIFY(library->query(tracks())->entries.empty());
    QVERIFY(library->query(albums)->entries.empty());
    QVERIFY(library->query({})->entries.empty());
    QVERIFY(std::filesystem::is_directory(root));
}

void LocalLibraryTest::missingFilesOnAnotherDeviceAreRetained() {
    QTemporaryDir temporary;
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    const auto source = fixture(root / "album", "01.flac");
    auto library = persistence::LocalLibrary::open(base / "state.sqlite");
    QVERIFY(library && library->add_root(root.native()));
    persistence::LibraryScanProgress initial;
    QVERIFY(library->scan({}, initial));
    // Model the device evidence left by an offline mount, without requiring
    // privileged mount operations. The existing mountpoint stays accessible.
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open((base / "state.sqlite").c_str(), &db), SQLITE_OK);
    QCOMPARE(sqlite3_exec(db,
                          "UPDATE local_library_tracks SET revision="
                          "'18446744073709551615' || substr(revision,instr(revision,':'))",
                          nullptr, nullptr, nullptr),
             SQLITE_OK);
    sqlite3_close(db);
    std::filesystem::rename(root / "album", base / "offline-album");
    persistence::LibraryScanProgress missing;
    QVERIFY(library->scan({}, missing));
    QCOMPARE(library->query(tracks())->entries.size(), 1U);
    QCOMPARE(library->query(tracks())->entries.front().available, 0U);
    QVERIFY(library->paths(tracks())->empty());
    // Returning files are revalidated and restore availability.
    std::filesystem::rename(base / "offline-album", root / "album");
    persistence::LibraryScanProgress returned;
    QVERIFY(library->scan({}, returned));
    QCOMPARE(library->paths(tracks())->front(), source);
}

void LocalLibraryTest::deletionCleanupPagesWithoutChangingWorkingLists() {
    QTemporaryDir temporary;
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    const auto source = fixture(root, "raw-\xff.flac");
    QVERIFY(!source.empty());
    for (int row = 0; row < 205; ++row) {
        std::filesystem::copy_file(std::filesystem::path{source},
                                   root / (std::to_string(row) + ".flac"));
    }
    auto repository = persistence::ListRepository::open(base / "state.sqlite");
    QVERIFY(repository);
    persistence::ListDocument list{.id = core::StableId::random(),
                                   .kind = persistence::ListKind::scratch,
                                   .name = "Local Queue",
                                   .pinned = false,
                                   .dirty = false,
                                   .items = {}};
    persistence::ListItem item;
    item.source = persistence::ListSource::local;
    item.source_reference = source;
    list.items = {item, item};
    QVERIFY(repository->replace_all(std::vector{list}));
    const auto saved = repository->load_all();
    QVERIFY(saved);
    auto library = persistence::LocalLibrary::open(base / "state.sqlite");
    QVERIFY(library && library->add_root(root.native()));
    persistence::LibraryScanProgress initial;
    QVERIFY(library->scan({}, initial));
    QCOMPARE(library->paths(tracks())->size(), 206U);
    // Keep the same root directory, as on a mounted collection with all albums deleted.
    for (const auto& entry : std::filesystem::directory_iterator{root}) {
        std::filesystem::remove(entry.path());
    }
    persistence::LibraryScanProgress clean;
    QVERIFY(library->scan({}, clean));
    QVERIFY(library->query(tracks())->entries.empty());
    QCOMPARE(repository->load_all(), saved);
}

void LocalLibraryTest::albumIdentityKeepsEditionsSeparate() {
    QTemporaryDir temporary;
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    const auto first = fixture(root / "edition-a", "01.flac");
    const auto second = fixture(root / "edition-b", "01.flac");
    auto library = persistence::LocalLibrary::open(base / "state.sqlite");
    QVERIFY(library && library->add_root(root.native()));
    persistence::LibraryScanProgress progress;
    QVERIFY(library->scan({}, progress));
    persistence::LibraryQuery albums;
    albums.kind = persistence::LibraryEntryKind::album;
    QCOMPARE(library->query(albums)->entries.size(), 2U);
    for (const auto& path : {first, second}) {
        TagLib::FLAC::File file{path.c_str()};
        auto properties = file.properties();
        properties.replace("MUSICBRAINZ_ALBUMID",
                           TagLib::String{"9e181c7e-6df1-4cb0-82ac-77d2be9a3c70"});
        file.setProperties(properties);
        QVERIFY(file.save());
    }
    persistence::LibraryScanProgress refresh;
    QVERIFY(library->scan({}, refresh));
    const auto grouped = library->query(albums);
    QVERIFY(grouped);
    QCOMPARE(grouped->entries.size(), 1U);
    QCOMPARE(grouped->entries.front().tracks, 2U);
}

void LocalLibraryTest::metadataAndMovesFollowTheListTransaction() {
    QTemporaryDir temporary;
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    const auto source = fixture(root, "01.flac");
    auto library = persistence::LocalLibrary::open(base / "state.sqlite");
    QVERIFY(library && library->add_root(root.native()));
    persistence::LibraryScanProgress progress;
    QVERIFY(library->scan({}, progress));
    auto repository = persistence::ListRepository::open(base / "state.sqlite");
    QVERIFY(repository);
    auto previous = core::observe_local_source_revision(source);
    QVERIFY(previous);
    persistence::ListDocument list{.id = core::StableId::random(),
                                   .kind = persistence::ListKind::scratch,
                                   .name = "Local Queue",
                                   .pinned = false,
                                   .dirty = false,
                                   .items = {}};
    persistence::ListItem item;
    item.source = persistence::ListSource::local;
    item.source_reference = source;
    item.source_revision = *previous;
    list.items.push_back(item);
    QVERIFY(repository->replace_all(std::vector{list}));
    QCOMPARE(fixture(root, "01.flac", "New title", "New album"), source);
    const auto read = metadata::read_local_metadata(source);
    QVERIFY(read);
    const persistence::LocalMetadataRefresh refresh{.operation_id = core::StableId::random(),
                                                    .source_reference = source,
                                                    .previous_revision = *previous,
                                                    .published_revision = read->source_revision,
                                                    .document = read->document};
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open((base / "state.sqlite").c_str(), &db), SQLITE_OK);
    QCOMPARE(
        sqlite3_exec(db,
                     "CREATE TRIGGER reject_library_refresh BEFORE UPDATE ON local_library_tracks "
                     "BEGIN SELECT RAISE(ABORT,'injected index failure'); END",
                     nullptr, nullptr, nullptr),
        SQLITE_OK);
    QVERIFY(!repository->refresh_local_metadata(refresh));
    QCOMPARE(repository->load_all()->front().items.front().source_revision,
             std::optional{*previous});
    QCOMPARE(library->query(tracks("First song"))->entries.size(), 1U);
    QVERIFY(library->query(tracks("New title"))->entries.empty());
    QCOMPARE(sqlite3_exec(db, "DROP TRIGGER reject_library_refresh", nullptr, nullptr, nullptr),
             SQLITE_OK);
    sqlite3_close(db);
    QVERIFY(repository->refresh_local_metadata(refresh));
    QCOMPARE(library->query(tracks("New title"))->entries.size(), 1U);
    QVERIFY(library->query(tracks("First song"))->entries.empty());
    QVERIFY(repository->refresh_local_metadata(refresh)->already_applied);
    const auto target = (root / "renamed.flac").native();
    std::filesystem::rename(std::filesystem::path{source}, std::filesystem::path{target});
    persistence::LocalSourceRelocation relocation{.operation_id = core::StableId::random(),
                                                  .source_reference = source,
                                                  .target_reference = target,
                                                  .previous_revision = read->source_revision,
                                                  .published_revision =
                                                      *core::observe_local_source_revision(target),
                                                  .published_document = std::nullopt};
    QVERIFY(repository->relocate_local_source(relocation));
    QCOMPARE(library->paths(tracks())->front(), target);
    QCOMPARE(repository->load_all()->front().items.front().source_reference, target);
    QVERIFY(repository->relocate_local_source(relocation)->already_applied);
    const auto outside = (base / "outside.flac").native();
    std::filesystem::rename(std::filesystem::path{target}, std::filesystem::path{outside});
    relocation.operation_id = core::StableId::random();
    relocation.source_reference = target;
    relocation.target_reference = outside;
    QVERIFY(repository->relocate_local_source(relocation));
    QVERIFY(library->query(tracks())->entries.empty());
    QCOMPARE(repository->load_all()->front().items.front().source_reference, outside);
}

// ADR-0150: the scan retains every bounded tag value in the field table
// (original bytes beside the normalized form) and the probed technical
// properties as typed columns; the reindex-once revision prefix backfills
// rows written before migration 30 on the next Refresh.
void LocalLibraryTest::denseMetadataDoesNotProduceFalseMissingMatches() {
    QTemporaryDir temporary;
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    const auto tagged = fixture(root, "tagged.flac");
    const auto missing = fixture(root, "missing.flac");
    {
        TagLib::FLAC::File file{tagged.c_str()};
        auto properties = file.properties();
        for (int index = 0; index < 90; ++index) {
            properties.replace(TagLib::String{"A" + std::to_string(index)},
                               TagLib::String{"value"});
        }
        properties.replace("REPLAYGAIN_ALBUM_GAIN", TagLib::String{"-9.25 dB"});
        TagLib::StringList genres;
        for (int index = 0; index < 40; ++index)
            genres.append(TagLib::String{"Genre" + std::to_string(index)});
        properties.replace("GENRE", genres);
        properties.replace("ZZLONG", TagLib::String{std::string(4096, 'x')});
        file.setProperties(properties);
        QVERIFY(file.save());
    }
    {
        TagLib::FLAC::File file{missing.c_str()};
        auto properties = file.properties();
        properties.erase("REPLAYGAIN_ALBUM_GAIN");
        file.setProperties(properties);
        QVERIFY(file.save());
    }
    const auto database = base / "state.sqlite";
    auto library = persistence::LocalLibrary::open(database);
    QVERIFY(library && library->add_root(root.native()));
    persistence::LibraryScanProgress progress;
    QVERIFY(library->scan({}, progress));
    auto query = query::compile_tkq("REPLAYGAIN_ALBUM_GAIN MISSING");
    QVERIFY(query);
    QCOMPARE(*library->filter_paths(*query), std::vector<std::string>{missing});
    auto last_genre = query::compile_tkq("genre IS Genre39");
    QVERIFY(last_genre);
    QCOMPARE(*library->filter_paths(*last_genre), std::vector<std::string>{tagged});
    auto long_value = query::compile_tkq("ZZLONG PRESENT");
    QVERIFY(long_value);
    QCOMPARE(*library->filter_paths(*long_value), std::vector<std::string>{tagged});

    // Downgrade/upgrade simulates a cache whose former per-field truncation
    // cannot be distinguished from actual absence. No automatic scan repairs it.
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(database.c_str(), &db), SQLITE_OK);
    // Migrations unwind strictly in reverse order down to the truncation-era
    // schema before the app re-migrates forward.
    for (const auto* name :
         {"0036_server_search_scope.down", "0035_local_ratings.down",
          "0034_metadata_field_filters.down", "0033_complete_library_fields.down"}) {
        QFile downgrade{
            QStringLiteral(TRACKKNIFE_MIGRATION_DIR "/%1.sql").arg(QString::fromLatin1(name))};
        QVERIFY(downgrade.open(QIODevice::ReadOnly));
        QCOMPARE(sqlite3_exec(db, downgrade.readAll().constData(), nullptr, nullptr, nullptr),
                 SQLITE_OK);
    }
    sqlite3_close(db);
    auto migrated = persistence::LocalLibrary::open(database);
    QVERIFY(migrated);
    const auto incomplete = migrated->filter_paths(*query);
    QVERIFY(!incomplete && incomplete.error().code == core::ErrorCode::conflict);
    QVERIFY(incomplete.error().message.find("Refresh") != std::string::npos);
    QVERIFY(!migrated->filter(*query, 0, 200));
    persistence::LibraryScanProgress refresh;
    QVERIFY(migrated->scan({}, refresh));
    QCOMPARE(refresh.indexed.load(), 2U);
    QCOMPARE(*migrated->filter_paths(*query), std::vector<std::string>{missing});
    persistence::LibraryScanProgress unchanged;
    QVERIFY(migrated->scan({}, unchanged));
    QCOMPARE(unchanged.indexed.load(), 0U);
}

void LocalLibraryTest::scanRetainsFieldRowsAndTechnicals() {
    QTemporaryDir temporary;
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    const auto path = fixture(root, "01.flac", "Substrate song");
    QVERIFY(!path.empty());
    {
        TagLib::FLAC::File file{path.c_str()};
        auto properties = file.properties();
        properties.replace("GENRE", TagLib::String{"Jazz", TagLib::String::UTF8});
        properties.insert("GENRE", TagLib::String{"BEBOP", TagLib::String::UTF8});
        properties.replace("REPLAYGAIN_TRACK_GAIN", TagLib::String{"-6.02 dB"});
        file.setProperties(properties);
        QVERIFY(file.save());
    }
    auto library = persistence::LocalLibrary::open(base / "state.sqlite");
    QVERIFY(library);
    QVERIFY(library->add_root(root.native()).has_value());
    persistence::LibraryScanProgress progress;
    QVERIFY(library->scan({}, progress).has_value());
    QCOMPARE(progress.indexed.load(), 1U);

    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open((base / "state.sqlite").c_str(), &db), SQLITE_OK);
    // Captured by reference: the backfill section below closes and
    // reopens the connection.
    const auto rows = [&db](const char* sql) {
        sqlite3_stmt* statement = nullptr;
        QList<QByteArray> values;
        if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK) {
            while (sqlite3_step(statement) == SQLITE_ROW) {
                values.push_back(
                    QByteArray{static_cast<const char*>(sqlite3_column_blob(statement, 0)),
                               sqlite3_column_bytes(statement, 0)});
            }
        }
        sqlite3_finalize(statement);
        return values;
    };
    // Multi-value field rows keep demuxer order and original spelling.
    QCOMPARE(rows("SELECT value FROM local_library_fields WHERE canonical_name='genre' "
                  "ORDER BY position"),
             (QList<QByteArray>{"Jazz", "BEBOP"}));
    QCOMPARE(rows("SELECT value_lower FROM local_library_fields WHERE canonical_name='genre' "
                  "ORDER BY position"),
             (QList<QByteArray>{"jazz", "bebop"}));
    QCOMPARE(rows("SELECT value FROM local_library_fields "
                  "WHERE canonical_name='replaygaintrackgain'"),
             (QList<QByteArray>{"-6.02 dB"}));
    // Technical columns come from the probe the scan already runs.
    QCOMPARE(rows("SELECT codec_name FROM local_library_tracks"), (QList<QByteArray>{"flac"}));
    QVERIFY(!rows("SELECT sample_rate FROM local_library_tracks WHERE sample_rate>0").isEmpty());
    QVERIFY(!rows("SELECT bits FROM local_library_tracks WHERE bits=16").isEmpty());
    QVERIFY(!rows("SELECT channels FROM local_library_tracks WHERE channels>0").isEmpty());
    QVERIFY(!rows("SELECT duration_ms FROM local_library_tracks WHERE duration_ms>0").isEmpty());

    // A pre-migration row (old revision format, no field rows) reindexes on
    // the next Refresh even though the file itself is unchanged.
    QCOMPARE(sqlite3_exec(db,
                          "UPDATE local_library_tracks SET revision=substr(revision,3);"
                          "DELETE FROM local_library_fields",
                          nullptr, nullptr, nullptr),
             SQLITE_OK);
    sqlite3_close(db);
    persistence::LibraryScanProgress backfill;
    QVERIFY(library->scan({}, backfill).has_value());
    QCOMPARE(backfill.indexed.load(), 1U);
    QCOMPARE(sqlite3_open((base / "state.sqlite").c_str(), &db), SQLITE_OK);
    QCOMPARE(rows("SELECT value FROM local_library_fields WHERE canonical_name='genre' "
                  "ORDER BY position"),
             (QList<QByteArray>{"Jazz", "BEBOP"}));
    sqlite3_close(db);
}

// ADR-0151: preparation runs on a bounded worker pool while the walk
// and every commit stay serial; ordering-free counters, incremental
// rescans, and deletion handling hold across the pipeline.
void LocalLibraryTest::parallelScanIndexesManyFiles() {
    QTemporaryDir temporary;
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    std::vector<std::string> paths;
    for (int index = 0; index < 24; ++index) {
        const auto name = QStringLiteral("%1.flac").arg(index, 2, 10, QLatin1Char('0'));
        const auto title = QStringLiteral("Track %1").arg(index);
        paths.push_back(fixture(root, name.toStdString(), title.toStdString(),
                                index % 2 == 0 ? "Even album" : "Odd album"));
        QVERIFY(!paths.back().empty());
    }
    auto library = persistence::LocalLibrary::open(base / "state.sqlite");
    QVERIFY(library && library->add_root(root.native()));
    persistence::LibraryScanProgress progress;
    const auto scanned = library->scan({}, progress);
    QVERIFY(scanned.has_value());
    QVERIFY(!scanned->incomplete);
    QCOMPARE(progress.indexed.load(), 24U);
    QCOMPARE(progress.failed.load(), 0U);
    QCOMPARE(library->paths(tracks())->size(), 24U);

    // Unchanged files skip the pipeline entirely on the next pass.
    persistence::LibraryScanProgress repeat;
    QVERIFY(library->scan({}, repeat).has_value());
    QCOMPARE(repeat.indexed.load(), 0U);

    // Deletions and retags keep working through the parallel path.
    std::error_code fs_error;
    for (int index = 0; index < 3; ++index) {
        QVERIFY(std::filesystem::remove(
            std::filesystem::path{paths[static_cast<std::size_t>(index)]}, fs_error));
    }
    QCOMPARE(fixture(root, "03.flac", "Retitled"), paths[3]);
    persistence::LibraryScanProgress changed;
    QVERIFY(library->scan({}, changed).has_value());
    QCOMPARE(changed.indexed.load(), 1U);
    QCOMPARE(library->paths(tracks())->size(), 21U);
    QCOMPARE(library->query(tracks("Retitled"))->entries.size(), 1U);
}

void LocalLibraryTest::ratingsFollowContentIdentity() {
    // ADR-0179: the identity hashes must stay byte-compatible with the Melody
    // server (sha256 over NUL-joined normalized fields, lowercase hex).
    QCOMPARE(persistence::track_rating_hash("Slayer", "Divine Intervention", "Killing Fields", 1),
             std::string{"14bc899daabc68458c8d37c3ea54321e9acff3e66fddde3de11f5fd5118a3572"});
    QCOMPARE(persistence::album_rating_hash("Slayer", "Divine Intervention", "1994"),
             std::string{"db4f9711c1c421acbce7ffb1e9f7ceb2a8e4d33f22912dc0c7419f74403c990e"});
    QCOMPARE(core::sha256_hex(""),
             std::string{"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"});
    QCOMPARE(core::sha256_hex("abc"),
             std::string{"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"});
    QCOMPARE(core::sha256_hex(std::string(56U, 'a')),
             std::string{"b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"});
    QCOMPARE(core::sha256_hex(std::string(200U, 'a')),
             std::string{"c2a908d98f5df987ade41b5fce213067efbcc21ef2240212a41e54b5e7c28ae5"});

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    const auto path = fixture(root, "rated.flac");
    QVERIFY(!path.empty());
    auto library = persistence::LocalLibrary::open(base / "state.sqlite");
    QVERIFY(library);
    QVERIFY(library->add_root(root.native()));
    persistence::LibraryScanProgress progress;
    QVERIFY(library->scan({}, progress));

    const auto track_hash = persistence::track_rating_hash("Björk", "Test album", "First song", 3);
    // The album identity carries the fixture's tagged year, matching
    // Melody's year-only date normalization.
    const auto album_hash = persistence::album_rating_hash("Björk", "Test album", "2026");
    auto page = library->query(tracks());
    QVERIFY(page);
    QCOMPARE(page->entries.front().rating_hash, track_hash);
    QCOMPARE(page->entries.front().rating, 0U);

    const auto document = metadata::read_local_metadata(path);
    QVERIFY(document);
    const auto identity = persistence::rating_identity(document->document, path);
    QCOMPARE(identity.track_hash, track_hash);
    QCOMPARE(identity.album_hash, album_hash);

    QVERIFY(library->set_rating(track_hash, false, 8U));
    QVERIFY(library->set_rating(album_hash, true, 6U));
    QVERIFY(!library->set_rating(track_hash, false, 11U));
    QVERIFY(!library->set_rating("not-a-hash", false, 5U));
    QCOMPARE(library->query(tracks())->entries.front().rating, 8U);
    persistence::LibraryQuery albums;
    albums.kind = persistence::LibraryEntryKind::album;
    QCOMPARE(library->query(albums)->entries.front().rating_hash, album_hash);
    QCOMPARE(library->query(albums)->entries.front().rating, 6U);
    QCOMPARE(*library->ratings({track_hash, album_hash, std::string(64U, '0')}),
             (std::vector<unsigned>{8U, 6U, 0U}));

    // ADR-0179: ratings are query targets. The pseudo-fields push into SQL,
    // evaluate in expression predicates, and shadow same-named tags.
    const auto filtered_paths = [&](const char* source) {
        const auto compiled = query::compile_tkq(source);
        if (!compiled) {
            return std::vector<std::string>{};
        }
        auto paths = library->filter_paths(*compiled);
        return paths ? *paths : std::vector<std::string>{};
    };
    QCOMPARE(filtered_paths("rating GREATER 7"), std::vector<std::string>{path});
    QCOMPARE(filtered_paths("rating EQUAL 8"), std::vector<std::string>{path});
    QCOMPARE(filtered_paths("rating LESS 8"), std::vector<std::string>{});
    QCOMPARE(filtered_paths("rating PRESENT"), std::vector<std::string>{path});
    QCOMPARE(filtered_paths("albumrating EQUAL 6"), std::vector<std::string>{path});
    QCOMPARE(filtered_paths("albumrating GREATER 6"), std::vector<std::string>{});
    QCOMPARE(filtered_paths("\"$info(rating)\" EQUAL 8"), std::vector<std::string>{path});

    // A rescan recomputes hashes from unchanged tags, so the rating stays.
    persistence::LibraryScanProgress rescan;
    QVERIFY(library->scan({}, rescan));
    QCOMPARE(library->query(tracks())->entries.front().rating, 8U);

    // An index migrated before the identity columns existed backfills the
    // hashes from its complete field evidence on open, so stored ratings
    // join without waiting for a Refresh.
    {
        sqlite3* db = nullptr;
        QCOMPARE(sqlite3_open((base / "state.sqlite").c_str(), &db), SQLITE_OK);
        QCOMPARE(sqlite3_exec(db,
                              "UPDATE local_library_tracks SET rating_hash='',"
                              "album_rating_hash=''",
                              nullptr, nullptr, nullptr),
                 SQLITE_OK);
        sqlite3_close(db);
    }
    auto backfilled = persistence::LocalLibrary::open(base / "state.sqlite");
    QVERIFY(backfilled);
    QCOMPARE(backfilled->query(tracks())->entries.front().rating_hash, track_hash);
    QCOMPARE(backfilled->query(tracks())->entries.front().rating, 8U);
    const auto backfilled_query = query::compile_tkq("rating GREATER 7");
    QVERIFY(backfilled_query);
    QCOMPARE(*backfilled->filter_paths(*backfilled_query), std::vector<std::string>{path});

    // Unrating deletes the stored row instead of keeping a zero.
    QVERIFY(library->set_rating(track_hash, false, 0U));
    QCOMPARE(library->query(tracks())->entries.front().rating, 0U);
    QCOMPARE(library->ratings({track_hash})->front(), 0U);
    QCOMPARE(filtered_paths("rating MISSING"), std::vector<std::string>{path});
    QCOMPARE(filtered_paths("rating PRESENT"), std::vector<std::string>{});
}

void LocalLibraryTest::migrationRoundTrip() {
    QTemporaryDir temporary;
    const auto database = (std::filesystem::path{temporary.path().toStdString()} / "state.sqlite");
    {
        auto repository = persistence::ListRepository::open(database);
        QVERIFY(repository);
        QCOMPARE(*repository->schema_version(), 38U);
    }
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(database.c_str(), &db), SQLITE_OK);
    // Down in reverse order, up in forward order: the ADR-0150 field table
    // references the track table, so 0030 must unwind before 0028.
    for (const auto* name :
         {"0036_server_search_scope.down", "0035_local_ratings.down",
          "0034_metadata_field_filters.down", "0033_complete_library_fields.down",
          "0032_saved_searches.down", "0031_composed_metadata_artwork.down",
          "0030_library_query_index.down", "0028_local_library.down", "0028_local_library.up",
          "0030_library_query_index.up", "0031_composed_metadata_artwork.up",
          "0032_saved_searches.up", "0033_complete_library_fields.up",
          "0034_metadata_field_filters.up", "0035_local_ratings.up",
          "0036_server_search_scope.up"}) {
        QFile migration{
            QStringLiteral(TRACKKNIFE_MIGRATION_DIR "/%1.sql").arg(QString::fromLatin1(name))};
        QVERIFY(migration.open(QIODevice::ReadOnly));
        QCOMPARE(sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr), SQLITE_OK);
        QCOMPARE(sqlite3_exec(db, migration.readAll().constData(), nullptr, nullptr, nullptr),
                 SQLITE_OK);
        QCOMPARE(sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr), SQLITE_OK);
    }
    sqlite3_close(db);
    auto library = persistence::LocalLibrary::open(database);
    QVERIFY(library);
    QVERIFY(library->roots()->empty());
    QVERIFY(library->query(tracks())->entries.empty());
    auto repository = persistence::ListRepository::open(database);
    QVERIFY(repository.has_value());
    persistence::SavedSearch saved{.id = core::StableId::random(),
                                   .name = "Keep me",
                                   .expression = "Jazz",
                                   .dialect = "words-1"};
    QVERIFY(repository->save_search(saved).has_value());
    QCOMPARE(sqlite3_open(database.c_str(), &db), SQLITE_OK);
    QFile downgrade{QStringLiteral(TRACKKNIFE_MIGRATION_DIR "/0032_saved_searches.down.sql")};
    QVERIFY(downgrade.open(QIODevice::ReadOnly));
    QCOMPARE(sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr), SQLITE_OK);
    QCOMPARE(sqlite3_exec(db, downgrade.readAll().constData(), nullptr, nullptr, nullptr),
             SQLITE_CONSTRAINT);
    QCOMPARE(sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(db);
    QCOMPARE(repository->load_saved_searches()->size(), 1U);
    QCOMPARE(*repository->schema_version(), 38U);
}

void LocalLibraryTest::scansOnlyOnRefresh_data() {
    QTest::addColumn<bool>("cancel");
    QTest::newRow("completed") << false;
    QTest::newRow("cancelled-with-pending-changes") << true;
}

void LocalLibraryTest::scansOnlyOnRefresh() {
    QFETCH(bool, cancel);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    QVERIFY(!fixture(root, "01.flac").empty());
    const auto database = base / "state.sqlite";
    auto library = persistence::LocalLibrary::open(database);
    QVERIFY(library && library->add_root(root.native()));

    LocalLibraryPanel panel{database};
    auto* button = panel.findChild<QToolButton*>(QStringLiteral("local-library-scan"));
    QVERIFY(button);
    QVERIFY(!panel.property("scanning").toBool());
    // Accelerate any timers so an accidental periodic scanner is exercised.
    for (auto* timer : panel.findChildren<QTimer*>()) {
        timer->setInterval(10);
    }
    panel.refreshLibrary();
    QTest::qWait(100);
    QVERIFY(!panel.property("scanning").toBool());
    QVERIFY(library->paths(tracks())->empty());

    // Hold a manual scan at the database boundary to exercise cancellation
    // and view refreshes arriving during a slow scan.
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(database.c_str(), &db), SQLITE_OK);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> lock{db, sqlite3_close};
    QCOMPARE(sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr), SQLITE_OK);
    button->click();
    QVERIFY(panel.property("scanning").toBool());
    panel.refreshLibrary();
    QTest::qWait(100);
    QVERIFY(panel.property("scanning").toBool());
    if (cancel) {
        panel.refreshLibrary();
        button->click();
    }
    QCOMPARE(sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr), SQLITE_OK);
    QTRY_VERIFY(!panel.property("scanning").toBool());
    if (cancel) {
        QVERIFY(panel.findChild<QLabel*>(QStringLiteral("local-library-status"))
                    ->text()
                    .contains(QStringLiteral("Scan stopped")));
    } else {
        QCOMPARE(library->paths(tracks())->size(), 1U);
    }

    const auto indexed = library->paths(tracks())->size();
    QVERIFY(!fixture(root, "02.flac").empty());
    panel.refreshLibrary();
    QTest::qWait(100);
    QVERIFY(!panel.property("scanning").toBool());
    QCOMPARE(library->paths(tracks())->size(), indexed);

    button->click();
    QVERIFY(panel.property("scanning").toBool());
    QTRY_VERIFY(!panel.property("scanning").toBool());
    QCOMPARE(library->paths(tracks())->size(), 2U);
    panel.stop();
}

// ADR-0150: the query toggle switches the search field into tkq —
// structured results in the tree, inline diagnostics for malformed
// queries (never a silent word search), and Enter committing the
// filtered result set through the ADR-0140 snapshot-tab path.
void LocalLibraryTest::locateLoadsAdditionalTreePages() {
    QTemporaryDir temporary;
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    const auto path = fixture(root, "01.flac");
    const auto database = base / "state.sqlite";
    auto library = persistence::LocalLibrary::open(database);
    QVERIFY(library && library->add_root(root.native()));
    persistence::LibraryScanProgress progress;
    QVERIFY(library->scan({}, progress));
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(database.c_str(), &db), SQLITE_OK);
    // Fill both the artist and album levels beyond the 200-row page boundary.
    const char* insert = R"SQL(
        WITH RECURSIVE numbers(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM numbers WHERE n<410)
        INSERT INTO local_library_tracks(raw_path,root,revision,title,artist,album,album_key,
            release_id,date,disc,track,search_track,search_album,available,seen)
        SELECT CAST(t.raw_path || n AS BLOB),root,revision,title,
            CASE WHEN n<=205 THEN 'A artist ' || n ELSE artist END,
            'A album ' || n,'page-' || n,release_id,date,disc,track,search_track,search_album,available,seen
        FROM local_library_tracks t CROSS JOIN numbers
    )SQL";
    QCOMPARE(sqlite3_exec(db, insert, nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(db);
    LocalLibraryPanel panel{database};
    panel.show();
    panel.locatePath(path, true);
    auto* tree = panel.findChild<QTreeView*>();
    QTRY_VERIFY(tree->currentIndex().data().toString().contains(QStringLiteral("Test album")));
    QVERIFY(tree->currentIndex().parent().data().toString().contains(QStringLiteral("Björk")));
    QVERIFY(tree->model()->rowCount() > 200);
    QVERIFY(tree->model()->rowCount(tree->currentIndex().parent()) > 200);
    QVERIFY(!panel.property("scanning").toBool());
    panel.stop();
}

void LocalLibraryTest::cachedSearchTabsLoadCovers_data() {
    QTest::addColumn<bool>("standalone");
    QTest::newRow("sidebar") << false;
    QTest::newRow("standalone") << true;
}

void LocalLibraryTest::cachedSearchTabsLoadCovers() {
    QFETCH(bool, standalone);
    QTemporaryDir temporary;
    const auto old_data = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", temporary.path().toUtf8());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QCoreApplication::setOrganizationName(QStringLiteral("TrackknifeLibraryTests"));
    QCoreApplication::setApplicationName(QStringLiteral("CachedSearchCovers"));
    QSettings{}.clear();
    const auto root = std::filesystem::path{temporary.path().toStdString()} / "music";
    QVERIFY(!fixture(root, "01.flac").empty());
    QVERIFY(!fixture(root, "02.flac").empty());
    QImage cover{64, 64, QImage::Format_RGB32};
    cover.fill(Qt::green);
    QVERIFY(cover.save(QString::fromStdString((root / "cover.png").native())));
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.findChild<LocalLibraryPanel*>() != nullptr);
        auto* panel = window.findChild<LocalLibraryPanel*>();
        panel->addRoot(root.native());
        QTRY_COMPARE(panel->findChild<QLabel*>(QStringLiteral("local-library-status"))->text(),
                     QStringLiteral("Folder added. Press Refresh to scan for music."));
        panel->findChild<QToolButton*>(QStringLiteral("local-library-scan"))->click();
        QTRY_VERIFY(!panel->property("scanning").toBool());
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QVERIFY(tabs);
        const auto count = tabs->count();
        if (standalone) {
            window.findChild<QAction*>(QStringLiteral("action-search-dialog"))->trigger();
            auto* dialog = window.findChild<SearchDialog*>();
            QVERIFY(dialog);
            dialog->findChild<QCheckBox*>(QStringLiteral("bench-search-query-mode"))
                ->setChecked(false);
            dialog->findChild<QLineEdit*>(QStringLiteral("bench-search-input"))
                ->setText(QStringLiteral("Test album"));
            auto* open = dialog->findChild<QPushButton*>(QStringLiteral("bench-search-open-tab"));
            QTRY_VERIFY(open->isEnabled());
            open->click();
        } else {
            panel->findChild<QLineEdit*>(QStringLiteral("local-library-search"))
                ->setText(QStringLiteral("Test album"));
            panel->commitSearch();
        }
        QTRY_COMPARE(tabs->count(), count + 1);
        auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
        QVERIFY(view);
        auto* model = qobject_cast<LocalListModel*>(view->model());
        QVERIFY(model && model->rowCount() == 2);
        QVERIFY(model->rows()[0].probed && model->rows()[0].technicals);
        QVERIFY(!model->rows()[0].source_revision);
        QCOMPARE(model->rows()[0].metadata.fields.front().provenance,
                 metadata::FieldProvenance::cached_snapshot);
        QTRY_VERIFY(model->hasArtwork(model->groupKey(0)));
        if (auto* dialog = window.findChild<SearchDialog*>()) {
            dialog->close();
        }
        auto* tree = panel->findChild<QTreeView*>();
        for (const bool album : {false, true}) {
            const auto position = view->visualRect(model->index(1, 0)).center();
            QVERIFY(QMetaObject::invokeMethod(view, "customContextMenuRequested",
                                              Qt::DirectConnection, Q_ARG(QPoint, position)));
            auto* menu = window.findChild<QMenu*>(QStringLiteral("bench-track-context-menu"));
            QVERIFY(menu);
            auto* locate =
                menu->findChild<QAction*>(album ? QStringLiteral("action-local-locate-album")
                                                : QStringLiteral("action-local-locate-artist"));
            QVERIFY(locate && locate->isEnabled());
            locate->trigger();
            menu->close();
            QTRY_VERIFY(tree->currentIndex().data().toString().contains(
                album ? QStringLiteral("Test album") : QStringLiteral("Björk")));
            QCOMPARE(window.findChild<QTabBar*>(QStringLiteral("bench-local-source-tabs"))
                         ->currentIndex(),
                     1);
            QCOMPARE(model->rowCount(), 2);
            QVERIFY(!panel->property("scanning").toBool());
        }
        panel->locatePath("/not-indexed.flac", true);
        QTRY_VERIFY(panel->findChild<QLabel*>(QStringLiteral("local-library-status"))
                        ->text()
                        .contains(QStringLiteral("not in the local library")));
    }
    qputenv("XDG_DATA_HOME", old_data);
}

void LocalLibraryTest::dynamicRulesFollowIndexedTagsAndKeepRawPaths() {
    QTemporaryDir temporary;
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    const auto alpha = fixture(root, "raw-\xff.flac", "Alpha");
    QVERIFY(!alpha.empty());
    const auto database = base / "state.sqlite";
    auto library = persistence::LocalLibrary::open(database);
    QVERIFY(library && library->add_root(root.native()));
    persistence::LibraryScanProgress progress;
    QVERIFY(library->scan({}, progress));
    DynamicPlaylistDialog dialog{
        QStringLiteral("local"), QStringLiteral("Local library"),
        [database](query::CompiledTkq compiled, core::CancellationToken cancellation,
                   DynamicPlaylistService::Completion completion) {
            completion(queryDynamicLocalLibrary(database, compiled, cancellation));
        }};
    dialog.show();
    auto* input = dialog.findChild<QLineEdit*>(QStringLiteral("dynamic-query"));
    auto* refresh = dialog.findChild<QPushButton*>(QStringLiteral("dynamic-refresh"));
    auto* view = dialog.findChild<QTableView*>(QStringLiteral("dynamic-tracks"));
    input->setText(QStringLiteral("title IS Alpha"));
    refresh->click();
    QCOMPARE(view->model()->rowCount(), 1);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model);
    QCOMPARE(model->rows().front().raw_path, alpha);
    // Newly indexed matching tracks join the same definition after invalidation.
    QVERIFY(!fixture(root, "02.flac", "Alpha").empty());
    persistence::LibraryScanProgress next;
    QVERIFY(library->scan({}, next));
    dialog.libraryChanged();
    QTRY_COMPARE(view->model()->rowCount(), 2);
    // Results remain entirely index-backed when the media folder goes offline.
    std::filesystem::rename(root, base / "offline");
    refresh->click();
    QCOMPARE(view->model()->rowCount(), 2);
    std::vector<LocalTrackRow> snapshot;
    connect(&dialog, &DynamicPlaylistDialog::snapshotRequested, &dialog,
            [&snapshot](const QString&, const DynamicPlaylistService::Tracks& tracks) {
                snapshot = std::get<std::vector<LocalTrackRow>>(tracks);
            });
    dialog.findChild<QPushButton*>(QStringLiteral("dynamic-open"))->click();
    QCOMPARE(snapshot.size(), 2U);
    QVERIFY(snapshot.front().probed);
    auto cancelled = core::CancellationSource{};
    cancelled.request_cancellation();
    const auto compiled = query::compile_tkq("ALL");
    QVERIFY(compiled);
    QVERIFY(!queryDynamicLocalLibrary(database, *compiled, cancelled.token()));
}

void LocalLibraryTest::databaseSearchOpensCachedRowsWithoutFiles() {
    QTemporaryDir temporary;
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    const auto alpha = fixture(root, "raw-\xff.flac", "Alpha");
    const auto beta = fixture(root, "02.flac", "Beta");
    const auto database = base / "state.sqlite";
    auto library = persistence::LocalLibrary::open(database);
    QVERIFY(library && library->add_root(root.native()));
    persistence::LibraryScanProgress progress;
    QVERIFY(library->scan({}, progress));
    std::filesystem::rename(root, base / "offline");
    const auto cached = library->cached_tracks({beta, alpha, beta});
    QVERIFY(cached && cached->size() == 3);
    QCOMPARE(cached->at(0).raw_path, beta);
    QCOMPARE(cached->at(1).raw_path, alpha);
    QCOMPARE(cached->at(2).raw_path, beta);
    QCOMPARE(cached->at(1).facts.title, std::string{"Alpha"});
    QVERIFY(cached->at(1).facts.sample_rate > 0);
    QVERIFY(!library->cached_tracks({"/not-indexed.flac"}));
    core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto cancelled = library->cached_tracks({alpha}, cancellation.token());
    QVERIFY(!cancelled && cancelled.error().code == core::ErrorCode::cancelled);

    SearchDialog dialog{database, {}, {}};
    dialog.show();
    auto* input = dialog.findChild<QLineEdit*>(QStringLiteral("bench-search-input"));
    auto* mode = dialog.findChild<QCheckBox*>(QStringLiteral("bench-search-query-mode"));
    auto* results = dialog.findChild<QListWidget*>(QStringLiteral("bench-search-results"));
    auto* open = dialog.findChild<QPushButton*>(QStringLiteral("bench-search-open-tab"));
    QVERIFY(input && mode && results && open);
    QSignalSpy requested{&dialog, &SearchDialog::rowsRequested};
    mode->setChecked(true);
    input->setText(QStringLiteral("codec IS flac SORT DESCENDING BY %title%"));
    QTRY_COMPARE(results->count(), 2);
    open->click();
    QCOMPARE(requested.size(), 1);
    const auto rows = requested.takeFirst().at(1).value<std::vector<LocalTrackRow>>();
    QCOMPARE(rows.size(), 2U);
    QCOMPARE(rows[0].raw_path, beta);
    QCOMPARE(rows[1].raw_path, alpha);
    QCOMPARE(rows[1].title, std::string{"Alpha"});
    QCOMPARE(rows[1].metadata.first_effective_value("albumartist"),
             std::optional<std::string>{"Björk"});
    QCOMPARE(rows[1].metadata.fields.front().provenance,
             metadata::FieldProvenance::cached_snapshot);
    QVERIFY(rows[1].probed && rows[1].technicals && rows[1].duration_ms);
    QVERIFY(!rows[1].source_revision);
    mode->setChecked(false);
}

void LocalLibraryTest::queryModeFiltersAndCommitsResults() {
    QTemporaryDir temporary;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QCoreApplication::setOrganizationName(QStringLiteral("TrackknifeLibraryTests"));
    QCoreApplication::setApplicationName(QStringLiteral("LocalLibraryQuery"));
    QSettings{}.clear();
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    const auto jazz = fixture(root, "01.flac", "Alpha");
    const auto rock = fixture(root, "02.flac", "Beta");
    QVERIFY(!jazz.empty() && !rock.empty());
    const auto tag_genre = [](const std::string& path, const char* genre) {
        TagLib::FLAC::File file{path.c_str()};
        auto properties = file.properties();
        properties.replace("GENRE", TagLib::String{genre});
        file.setProperties(properties);
        QVERIFY(file.save());
    };
    tag_genre(jazz, "Jazz");
    tag_genre(rock, "Rock");
    const auto database = base / "state.sqlite";
    auto library = persistence::LocalLibrary::open(database);
    QVERIFY(library && library->add_root(root.native()));
    persistence::LibraryScanProgress progress;
    QVERIFY(library->scan({}, progress).has_value());
    QCOMPARE(progress.indexed.load(), 2U);

    LocalLibraryPanel panel{database};
    panel.show();
    auto* search = panel.findChild<QLineEdit*>(QStringLiteral("local-library-search"));
    auto* toggle = panel.findChild<QCheckBox*>(QStringLiteral("local-library-query-toggle"));
    auto* error = panel.findChild<QLabel*>(QStringLiteral("local-library-query-error"));
    auto* tree = panel.findChild<QTreeView*>();
    QVERIFY(search != nullptr && toggle != nullptr && error != nullptr && tree != nullptr);
    QVERIFY(!toggle->isChecked());
    toggle->setChecked(true);

    QSignalSpy committed{&panel, &LocalLibraryPanel::searchCommitted};
    search->setText(QStringLiteral("genre IS jazz"));
    auto* model = tree->model();
    QTRY_VERIFY(model->rowCount() == 1 &&
                model->data(model->index(0, 0)).toString() == QStringLiteral("Tracks"));
    const auto group = model->index(0, 0);
    QTRY_COMPARE(model->rowCount(group), 1);
    QVERIFY(model->data(model->index(0, 0, group)).toString().contains(QStringLiteral("Alpha")));
    QVERIFY(!error->isVisible());

    // Malformed structure is a visible diagnostic, not a word search.
    search->setText(QStringLiteral("genre HAS"));
    QTRY_VERIFY(error->isVisible());
    QVERIFY(!error->text().isEmpty());

    // A valid query commits its full filtered result set.
    search->setText(QStringLiteral("genre IS rock OR genre IS jazz"));
    QTRY_VERIFY(!error->isVisible());
    QTRY_VERIFY(model->rowCount() == 1 && model->rowCount(model->index(0, 0)) == 2);
    QTest::keyClick(search, Qt::Key_Return);
    QTRY_COMPARE(committed.size(), 1);
    const auto arguments = committed.takeFirst();
    QCOMPARE(arguments.at(0).toString(), QStringLiteral("genre IS rock OR genre IS jazz"));
    const auto rows = arguments.at(1).value<std::vector<LocalTrackRow>>();
    QCOMPARE(rows.size(), 2U);
    QVERIFY(rows.front().probed && rows.front().technicals.has_value());

    // Off again: the plain word search is untouched and the sticky
    // setting resets for later tests.
    toggle->setChecked(false);
    search->setText(QStringLiteral("Alpha"));
    QTRY_VERIFY(model->rowCount() == 2);
    panel.stop();
}

void LocalLibraryTest::localViewBrowsesSearchesAndOpensFiles() {
    QTemporaryDir temporary;
    const auto old_data = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", temporary.path().toUtf8());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QCoreApplication::setOrganizationName(QStringLiteral("TrackknifeLibraryTests"));
    QCoreApplication::setApplicationName(QStringLiteral("LocalLibrary"));
    QSettings{}.clear();
    const auto root = std::filesystem::path{temporary.path().toStdString()} / "music";
    const auto path = fixture(root, "01.flac");
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.findChild<LocalLibraryPanel*>() != nullptr);
        auto* panel = window.findChild<LocalLibraryPanel*>();
        auto* selector = window.findChild<QTabBar*>(QStringLiteral("bench-local-source-tabs"));
        auto* tree = panel->findChild<QTreeView*>();
        auto* search = panel->findChild<QLineEdit*>();
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        auto* sources = window.findChild<QStackedWidget*>(QStringLiteral("bench-source-stack"));
        QVERIFY(selector && tree && search && tabs && sources);
        QCOMPARE(selector->currentIndex(), 0);
        selector->setCurrentIndex(1);
        QCOMPARE(sources->currentWidget(), panel);
        panel->addRoot(root.native());
        QTRY_COMPARE(panel->findChild<QLabel*>(QStringLiteral("local-library-status"))->text(),
                     QStringLiteral("Folder added. Press Refresh to scan for music."));
        QVERIFY(!panel->property("scanning").toBool());
        panel->findChild<QToolButton*>(QStringLiteral("local-library-scan"))->click();
        QTRY_VERIFY(tree->model()->index(0, 0).data().toString().contains(QStringLiteral("Björk")));
        const auto artist = tree->model()->index(0, 0);
        QCOMPARE(artist.data(ui::ServerLibraryTreeDelegate::secondaryTextRole).toString(),
                 QStringLiteral("1 album"));
        tree->expand(artist);
        QTRY_VERIFY(tree->model()
                        ->index(0, 0, tree->model()->index(0, 0))
                        .data()
                        .toString()
                        .contains(QStringLiteral("Test album")));
        search->setText(QStringLiteral("Test album"));
        QCOMPARE(panel->findChild<QLabel*>(QStringLiteral("local-library-status"))->text(),
                 QStringLiteral("Searching…"));
        QTRY_COMPARE(tree->model()->rowCount(), 2);
        QTRY_VERIFY(tree->model()
                        ->index(0, 0, tree->model()->index(0, 0))
                        .data()
                        .toString()
                        .contains(QStringLiteral("Test album")));
        QTRY_VERIFY(tree->model()
                        ->index(0, 0, tree->model()->index(1, 0))
                        .data()
                        .toString()
                        .contains(QStringLiteral("First song")));
        const auto album = tree->model()->index(0, 0, tree->model()->index(0, 0));
        tree->setCurrentIndex(album);
        QVERIFY(triggerLibraryAction(panel, album, 0));
        QTRY_VERIFY(qobject_cast<QTableView*>(tabs->currentWidget()) != nullptr &&
                    qobject_cast<QTableView*>(tabs->currentWidget())->model()->rowCount() == 1);
        auto* local = qobject_cast<LocalListModel*>(
            qobject_cast<QTableView*>(tabs->currentWidget())->model());
        QVERIFY(local);
        QCOMPARE(local->rows().front().raw_path, path);
        auto* local_view = qobject_cast<QTableView*>(tabs->currentWidget());
        auto* mpd = window.findChild<QTableView*>(QStringLiteral("bench-mpd-queue"));
        QVERIFY(local_view && mpd);
        const auto second = fixture(root, "02.flac", "Second song");
        panel->findChild<QToolButton*>(QStringLiteral("local-library-scan"))->click();
        QTRY_VERIFY(tree->model()
                        ->index(0, 0, tree->model()->index(0, 0))
                        .data(ui::ServerLibraryTreeDelegate::secondaryTextRole)
                        .toString()
                        .contains(QStringLiteral("2 tracks")));
        const auto album_index = [&] {
            return tree->model()->index(0, 0, tree->model()->index(0, 0));
        };
        // An album drag captures the selection before a search reset and resolves
        // asynchronously into the exact target tab and insertion row.
        std::unique_ptr<QMimeData> mime{tree->model()->mimeData({album_index()})};
        QVERIFY(dynamic_cast<ui::LocalFilesMimeData*>(mime.get()));
        const auto first_rect = local_view->visualRect(local->index(0, 0));
        QVERIFY(dropFiles(local_view, mime.get(), first_rect.topLeft() + QPoint{5, 2}));
        search->setText(QStringLiteral("no match"));
        tabs->setCurrentWidget(mpd);
        QTRY_COMPARE(local->rowCount(), 3);
        QCOMPARE(local->rows()[0].raw_path, path);
        QCOMPARE(local->rows()[1].raw_path, second);
        QCOMPARE(local->rows()[2].raw_path, path);
        QCOMPARE(tabs->currentWidget(), mpd);
        QVERIFY(!dropFiles(mpd, mime.get(), QPoint{20, 20}));
        QCOMPARE(mpd->model()->rowCount(), 0);
        tabs->setCurrentWidget(local_view);
        search->setText(QStringLiteral("Test album"));
        QTRY_VERIFY(album_index()
                        .data(ui::ServerLibraryTreeDelegate::secondaryTextRole)
                        .toString()
                        .contains(QStringLiteral("2 tracks")));
        {
            ui::ServerLibraryTreeDelegate mpd_delegate{
                static_cast<ui::ServerLibraryTreeView*>(tree), {}};
            QStyleOptionViewItem option;
            option.initFrom(tree);
            const auto heading = tree->model()->index(0, 0);
            QCOMPARE(tree->itemDelegate()->sizeHint(option, heading).height(),
                     mpd_delegate.sizeHint(option, heading).height());
        }
        // Append an overlapping album + track selection once, preserving the
        // multi-selection when opening the menu on an already selected entry.
        QTRY_COMPARE(tree->model()->rowCount(tree->model()->index(1, 0)), 2);
        tree->selectionModel()->select(album_index(), QItemSelectionModel::ClearAndSelect |
                                                          QItemSelectionModel::Rows);
        const auto first_track = tree->model()->index(0, 0, tree->model()->index(1, 0));
        tree->selectionModel()->select(first_track,
                                       QItemSelectionModel::Select | QItemSelectionModel::Rows);
        QVERIFY(triggerLibraryAction(panel, first_track, 0));
        QCOMPARE(tree->selectionModel()->selectedRows().size(), 2);
        QTRY_COMPARE(local->rowCount(), 5);
        QCOMPARE(local->rows()[3].raw_path, path);
        QCOMPARE(local->rows()[4].raw_path, second);
        tree->setCurrentIndex(album_index());
        local_view->setCurrentIndex(local->index(0, 0));
        QVERIFY(triggerLibraryAction(panel, album_index(), 1));
        QTRY_COMPARE(local->rowCount(), 7);
        QCOMPARE(local->rows()[1].raw_path, path);
        QCOMPARE(local->rows()[2].raw_path, second);
        QVERIFY(triggerLibraryAction(panel, album_index(), 2));
        QTRY_COMPARE(local->rowCount(), 2);
        QCOMPARE(local->rows()[0].raw_path, path);
        QCOMPARE(local->rows()[1].raw_path, second);
        const auto original_tabs = tabs->count();
        QVERIFY(triggerLibraryAction(panel, album_index(), 3));
        QTRY_COMPARE(tabs->count(), original_tabs + 1);
        auto* new_view = qobject_cast<QTableView*>(tabs->currentWidget());
        QVERIFY(new_view && new_view != local_view);
        QTRY_COMPARE(new_view->model()->rowCount(), 2);
        // ADR-0140: Enter in the search field keeps the full result set as
        // a new list tab named after the query.
        std::filesystem::rename(root, root.parent_path() / "offline");
        search->setFocus();
        QTest::keyClick(search, Qt::Key_Return);
        QTRY_COMPARE(tabs->count(), original_tabs + 2);
        auto* committed_view = qobject_cast<QTableView*>(tabs->currentWidget());
        QVERIFY(committed_view != nullptr && committed_view != new_view);
        QVERIFY(
            tabs->tabText(tabs->currentIndex()).startsWith(QStringLiteral("Search: Test album")));
        QTRY_COMPARE(committed_view->model()->rowCount(), 2);
        {
            auto* committed_model = qobject_cast<LocalListModel*>(committed_view->model());
            QVERIFY(committed_model != nullptr);
            QTRY_COMPARE(committed_model->rows()[0].raw_path, path);
            QCOMPARE(committed_model->rows()[1].raw_path, second);
            QVERIFY(committed_model->rows()[0].probed);
            QVERIFY(committed_model->rows()[0].technicals.has_value());
            QCOMPARE(committed_model->rows()[0].title, std::string{"First song"});
            QCOMPARE(committed_model->rows()[0].metadata.fields.front().provenance,
                     metadata::FieldProvenance::cached_snapshot);
        }
        std::filesystem::rename(root.parent_path() / "offline", root);
        const auto committed_index = tabs->currentIndex();
        QTimer::singleShot(0, [] {
            if (auto* confirmation =
                    qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                confirmation->done(QMessageBox::Yes);
            }
        });
        QVERIFY(QMetaObject::invokeMethod(tabs, "tabCloseRequested", Qt::DirectConnection,
                                          Q_ARG(int, committed_index)));
        QTRY_COMPARE(tabs->count(), original_tabs + 1);
        tabs->setCurrentWidget(new_view);
        // Closing a captured destination must never redirect its pending drop.
        QVERIFY(dropFiles(new_view, mime.get(), QPoint{20, 20}));
        const auto closed_index = tabs->currentIndex();
        QTimer::singleShot(0, [] {
            if (auto* confirmation =
                    qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                confirmation->done(QMessageBox::Yes);
            }
        });
        QVERIFY(QMetaObject::invokeMethod(tabs, "tabCloseRequested", Qt::DirectConnection,
                                          Q_ARG(int, closed_index)));
        tabs->setCurrentWidget(local_view);
        QTest::qWait(300);
        QCOMPARE(local->rowCount(), 2);
        QVERIFY(!panel->property("scanning").toBool());
        if (const auto screenshot = qgetenv("TRACKKNIFE_LIBRARY_SCREENSHOT");
            !screenshot.isEmpty()) {
            tree->setFocus();
            tree->setCurrentIndex(album_index());
            QVERIFY(window.grab().save(QString::fromUtf8(screenshot)));
        }
        tabs->setCurrentWidget(mpd);
        QVERIFY(!panel->isVisible());
        QCOMPARE(window.property("trackknife-active-authority").toString(), QStringLiteral("mpd"));
        QVERIFY(sources->currentWidget() != panel);
    }
    qputenv("XDG_DATA_HOME", old_data);
}

void LocalLibraryTest::dragResolvesUnloadedPagesAndRawPaths() {
    QTemporaryDir temporary;
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    const auto raw = fixture(root, "raw-\xff.flac");
    QVERIFY(!raw.empty());
    for (int index = 0; index < 204; ++index) {
        std::filesystem::copy_file(std::filesystem::path{raw},
                                   root / (std::to_string(index) + ".flac"));
    }
    auto library = persistence::LocalLibrary::open(base / "state.sqlite");
    QVERIFY(library && library->add_root(root.native()));
    persistence::LibraryScanProgress progress;
    QVERIFY(library->scan({}, progress));
    LocalLibraryPanel panel{base / "state.sqlite"};
    panel.resize(420, 400);
    panel.show();
    auto* tree = panel.findChild<QTreeView*>();
    QTRY_COMPARE(tree->model()->rowCount(), 1);
    const auto artist = tree->model()->index(0, 0);
    QVERIFY(!tree->isExpanded(artist));
    std::unique_ptr<QMimeData> mime{tree->model()->mimeData({artist})};
    const auto* files = dynamic_cast<ui::LocalFilesMimeData*>(mime.get());
    QVERIFY(files);
    std::vector<std::string> paths;
    files->resolve([&](std::vector<std::string> resolved) { paths = std::move(resolved); });
    QTRY_COMPARE(paths.size(), 205U);
    QVERIFY(std::ranges::find(paths, raw) != paths.end());
    QVERIFY(!tree->isExpanded(artist));
    QVERIFY(!panel.property("scanning").toBool());
    // Shared MPD-style branch interaction and inline actions work with local
    // presentation data, without enabling actions on placeholder rows.
    tree->setCurrentIndex(artist);
    QTest::keyClick(tree, Qt::Key_Return);
    QTRY_VERIFY(tree->isExpanded(artist));
    QTRY_VERIFY(tree->model()
                    ->index(0, 0, artist)
                    .data()
                    .toString()
                    .contains(QStringLiteral("Test album")));
    const auto album = tree->model()->index(0, 0, artist);
    std::vector<persistence::LibraryEntry> selected;
    connect(&panel, &LocalLibraryPanel::actionRequested, &panel,
            [&](std::vector<persistence::LibraryEntry> entries, LocalLibraryAction action) {
                QCOMPARE(action, LocalLibraryAction::append);
                selected = std::move(entries);
            });
    const auto action_rect = ui::ServerLibraryTreeView::actionRect(tree->visualRect(album), 0);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, action_rect.center());
    QCOMPARE(selected.size(), 1U);
    QCOMPARE(selected.front().kind, persistence::LibraryEntryKind::album);
    QCOMPARE(selected.front().tracks, 205U);
    tree->selectionModel()->select(artist, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    selected.clear();
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, action_rect.center());
    QCOMPARE(tree->selectionModel()->selectedRows().size(), 2);
    QCOMPARE(selected.size(), 1U);
    QCOMPARE(selected.front().kind, persistence::LibraryEntryKind::artist);
    std::filesystem::rename(root, base / "offline");
    persistence::LibraryScanProgress offline;
    QVERIFY(library->scan({}, offline));
    panel.refreshLibrary();
    QTRY_VERIFY(
        tree->model()->index(0, 0).data().toString().contains(QStringLiteral("unavailable")));
    auto* menu = libraryMenu(&panel, tree->model()->index(0, 0));
    QVERIFY(menu);
    for (int action = 0; action < 4; ++action) {
        auto* command =
            menu->findChild<QAction*>(QStringLiteral("action-local-library-%1").arg(action));
        QVERIFY(command && !command->isEnabled());
    }
    menu->close();
    std::filesystem::rename(base / "offline", root);
    std::filesystem::remove(std::filesystem::path{raw});
}

void LocalLibraryTest::trackNumbersAppearInTreeAndSearch() {
    QTemporaryDir temporary;
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    const auto first = fixture(root, "first.flac", "First song", "Test album", "3/12");
    const auto last = fixture(root, "last.flac", "Last song", "Test album", "12");
    const auto unknown = fixture(root, "unknown.flac", "Unnumbered", "Test album", "");
    auto library = persistence::LocalLibrary::open(base / "state.sqlite");
    QVERIFY(library && library->add_root(root.native()));
    persistence::LibraryScanProgress progress;
    QVERIFY(library->scan({}, progress));
    auto query = tracks();
    query.raw_path = first;
    auto page = library->query(query);
    QVERIFY(page && page->entries.size() == 1U);
    QCOMPARE(page->entries.front().track_number, 3);
    QCOMPARE(page->entries.front().label, std::string{"03. First song"});
    query.raw_path = last;
    QCOMPARE(library->query(query)->entries.front().label, std::string{"12. Last song"});
    query.raw_path = unknown;
    QCOMPARE(library->query(query)->entries.front().label, std::string{"Unnumbered"});
    QCOMPARE(library->query(tracks("First"))->entries.front().label,
             std::string{"Björk — 03. First song"});
    QCOMPARE(library->query(tracks("Unnumbered"))->entries.front().label,
             std::string{"Björk — Unnumbered"});
}

void LocalLibraryTest::albumCoversLoadAndRefresh() {
    QTemporaryDir temporary;
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    const auto source = fixture(root, "raw-\xff.flac");
    QVERIFY(!source.empty());
    QImage cover{512, 256, QImage::Format_RGB32};
    cover.fill(Qt::blue);
    QByteArray bytes;
    QBuffer buffer{&bytes};
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(cover.save(&buffer, "PNG"));
    {
        TagLib::FLAC::File file{source.c_str()};
        auto* picture = new TagLib::FLAC::Picture;
        picture->setType(TagLib::FLAC::Picture::FrontCover);
        picture->setMimeType("image/png");
        picture->setWidth(cover.width());
        picture->setHeight(cover.height());
        picture->setColorDepth(24);
        picture->setData(
            TagLib::ByteVector{bytes.constData(), static_cast<unsigned int>(bytes.size())});
        file.addPicture(picture);
        QVERIFY(file.save());
    }
    const auto folder_cover = QString::fromStdString((root / "cover.png").native());
    cover.fill(Qt::red);
    QVERIFY(cover.save(folder_cover));
    auto library = persistence::LocalLibrary::open(base / "state.sqlite");
    QVERIFY(library && library->add_root(root.native()));
    persistence::LibraryScanProgress progress;
    QVERIFY(library->scan({}, progress));
    persistence::LibraryQuery albums;
    albums.kind = persistence::LibraryEntryKind::album;
    const auto album_key = library->query(albums)->entries.front().key;
    const auto representative = library->artwork_source(album_key);
    QVERIFY(representative && representative->has_value());
    QCOMPARE(**representative, source);
    LocalLibraryPanel panel{base / "state.sqlite"};
    panel.resize(420, 400);
    auto* tree = panel.findChild<QTreeView*>();
    auto* search = panel.findChild<QLineEdit*>();
    search->setText(QStringLiteral("Test album"));
    panel.show();
    const auto album = [&] { return tree->model()->index(0, 0, tree->model()->index(0, 0)); };
    const auto image = [&] {
        return album().data(Qt::DecorationRole).value<QIcon>().pixmap(128, 128).toImage();
    };
    const auto color = [&] {
        const auto loaded = image();
        return loaded.isNull() ? QColor{}
                               : loaded.pixelColor(loaded.width() / 2, loaded.height() / 2);
    };
    QTRY_COMPARE(color(), QColor{Qt::blue});
    QCOMPARE(image().size(), QSize(128, 64));
    QVERIFY(!panel.property("scanning").toBool());
    {
        TagLib::FLAC::File file{source.c_str()};
        file.removePictures();
        QVERIFY(file.save());
    }
    // Operation notifications invalidate thumbnails without a library scan.
    panel.refreshLibrary();
    QTRY_COMPARE(color(), QColor{Qt::red});
    QVERIFY(!panel.property("scanning").toBool());
    cover.fill(Qt::green);
    QVERIFY(cover.save(folder_cover));
    QCOMPARE(color(), QColor{Qt::red});
    panel.findChild<QToolButton*>(QStringLiteral("local-library-scan"))->click();
    QTRY_COMPARE(color(), QColor{Qt::green});
    QTRY_VERIFY(!panel.property("scanning").toBool());
    panel.hide();
    cover.fill(Qt::yellow);
    QVERIFY(cover.save(folder_cover));
    panel.refreshLibrary();
    QTest::qWait(300);
    QVERIFY(color() != QColor{Qt::yellow});
    panel.show();
    QTRY_COMPARE(color(), QColor{Qt::yellow});
    QCOMPARE(search->text(), QStringLiteral("Test album"));
    search->clear();
    QTRY_VERIFY(tree->model()->index(0, 0).data().toString().contains(QStringLiteral("Björk")));
    tree->expand(tree->model()->index(0, 0));
    QTRY_COMPARE(color(), QColor{Qt::yellow});
    cover.fill(Qt::cyan);
    QVERIFY(cover.save(folder_cover));
    panel.findChild<QToolButton*>(QStringLiteral("local-library-scan"))->click();
    QTRY_COMPARE(color(), QColor{Qt::cyan});
    search->setText(QStringLiteral("Test album"));
    QTRY_COMPARE(tree->model()->index(0, 0).data().toString(), QStringLiteral("Albums"));
    QTRY_COMPARE(color(), QColor{Qt::cyan});
    QVERIFY(!panel.property("scanning").toBool());
    core::CancellationSource cancelled;
    cancelled.request_cancellation();
    QVERIFY(ui::loadLocalArtwork(source, cancelled.token()).isNull());
    // Corrupt and oversized fallback images leave the placeholder intact.
    {
        std::ofstream invalid{root / "cover.png", std::ios::binary | std::ios::trunc};
        invalid << "not an image";
    }
    QVERIFY(ui::loadLocalArtwork(source).isNull());
    std::filesystem::resize_file(root / "cover.png", 17U * 1024U * 1024U);
    QVERIFY(ui::loadLocalArtwork(source).isNull());
    panel.stop();
    QVERIFY(std::filesystem::remove(std::filesystem::path{source}));
}

} // namespace trackknife::bench

QTEST_MAIN(trackknife::bench::LocalLibraryTest)
#include "local_library_test.moc"
