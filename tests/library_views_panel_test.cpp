// SPDX-License-Identifier: GPL-3.0-only

// ADR-0254: the library panel shows a chosen view, grouped by the engine it
// browses: the dropdown chooses it, rows open level by level, a group's
// files are the group's, a query narrows it, the editor's views appear in
// every panel, and Go to album goes back to the artist tree.

#include "bench/catalogue_source.hpp"
#include "bench/library_views_dialog.hpp"
#include "bench/local_library_panel.hpp"
#include "bench/settings_dialog.hpp"
#include "trackknife/engine/catalogue_methods.hpp"
#include "trackknife/engine/server.hpp"
#include "workspace/library_browser.hpp"
#include "workspace/library_view_definitions.hpp"

#include <taglib/flacfile.h>
#include <taglib/tpropertymap.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeView>
#include <QtTest>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace trackknife::bench {

namespace engine = trackknife::engine;
namespace protocol = trackknife::protocol;

namespace {

std::string tagged(const QString& folder, const std::string& name, const std::string& artist,
                   const std::string& album, const std::vector<std::string>& genres) {
    QFile encoded{QStringLiteral(TRACKKNIFE_AUDIO_FIXTURE_DIR "/tagged-tone-flac.b64")};
    if (!encoded.open(QIODevice::ReadOnly)) {
        return {};
    }
    const auto data = QByteArray::fromBase64(encoded.readAll());
    std::filesystem::create_directories(folder.toStdString());
    const auto path = folder.toStdString() + "/" + name;
    {
        std::ofstream output{path, std::ios::binary};
        output.write(data.data(), data.size());
    }
    TagLib::FLAC::File file{path.c_str()};
    auto properties = file.properties();
    properties.replace("TITLE", TagLib::String{name, TagLib::String::UTF8});
    properties.replace("ARTIST", TagLib::String{artist, TagLib::String::UTF8});
    properties.replace("ALBUMARTIST", TagLib::String{artist, TagLib::String::UTF8});
    properties.replace("ALBUM", TagLib::String{album, TagLib::String::UTF8});
    properties.erase("GENRE");
    if (!genres.empty()) {
        TagLib::StringList list;
        for (const auto& genre : genres) {
            list.append(TagLib::String{genre, TagLib::String::UTF8});
        }
        properties.replace("GENRE", list);
    }
    file.setProperties(properties);
    return file.save() ? path : std::string{};
}

QStringList rows(const QAbstractItemModel* model, const QModelIndex& parent = {}) {
    QStringList found;
    for (int row = 0; row < model->rowCount(parent); ++row) {
        found.push_back(model->index(row, 0, parent).data().toString());
    }
    return found;
}

QModelIndex row(const QAbstractItemModel* model, const QString& label,
                const QModelIndex& parent = {}) {
    for (int index = 0; index < model->rowCount(parent); ++index) {
        if (model->index(index, 0, parent).data().toString() == label) {
            return model->index(index, 0, parent);
        }
    }
    return {};
}

} // namespace

class LibraryViewsPanelTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
    void init();
    void thePanelShowsAView();
    void theFooterFollowsALaterPalette();
};

void LibraryViewsPanelTest::initTestCase() { QStandardPaths::setTestModeEnabled(true); }

void LibraryViewsPanelTest::init() {
    QSettings settings;
    settings.remove(QStringLiteral("library/view"));
    settings.remove(QStringLiteral("library/views"));
    settings.remove(QStringLiteral("library/newest-first"));
    settings.remove(QStringLiteral("library/query-mode"));
}

