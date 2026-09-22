// SPDX-License-Identifier: GPL-3.0-only

// ADR-0220: the library panel pointed at an engine instead of a database.
// This is the first test of the running application's behaviour changing --
// everything below it proved the parts, and this proves the setting actually
// reroutes the panel and that an unreachable engine does not cost the user
// their library.

#include "bench/local_library_panel.hpp"
#include "bench/settings_dialog.hpp"
#include "trackknife/engine/catalogue_methods.hpp"
#include "trackknife/engine/server.hpp"

#include <QLabel>
#include <QSettings>
#include <QtTest>

#include <filesystem>

namespace trackknife::bench {

namespace engine = trackknife::engine;
namespace protocol = trackknife::protocol;

class LibraryPanelEngineTest final : public QObject {
    Q_OBJECT

  private slots:
    void init();
    void aConfiguredEngineServesThePanel();
    void anUnreachableEngineFallsBackAndSaysSo();
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
    LocalLibraryPanel panel{unused};
    panel.show();

    QTRY_COMPARE((*server)->connections(), std::size_t{1});
    // And the file it was told not to use was not created behind its back.
    QVERIFY(!std::filesystem::exists(unused));

    (*server)->stop();
}

void LibraryPanelEngineTest::anUnreachableEngineFallsBackAndSaysSo() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const std::filesystem::path database{
        (directory.path() + QStringLiteral("/library.sqlite3")).toStdString()};
    const auto absent = directory.path() + QStringLiteral("/no-engine.sock");

    QSettings{}.setValue(QLatin1String(SettingsDialog::library_engine_socket_key), absent);

    LocalLibraryPanel panel{database};
    panel.show();

    // An engine that is not there costs the user the engine, not the library:
    // the panel still works, and says why it is not using what was asked for.
    auto* status = panel.findChild<QLabel*>(QStringLiteral("local-library-status"));
    QVERIFY(status != nullptr);
    QTRY_VERIFY(status->text().contains(QStringLiteral("unreachable")));
    QVERIFY(status->text().contains(QStringLiteral("local library")));
}

} // namespace trackknife::bench

QTEST_MAIN(trackknife::bench::LibraryPanelEngineTest)
#include "library_panel_engine_test.moc"
