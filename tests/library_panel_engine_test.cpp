// SPDX-License-Identifier: GPL-3.0-only

// ADR-0220: the library panel pointed at an engine instead of a database.
// This is the first test of the running application's behaviour changing --
// everything below it proved the parts, and this proves the setting actually
// reroutes the panel and that an unreachable engine does not cost the user
// their library.

#include "bench/catalogue_source.hpp"
#include "bench/local_library_panel.hpp"
#include "bench/search_dialog.hpp"
#include "bench/settings_dialog.hpp"
#include "trackknife/engine/catalogue_methods.hpp"
#include "trackknife/engine/job_methods.hpp"
#include "trackknife/engine/server.hpp"

#include <QAbstractButton>
#include <QLabel>
#include <QLineEdit>
#include <QSettings>
#include <QtTest>
#include <atomic>

#include <filesystem>

namespace trackknife::bench {

namespace engine = trackknife::engine;
namespace protocol = trackknife::protocol;

class LibraryPanelEngineTest final : public QObject {
    Q_OBJECT

  private slots:
    void init();
    void aConfiguredEngineServesThePanel();
    void everyPathReachesTheEngineNotTheDatabase();
    void anUnreachableEngineFallsBackAndSaysSo();
    void thePanelSaysWhichLibraryItIsShowing();
    void theSearchDialogAsksTheEngineToo();
};

void LibraryPanelEngineTest::init() {
    QSettings{}.remove(QLatin1String(SettingsDialog::library_engine_socket_key));
}

void LibraryPanelEngineTest::aConfiguredEngineServesThePanel() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const std::filesystem::path database{
        (directory.path() + QStringLiteral("/library.sqlite3")).toStdString()};
    const std::filesystem::path socket{
        (directory.path() + QStringLiteral("/engine.sock")).toStdString()};

    engine::LocalCatalogue catalogue{database};
    QVERIFY(catalogue.prepare().has_value());
    protocol::Dispatcher dispatcher;
    engine::register_catalogue_methods(dispatcher, catalogue);
    auto server = engine::Server::listen(socket, dispatcher);
    QVERIFY(server.has_value());
    (*server)->start();

    QSettings{}.setValue(QLatin1String(SettingsDialog::library_engine_socket_key),
                         QString::fromStdString(socket.string()));

    // The panel is given a database path it must not use: if it opens that
    // instead of asking the engine, the connection count below stays zero.
    const std::filesystem::path unused{
        (directory.path() + QStringLiteral("/never-opened.sqlite3")).toStdString()};
    CatalogueSource catalogues{unused};
    LocalLibraryPanel panel{catalogues};
    panel.show();

    QTRY_COMPARE((*server)->connections(), std::size_t{1});
    // And the file it was told not to use was not created behind its back.
    QVERIFY(!std::filesystem::exists(unused));

    (*server)->stop();
}

// The panel has three independent paths to a catalogue -- the query pool, the
// scan, and the artwork pool -- and each was written separately. Routing two
// of them and forgetting the third is exactly the bug this catches: the folder
// went to the engine, the scan ran against the local database, and the tree
// then read back an empty remote library. Nothing looked broken; there was
// simply no music.
void LibraryPanelEngineTest::everyPathReachesTheEngineNotTheDatabase() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const std::filesystem::path engine_database{
        (directory.path() + QStringLiteral("/engine.sqlite3")).toStdString()};
    const std::filesystem::path socket{
        (directory.path() + QStringLiteral("/engine.sock")).toStdString()};
    // The panel is given a path it must never open. If any path falls through
    // to the local implementation, this file appears.
    const std::filesystem::path forbidden{
        (directory.path() + QStringLiteral("/must-not-exist.sqlite3")).toStdString()};

    engine::LocalCatalogue catalogue{engine_database};
    QVERIFY(catalogue.prepare().has_value());
    protocol::Dispatcher dispatcher;
    engine::register_catalogue_methods(dispatcher, catalogue);
    auto server = engine::Server::listen(socket, dispatcher);
    QVERIFY(server.has_value());
    engine::JobRegistry jobs{(*server)->sink()};
    engine::JobCatalog job_catalogue;
    engine::register_catalogue_jobs(job_catalogue, catalogue);
    engine::register_job_methods(dispatcher, jobs, job_catalogue);
    (*server)->start();

    QSettings{}.setValue(QLatin1String(SettingsDialog::library_engine_socket_key),
                         QString::fromStdString(socket.string()));

    CatalogueSource catalogues{forbidden};
    LocalLibraryPanel panel{catalogues};
    panel.show();
    QTRY_COMPARE((*server)->connections(), std::size_t{1});

    // A scan is the path that was missed. Driven through the button, the way
    // a user reaches it.
    auto* refresh = panel.findChild<QAbstractButton*>(QStringLiteral("local-library-scan"));
    QVERIFY2(refresh != nullptr, "the panel must offer a scan control");
    refresh->click();
    QTRY_VERIFY_WITH_TIMEOUT(!panel.property("scanning").toBool(), 15000);

    // Artwork is the third path; asking the tree to paint exercises it.
    panel.repaint();
    QTest::qWait(50);

    QVERIFY2(!std::filesystem::exists(forbidden),
             "a path fell through to the local database instead of asking the engine");

    (*server)->stop();
}