void LibraryViewsPanelTest::thePanelShowsAView() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto music = directory.path() + QStringLiteral("/music");
    const auto jazz_and_rock =
        tagged(music + QStringLiteral("/one"), "a.flac", "Alpha", "One", {"Rock", "Jazz"});
    QVERIFY(!jazz_and_rock.empty());
    QVERIFY(!tagged(music + QStringLiteral("/one"), "b.flac", "Alpha", "One", {"Rock"}).empty());
    QVERIFY(!tagged(music + QStringLiteral("/two"), "c.flac", "Beta", "Two", {}).empty());

    const std::filesystem::path database{
        (directory.path() + QStringLiteral("/engine.sqlite3")).toStdString()};
    const std::filesystem::path socket{
        (directory.path() + QStringLiteral("/engine.sock")).toStdString()};
    engine::LocalCatalogue catalogue{database};
    QVERIFY(catalogue.prepare().has_value());
    QVERIFY(catalogue.add_root(music.toStdString()).has_value());
    persistence::LibraryScanProgress progress;
    QVERIFY(catalogue.scan({}, progress).has_value());
    protocol::Dispatcher dispatcher;
    engine::register_catalogue_methods(dispatcher, catalogue);
    auto server = engine::Server::listen(socket, dispatcher);
    QVERIFY(server.has_value());
    (*server)->start();
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_engine_socket_key),
                         QString::fromStdString(socket.string()));

    const std::filesystem::path unused{
        (directory.path() + QStringLiteral("/unused.sqlite3")).toStdString()};
    CatalogueSource catalogues{unused, CatalogueSource::Role::remote};
    LocalLibraryPanel panel{catalogues};
    panel.show();
    auto* tree = panel.findChild<QTreeView*>(QStringLiteral("local-library-tree"));
    auto* choice = panel.findChild<QComboBox*>(QStringLiteral("local-library-view"));
    QVERIFY(tree != nullptr && choice != nullptr);
    const auto* model = tree->model();
    QTRY_COMPARE(rows(model), (QStringList{QStringLiteral("Alpha"), QStringLiteral("Beta")}));
    QCOMPARE(choice->currentData().toString(), default_library_view_id);
    // Recently added is a view now, not a toggle.
    QVERIFY(choice->findData(recent_library_view_id) >= 0);

    // The genre view: a track under each of its genres, none last.
    const auto genres = choice->findData(QStringLiteral("genre-artist-album"));
    QVERIFY(genres >= 0);
    choice->setCurrentIndex(genres);
    emit choice->activated(genres);
    QTRY_COMPARE(rows(model), (QStringList{QStringLiteral("Jazz"), QStringLiteral("Rock"),
                                           QStringLiteral("Unknown")}));
    QCOMPARE(QSettings{}.value(QStringLiteral("library/view")).toString(),
             QStringLiteral("genre-artist-album"));

    const auto rock = row(model, QStringLiteral("Rock"));
    tree->expand(rock);
    QTRY_COMPARE(rows(model, rock), QStringList{QStringLiteral("Alpha")});
    const auto alpha = row(model, QStringLiteral("Alpha"), rock);
    tree->expand(alpha);
    QTRY_COMPARE(rows(model, alpha), QStringList{QStringLiteral("One")});
    const auto one = row(model, QStringLiteral("One"), alpha);
    // One album at the last level is the album, with its cover's key.
    QCOMPARE(one.data(library_kind_role).toString(), QStringLiteral("album"));
    QVERIFY(!one.data(library_cover_key_role).toString().isEmpty());
    tree->expand(one);
    QTRY_COMPARE(model->rowCount(one), 2);

    // Under Jazz the same album is only its jazz track: its files are what
    // the view shows, not the whole album.
    const auto jazz = row(model, QStringLiteral("Jazz"));
    tree->expand(jazz);
    QTRY_COMPARE(rows(model, jazz), QStringList{QStringLiteral("Alpha")});
    const auto jazz_alpha = row(model, QStringLiteral("Alpha"), jazz);
    tree->expand(jazz_alpha);
    QTRY_COMPARE(model->rowCount(jazz_alpha), 1);
    const auto jazz_one = model->index(0, 0, jazz_alpha);
    std::vector<std::string> resolved;
    panel.resolveEntries(
        LibraryBrowser::selectedEntries({jazz_one}),
        [&resolved](std::vector<std::string> paths) { resolved = std::move(paths); });
    QTRY_COMPARE(resolved, std::vector<std::string>{jazz_and_rock});
    resolved.clear();
    panel.resolveEntries(
        LibraryBrowser::selectedEntries({rock}),
        [&resolved](std::vector<std::string> paths) { resolved = std::move(paths); });
    QTRY_COMPARE(resolved.size(), std::size_t{2});

    // A query narrows the view before it is grouped.
    auto* query = panel.findChild<QCheckBox*>(QStringLiteral("local-library-query-toggle"));
    auto* search = panel.findChild<QLineEdit*>(QStringLiteral("local-library-search"));
    QVERIFY(query != nullptr && search != nullptr);
    query->setChecked(true);
    search->setText(QStringLiteral("genre IS jazz"));
    QTRY_COMPARE(rows(model), (QStringList{QStringLiteral("Jazz"), QStringLiteral("Rock")}));
    search->clear();
    query->setChecked(false);
    QTRY_COMPARE(rows(model).size(), 3);

    // A view saved in the editor is offered at once, in every panel.
    LibraryViewDefinition by_album{
        .id = QStringLiteral("mine"),
        .name = QStringLiteral("By album"),
        .levels = {{.format = "%album%", .sort = {}, .descending = true}},
        .builtin = false};
    QVERIFY(saveCustomLibraryViews({by_album}).has_value());
    const auto mine = choice->findData(QStringLiteral("mine"));
    QVERIFY(mine >= 0);
    choice->setCurrentIndex(mine);
    emit choice->activated(mine);
    QTRY_COMPARE(rows(model), (QStringList{QStringLiteral("Two"), QStringLiteral("One")}));

    // The editor: a level that does not compile says so and cannot be saved.
    auto* edit = panel.findChild<QToolButton*>(QStringLiteral("local-library-edit-views"));
    QVERIFY(edit != nullptr);
    edit->click();
    auto* dialog = panel.window()->findChild<LibraryViewsDialog*>();
    QVERIFY(dialog != nullptr);
    auto* list = dialog->findChild<QListWidget*>(QStringLiteral("library-views-list"));
    QVERIFY(list != nullptr && list->currentItem() != nullptr);
    QCOMPARE(list->currentItem()->text(), QStringLiteral("By album"));
    auto* levels = dialog->findChild<QTableWidget*>(QStringLiteral("library-view-levels"));
    QVERIFY(levels != nullptr && levels->rowCount() == 1);
    levels->item(0, 0)->setText(QStringLiteral("$if("));
    auto* error = dialog->findChild<QLabel*>(QStringLiteral("library-view-error"));
    auto* save = dialog->findChild<QPushButton*>(QStringLiteral("library-views-save"));
    QVERIFY(error != nullptr && save != nullptr);
    QVERIFY(error->isVisible() && error->text().startsWith(QStringLiteral("Level 1")));
    QVERIFY(!save->isEnabled());
    levels->item(0, 0)->setText(QStringLiteral("%albumartist%"));
    QVERIFY(!error->isVisible());
    QVERIFY(save->isEnabled());
    save->click();
    QTRY_COMPARE(rows(model), (QStringList{QStringLiteral("Beta"), QStringLiteral("Alpha")}));
    // The preview shows the view being edited.
    auto* preview = dialog->findChild<QTreeView*>(QStringLiteral("library-view-preview"));
    QVERIFY(preview != nullptr);
    QTRY_COMPARE(rows(preview->model()),
                 (QStringList{QStringLiteral("Beta"), QStringLiteral("Alpha")}));
    // Every shipped view is there to copy -- the artist tree too, with the
    // levels that say what it does -- but not to change.
    list->setCurrentRow(0);
    QCOMPARE(list->currentItem()->text(), QStringLiteral("Artist › Album"));
    QVERIFY(dialog->findChild<QLineEdit*>(QStringLiteral("library-view-name"))->isReadOnly());
    QCOMPARE(levels->rowCount(), 2);
    auto* copy = dialog->findChild<QPushButton*>(QStringLiteral("library-views-copy"));
    QVERIFY(copy != nullptr && copy->isEnabled());
    // Previewing it shows the artist tree, and does not choose it.
    QTRY_COMPARE(rows(preview->model()),
                 (QStringList{QStringLiteral("Alpha"), QStringLiteral("Beta")}));
    QCOMPARE(QSettings{}.value(QStringLiteral("library/view")).toString(), QStringLiteral("mine"));
    copy->click();
    QCOMPARE(list->currentItem()->text(), QStringLiteral("Artist › Album (copy)"));
    QVERIFY(!dialog->findChild<QLineEdit*>(QStringLiteral("library-view-name"))->isReadOnly());
    QCOMPARE(levels->rowCount(), 2);
    const auto folders_row = list->findItems(QStringLiteral("Folders"), Qt::MatchExactly);
    QCOMPARE(folders_row.size(), 1);
    list->setCurrentItem(folders_row.front());
    QVERIFY(!copy->isEnabled());
    dialog->close();

    // Folders: the engine's, from the library's root down.
    const auto folders = choice->findData(folders_library_view_id);
    QVERIFY(folders >= 0);
    choice->setCurrentIndex(folders);
    emit choice->activated(folders);
    QTRY_COMPARE(model->rowCount(), 1);
    const auto root = model->index(0, 0);
    tree->expand(root);
    QTRY_COMPARE(rows(model, root), (QStringList{QStringLiteral("one"), QStringLiteral("two")}));
    QCOMPARE(row(model, QStringLiteral("one"), root).data(library_kind_role).toString(),
             QStringLiteral("album"));
    resolved.clear();
    panel.resolveEntries(
        LibraryBrowser::selectedEntries({root}),
        [&resolved](std::vector<std::string> paths) { resolved = std::move(paths); });
    QTRY_COMPARE(resolved.size(), std::size_t{3});

    // ADR-0273: locating finds the file in the layout shown, and keeps it.
    // In Folders, its folder under the root.
    panel.locatePath(jazz_and_rock, true);
    QTRY_COMPARE(tree->currentIndex().data().toString(), QStringLiteral("one"));
    QCOMPARE(tree->currentIndex().parent().data().toString(), model->index(0, 0).data().toString());
    QCOMPARE(choice->currentData().toString(), folders_library_view_id);
    QCOMPARE(QSettings{}.value(QStringLiteral("library/view")).toString(), folders_library_view_id);

    // In a view, through its levels as the engine found them: the artist at
    // the level that names it, under the file's first genre.
    choice->setCurrentIndex(genres);
    emit choice->activated(genres);
    QTRY_COMPARE(rows(model).size(), 3);
    panel.locatePath(jazz_and_rock, false);
    QTRY_COMPARE(tree->currentIndex().data().toString(), QStringLiteral("Alpha"));
    QCOMPARE(tree->currentIndex().parent().data().toString(), QStringLiteral("Jazz"));
    QCOMPARE(choice->currentData().toString(), QStringLiteral("genre-artist-album"));
    // The album at the last level.
    panel.locatePath(jazz_and_rock, true);
    QTRY_COMPARE(tree->currentIndex().data().toString(), QStringLiteral("One"));
    QCOMPARE(tree->currentIndex().parent().data().toString(), QStringLiteral("Alpha"));
    QCOMPARE(tree->currentIndex().parent().parent().data().toString(), QStringLiteral("Jazz"));

    // Recently added: the album, whichever is asked for.
    const auto recent = choice->findData(recent_library_view_id);
    choice->setCurrentIndex(recent);
    emit choice->activated(recent);
    QTRY_COMPARE(rows(model).size(), 2);
    panel.locatePath(jazz_and_rock, false);
    QTRY_VERIFY(tree->currentIndex().data().toString().contains(QStringLiteral("One")));
    QCOMPARE(choice->currentData().toString(), recent_library_view_id);

    // The artist tree as ever: the artist, the album under it.
    const auto artists = choice->findData(default_library_view_id);
    choice->setCurrentIndex(artists);
    emit choice->activated(artists);
    QTRY_COMPARE(rows(model), (QStringList{QStringLiteral("Alpha"), QStringLiteral("Beta")}));
    panel.locatePath(jazz_and_rock, true);
    QTRY_COMPARE(tree->currentIndex().data().toString(), QStringLiteral("One"));
    QCOMPARE(tree->currentIndex().parent().data().toString(), QStringLiteral("Alpha"));

    (*server)->stop();
}

