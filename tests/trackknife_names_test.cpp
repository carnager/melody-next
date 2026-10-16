// SPDX-License-Identifier: GPL-3.0-only

// ADR-0287: the window's settings, data and cache move from Trackknife's
// names to Melody's, once; the engine's files stay for the engine, and a
// rating backup copy on under the old default keeps its name.

#include "bench/settings_keys.hpp"
#include "workspace/startup.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace trackknife::bench {

class TrackknifeNamesTest final : public QObject {
    Q_OBJECT

  private slots:
    void movesTheWindowsOwnOnce();
    void aWindowUnderTestNeverTakesTheRealEngineFolder();
};

namespace {

void touch(const QString& path) {
    QDir{}.mkpath(QFileInfo{path}.absolutePath());
    QFile file{path};
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("x");
}

} // namespace

void TrackknifeNamesTest::movesTheWindowsOwnOnce() {
    QTemporaryDir home;
    QVERIFY(home.isValid());
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(home.filePath(QStringLiteral("config"))));
    qputenv("XDG_DATA_HOME", QFile::encodeName(home.filePath(QStringLiteral("data"))));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(home.filePath(QStringLiteral("cache"))));
    QCoreApplication::setOrganizationName(QStringLiteral("melody"));
    QCoreApplication::setApplicationName(QStringLiteral("melody"));

    // Trackknife's settings: a rating backup copy on, its name the default.
    const auto old_settings = home.filePath(QStringLiteral("config/trackknife/trackknife.conf"));
    {
        QSettings old{old_settings, QSettings::NativeFormat};
        old.setValue(QLatin1String(SettingsKeys::rating_backup_key), true);
        old.setValue(QStringLiteral("library/engine"), QStringLiteral("192.168.0.200:6603"));
    }
    const auto old_data = home.filePath(QStringLiteral("data/trackknife/trackknife"));
    touch(old_data + QStringLiteral("/tabs/one.json"));
    touch(old_data + QStringLiteral("/before-main-restore-1/lists.sqlite"));
    touch(old_data + QStringLiteral("/lists.sqlite"));
    touch(home.filePath(QStringLiteral("cache/trackknife/trackknife/covers/a.jpg")));

    adoptTrackknifeNames();

    QSettings settings;
    QCOMPARE(QDir::cleanPath(settings.fileName()),
             home.filePath(QStringLiteral("config/melody/melody.conf")));
    QCOMPARE(settings.value(QStringLiteral("library/engine")).toString(),
             QStringLiteral("192.168.0.200:6603"));
    // Kept under the name it had, so no file is written again for it.
    QCOMPARE(settings.value(QLatin1String(SettingsKeys::rating_backup_tag_key)).toString(),
             QStringLiteral("TRACKKNIFE_RATING"));
    QVERIFY(!QFile::exists(old_settings));

    const auto data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QCOMPARE(data, home.filePath(QStringLiteral("data/melody/melody")));
    QVERIFY(QFile::exists(data + QStringLiteral("/tabs/one.json")));
    QVERIFY(QFile::exists(data + QStringLiteral("/before-main-restore-1/lists.sqlite")));
    // The engine's database is the engine's to move.
    QVERIFY(QFile::exists(old_data + QStringLiteral("/lists.sqlite")));
    QVERIFY(!QFile::exists(data + QStringLiteral("/lists.sqlite")));
    QVERIFY(QFile::exists(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
                          QStringLiteral("/covers/a.jpg")));

    // Once: what is left under the old names stays there.
    touch(old_data + QStringLiteral("/tabs/two.json"));
    adoptTrackknifeNames();
    QVERIFY(!QFile::exists(data + QStringLiteral("/tabs/two.json")));
}

// A window in test mode with no engine folder named -- the benchmark once --
// moved a user's library out of Trackknife's folder while their engine was
// down. It keeps its engine in its own test folder instead.
void TrackknifeNamesTest::aWindowUnderTestNeverTakesTheRealEngineFolder() {
    QTemporaryDir home;
    QVERIFY(home.isValid());
    qputenv("XDG_DATA_HOME", QFile::encodeName(home.filePath(QStringLiteral("data"))));
    qunsetenv("MELODY_STATE_DIR");
    QStandardPaths::setTestModeEnabled(true);
    isolateTestEngineState();
    const auto named = qEnvironmentVariable("MELODY_STATE_DIR");
    QCOMPARE(named, QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    QVERIFY(!named.startsWith(home.path()));
    // One named is kept.
    qputenv("MELODY_STATE_DIR", "/somewhere/chosen");
    isolateTestEngineState();
    QCOMPARE(qEnvironmentVariable("MELODY_STATE_DIR"), QStringLiteral("/somewhere/chosen"));
    qunsetenv("MELODY_STATE_DIR");
    QStandardPaths::setTestModeEnabled(false);
}

} // namespace trackknife::bench

QTEST_GUILESS_MAIN(trackknife::bench::TrackknifeNamesTest)
#include "trackknife_names_test.moc"