void LibraryPanelEngineTest::anUnreachableEngineFallsBackAndSaysSo() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const std::filesystem::path database{
        (directory.path() + QStringLiteral("/library.sqlite3")).toStdString()};
    const auto absent = directory.path() + QStringLiteral("/no-engine.sock");

    QSettings{}.setValue(QLatin1String(SettingsDialog::library_engine_socket_key), absent);

    CatalogueSource catalogues{database};
    LocalLibraryPanel panel{catalogues};
    panel.show();

    // An engine that is not there costs the user the engine, not the library:
    // the panel still works, and says why it is not using what was asked for.
    auto* status = panel.findChild<QLabel*>(QStringLiteral("local-library-status"));
    QVERIFY(status != nullptr);
    QTRY_VERIFY(status->text().contains(QStringLiteral("unreachable")));
    QVERIFY(status->text().contains(QStringLiteral("local library")));
}

// ADR-0220: a silent fallback is indistinguishable from the engine working,
// which is exactly how a broken setup went unnoticed. The panel now states
// which of the three situations it is in, and keeps stating it.
void LibraryPanelEngineTest::thePanelSaysWhichLibraryItIsShowing() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const std::filesystem::path database{
        (directory.path() + QStringLiteral("/library.sqlite3")).toStdString()};

    // No engine configured.
    {
        CatalogueSource catalogues{database};
        LocalLibraryPanel panel{catalogues};
        panel.show();
        auto* source = panel.findChild<QLabel*>(QStringLiteral("local-library-source"));
        QVERIFY(source != nullptr);
        QVERIFY(source->text().contains(QStringLiteral("this process")));
        QVERIFY(!source->text().contains(QStringLiteral("unreachable")));
    }

    // Configured but not answering: the distinction that was invisible.
    {
        QSettings{}.setValue(QLatin1String(SettingsDialog::library_engine_socket_key),
                             directory.path() + QStringLiteral("/absent.sock"));
        CatalogueSource catalogues{database};
        LocalLibraryPanel panel{catalogues};
        panel.show();
        auto* source = panel.findChild<QLabel*>(QStringLiteral("local-library-source"));
        QVERIFY(source != nullptr);
        QVERIFY(source->text().contains(QStringLiteral("this process")));
        QVERIFY(source->text().contains(QStringLiteral("unreachable")));
    }

    // Answering.
    {
        const std::filesystem::path socket{
            (directory.path() + QStringLiteral("/engine.sock")).toStdString()};
        engine::LocalCatalogue catalogue{database};
        QVERIFY(catalogue.prepare().has_value());
        protocol::Dispatcher dispatcher;
        engine::register_catalogue_methods(dispatcher, catalogue);
        auto server = engine::Server::listen(socket, dispatcher);
        QVERIFY(server.has_value());
        (*server)->start();

        QSettings{}.setValue(QLatin1String(SettingsDialog::library_engine_socket_key),
                             QString::fromStdString(socket.string()));
        CatalogueSource catalogues{database};
        LocalLibraryPanel panel{catalogues};
        panel.show();
        auto* source = panel.findChild<QLabel*>(QStringLiteral("local-library-source"));
        QVERIFY(source != nullptr);
        QVERIFY(source->text().contains(QStringLiteral("engine at")));
        QVERIFY(!source->text().contains(QStringLiteral("unreachable")));
        // And it must not be mistaken for the no-engine case.
        QVERIFY(!source->text().contains(QStringLiteral("this process")));

        (*server)->stop();
    }
}

// The search dialog is a separate surface with its own catalogue access, and
// it stayed local after the panel was routed -- which is why searching an
// engine-backed library returned zero matches while browsing it worked. One
// CatalogueSource is what stops a fourth surface repeating it.
//
// Asserted by watching the engine receive the search rather than by watching
// a file not appear: the workspace and the catalogue share one database file,
// so loading saved searches touches it legitimately.
void LibraryPanelEngineTest::theSearchDialogAsksTheEngineToo() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const std::filesystem::path engine_database{
        (directory.path() + QStringLiteral("/engine.sqlite3")).toStdString()};
    const std::filesystem::path socket{
        (directory.path() + QStringLiteral("/engine.sock")).toStdString()};
    const std::filesystem::path workspace{
        (directory.path() + QStringLiteral("/workspace.sqlite3")).toStdString()};

    engine::LocalCatalogue catalogue{engine_database};
    QVERIFY(catalogue.prepare().has_value());

    protocol::Dispatcher dispatcher;
    engine::register_catalogue_methods(dispatcher, catalogue);
    // Wrap the method the search runs, so the engine can say whether it was
    // asked. Registering again replaces the handler.
    std::atomic_int searched{0};
    dispatcher.on("catalogue.filter_paths",
                  [&](const protocol::Json& params) -> core::Result<protocol::Json> {
                      searched.fetch_add(1);
                      static_cast<void>(params);
                      return protocol::Json{{"paths", protocol::Json::array()}};
                  });

    auto server = engine::Server::listen(socket, dispatcher);
    QVERIFY(server.has_value());
    (*server)->start();

    QSettings{}.setValue(QLatin1String(SettingsDialog::library_engine_socket_key),
                         QString::fromStdString(socket.string()));

    CatalogueSource catalogues{workspace};
    QVERIFY(catalogues.usingEngine());
    SearchDialog dialog{catalogues, {}, {}};
    dialog.show();

    auto* input = dialog.findChild<QLineEdit*>(QStringLiteral("bench-search-input"));
    QVERIFY(input != nullptr);
    input->setText(QStringLiteral("anything"));

    QTRY_VERIFY2(searched.load() > 0,
                 "the search dialog never asked the engine; it searched locally");

    (*server)->stop();
}

} // namespace trackknife::bench

QTEST_MAIN(trackknife::bench::LibraryPanelEngineTest)
#include "library_panel_engine_test.moc"