// The desktop says dark only after the window is up -- through its portal,
// a moment after start -- and the colour scheme follows with a palette of
// its own. The footer's status took it on too; under a style sheet it had
// kept the palette it was first shown with: dark text on the dark ground.
void LibraryViewsPanelTest::theFooterFollowsALaterPalette() {
    const auto before = QGuiApplication::palette();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_engine_socket_key),
                         directory.path() + QStringLiteral("/no-engine.sock"));
    const std::filesystem::path unused{
        (directory.path() + QStringLiteral("/unused.sqlite3")).toStdString()};
    CatalogueSource catalogues{unused, CatalogueSource::Role::remote};
    LocalLibraryPanel panel{catalogues};
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    auto* status = panel.findChild<QLabel*>(QStringLiteral("local-library-status"));
    QVERIFY(status != nullptr);

    auto later = before;
    const QColor quiet{0x9a, 0xa0, 0xa8};
    later.setColor(QPalette::Window, QColor{0x1e, 0x1f, 0x22});
    later.setColor(QPalette::PlaceholderText, quiet);
    QGuiApplication::setPalette(later);
    QTRY_COMPARE(status->palette().color(QPalette::PlaceholderText), quiet);
    QGuiApplication::setPalette(before);
}

} // namespace trackknife::bench

QTEST_MAIN(trackknife::bench::LibraryViewsPanelTest)
#include "library_views_panel_test.moc"
