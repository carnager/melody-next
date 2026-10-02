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
    panel.resolveEntries(LibraryBrowser::selectedEntries({jazz_one}),
                         [&resolved](std::vector<std::string> paths) { resolved = std::move(paths); });
    QTRY_COMPARE(resolved, std::vector<std::string>{jazz_and_rock});
    resolved.clear();
    panel.resolveEntries(LibraryBrowser::selectedEntries({rock}),
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
    LibraryViewDefinition by_album{.id = QStringLiteral("mine"),
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
    // Shipped views are looked at, not changed.
    list->setCurrentRow(0);
    QVERIFY(dialog->findChild<QLineEdit*>(QStringLiteral("library-view-name"))->isReadOnly());
    dialog->close();

    // Go to album finds the file under its artist, in the artist tree.
    panel.locatePath(jazz_and_rock, true);
    QTRY_COMPARE(choice->currentData().toString(), default_library_view_id);
    QTRY_COMPARE(rows(model), (QStringList{QStringLiteral("Alpha"), QStringLiteral("Beta")}));

    (*server)->stop();
}

} // namespace trackknife::bench

QTEST_MAIN(trackknife::bench::LibraryViewsPanelTest)
#include "library_views_panel_test.moc"
