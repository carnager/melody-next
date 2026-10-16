// SPDX-License-Identifier: GPL-3.0-only

// ADR-0288: plugins as actions -- their manifest, what they are told, how
// they are run, and that they stay off until enabled.

#include "workspace/plugins.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace trackknife::bench {

class PluginsTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
    void manifestsAreReadOrRefused();
    void tracksAreToldAsAlbums();
    void anActionRunsAndSaysWhatItGot();
    void pluginsAreFoundAndStayOffUntilEnabled();
    void theExamplePluginReads();
};

namespace {

[[nodiscard]] QByteArray manifest(const QString& action) {
    return QStringLiteral(R"({"schema": 1, "name": "Album share", "description": "A link",
                             "actions": [%1]})")
        .arg(action)
        .toUtf8();
}

void writeFile(const QString& path, const QByteArray& content, const bool executable = false) {
    QDir{}.mkpath(QFileInfo{path}.absolutePath());
    QFile file{path};
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(content);
    file.close();
    if (executable) {
        file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
}

[[nodiscard]] PluginOutcome run(const Plugin& plugin, const PluginAction& action,
                                const QByteArray& input) {
    std::optional<PluginOutcome> outcome;
    QObject context;
    runPluginAction(plugin, action, input, &context,
                    [&outcome](PluginOutcome done) { outcome = std::move(done); });
    if (!QTest::qWaitFor([&outcome] { return outcome.has_value(); }, 10'000)) {
        return PluginOutcome{.succeeded = false, .text = QStringLiteral("never answered")};
    }
    return *outcome;
}

} // namespace

void PluginsTest::initTestCase() {
    QCoreApplication::setOrganizationName(QStringLiteral("MelodyTests"));
    QCoreApplication::setApplicationName(QStringLiteral("plugins-test"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
}

void PluginsTest::manifestsAreReadOrRefused() {
    QString problem;
    const auto plugin =
        parsePluginManifest(manifest(R"({"id": "share", "label": "Share album", "for": "albums",
                     "run": ["./share", "--quiet"], "result": "notify-and-copy", "timeout": 300})"),
                            QStringLiteral("/plugins/albumshare"), &problem);
    QVERIFY2(plugin.has_value(), qPrintable(problem));
    QCOMPARE(plugin->name, QStringLiteral("albumshare"));
    QCOMPARE(plugin->title, QStringLiteral("Album share"));
    QCOMPARE(plugin->actions.size(), std::size_t{1U});
    const auto& action = plugin->actions.front();
    QCOMPARE(action.label, QStringLiteral("Share album"));
    QVERIFY(action.scope == PluginAction::Scope::albums);
    QCOMPARE(action.run, (QStringList{QStringLiteral("./share"), QStringLiteral("--quiet")}));
    QVERIFY(action.result == PluginAction::Result::notify_and_copy);
    QCOMPARE(action.timeout_seconds, 300);

    // Defaults: an album at a time, the result as a notification, two minutes.
    const auto plain = parsePluginManifest(manifest(R"({"id": "a", "label": "A", "run": ["a"]})"),
                                           QStringLiteral("/p/x"), &problem);
    QVERIFY(plain && plain->actions.front().scope == PluginAction::Scope::albums &&
            plain->actions.front().result == PluginAction::Result::notify &&
            plain->actions.front().timeout_seconds == 120);

    // Said, not guessed at.
    const auto refused = [&problem](const QByteArray& json) {
        problem.clear();
        return !parsePluginManifest(json, QStringLiteral("/p/x"), &problem) && !problem.isEmpty();
    };
    QVERIFY(refused("not json"));
    QVERIFY(refused(R"({"schema": 2, "name": "A", "actions": []})"));
    QVERIFY(refused(R"({"schema": 1, "name": "A", "actions": [], "colour": "red"})"));
    QVERIFY(refused(manifest(R"({"id": "a", "label": "A", "run": []})")));
    QVERIFY(refused(manifest(R"({"id": "a", "label": "A", "run": ["a"], "for": "artists"})")));
    QVERIFY(refused(manifest(R"({"id": "a", "label": "A", "run": ["a"], "result": "beep"})")));
    QVERIFY(refused(manifest(R"({"id": "a", "label": "A", "run": ["a"], "timeout": 0})")));
    QVERIFY(refused(manifest(R"({"id": "a", "label": "A", "run": ["a"], "shell": true})")));
    QVERIFY(problem.contains(QStringLiteral("shell")));
}

void PluginsTest::tracksAreToldAsAlbums() {
    const std::vector<std::string> folders{"/srv/music"};
    const auto track = [](std::string path, std::string album, std::string title) {
        return PluginTrack{.path = std::move(path),
                           .title = std::move(title),
                           .artist = "Air Supply",
                           .album = std::move(album),
                           .album_artist = "Air Supply",
                           .date = "1983",
                           .track_number = "3"};
    };
    auto albums =
        pluginAlbums({track("/srv/music/Air Supply/Making Love/03.mp3", "Making Love", "The One"),
                      track("/srv/music/Air Supply/Making Love/CD2/04.mp3", "Making Love", "Lost"),
                      track("/srv/music/Air Supply/Lost/01.mp3", "Lost in Love", "Lost in Love"),
                      track("/elsewhere/loose.flac", "", "Loose")},
                     folders);
    QCOMPARE(albums.size(), std::size_t{3U});
    // Its folder: the one every track of it is in, and that within the music
    // folder -- what albumshare calls common_dir.
    QCOMPARE(albums[0].folder, std::string{"/srv/music/Air Supply/Making Love"});
    QCOMPARE(albums[0].music_folder, std::string{"/srv/music"});
    QCOMPARE(albums[0].relative_folder, std::string{"Air Supply/Making Love"});
    QCOMPARE(albums[0].tracks.size(), std::size_t{2U});
    QCOMPARE(albums[1].album, std::string{"Lost in Love"});
    // Outside the music folders: no relative folder.
    QCOMPARE(albums[2].relative_folder, std::string{});

    const PluginAction action{.id = QStringLiteral("share"),
                              .label = QStringLiteral("Share"),
                              .scope = PluginAction::Scope::albums,
                              .run = {QStringLiteral("x")},
                              .result = PluginAction::Result::notify,
                              .timeout_seconds = 5};
    const auto input = QJsonDocument::fromJson(
        pluginInput(action,
                    PluginEngine{.name = QStringLiteral("gemenon"),
                                 .address = QStringLiteral("192.168.0.200:6603"),
                                 .here = false},
                    {albums[0]}, folders));
    const auto root = input.object();
    QCOMPARE(root.value(QStringLiteral("schema")).toInt(), 1);
    QCOMPARE(root.value(QStringLiteral("action")).toString(), QStringLiteral("share"));
    const auto engine = root.value(QStringLiteral("engine")).toObject();
    QCOMPARE(engine.value(QStringLiteral("name")).toString(), QStringLiteral("gemenon"));
    QCOMPARE(engine.value(QStringLiteral("address")).toString(),
             QStringLiteral("192.168.0.200:6603"));
    QCOMPARE(engine.value(QStringLiteral("here")).toBool(true), false);
    const auto album = root.value(QStringLiteral("albums")).toArray().first().toObject();
    QCOMPARE(album.value(QStringLiteral("artist")).toString(), QStringLiteral("Air Supply"));
    QCOMPARE(album.value(QStringLiteral("album")).toString(), QStringLiteral("Making Love"));
    QCOMPARE(album.value(QStringLiteral("date")).toString(), QStringLiteral("1983"));
    QCOMPARE(album.value(QStringLiteral("relative_folder")).toString(),
             QStringLiteral("Air Supply/Making Love"));
    const auto first = album.value(QStringLiteral("tracks")).toArray().first().toObject();
    QCOMPARE(first.value(QStringLiteral("relative_path")).toString(),
             QStringLiteral("Air Supply/Making Love/03.mp3"));
    QCOMPARE(first.value(QStringLiteral("title")).toString(), QStringLiteral("The One"));
}

void PluginsTest::anActionRunsAndSaysWhatItGot() {
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    // It reads the input and answers with the album's folder within the
    // music folder; a word given makes it fail, another makes it wait.
    writeFile(folder.filePath(QStringLiteral("share")), R"sh(#!/bin/sh
input=$(cat)
case "$1" in
  fail) echo "no server configured" >&2; exit 3 ;;
  wait) sleep 5 ;;
esac
printf '%s\n' "$input" | sed -n 's/.*"relative_folder":"\([^"]*\)".*/https:\/\/example.org\/\1/p'
)sh",
              true);
    Plugin plugin{.name = QStringLiteral("albumshare"),
                  .title = QStringLiteral("Album share"),
                  .description = {},
                  .folder = folder.path(),
                  .actions = {}};
    PluginAction action{.id = QStringLiteral("share"),
                        .label = QStringLiteral("Share"),
                        .scope = PluginAction::Scope::albums,
                        .run = {QStringLiteral("./share")},
                        .result = PluginAction::Result::notify,
                        .timeout_seconds = 5};
    const auto albums = pluginAlbums({PluginTrack{.path = "/srv/music/A/B/01.flac",
                                                  .title = "T",
                                                  .artist = "A",
                                                  .album = "B",
                                                  .album_artist = "A",
                                                  .date = "2000",
                                                  .track_number = "1"}},
                                     {"/srv/music"});
    const auto input = pluginInput(action, PluginEngine{}, albums, {"/srv/music"});

    const auto shared = run(plugin, action, input);
    QVERIFY2(shared.succeeded, qPrintable(shared.text));
    QCOMPARE(shared.text, QStringLiteral("https://example.org/A/B"));

    // A failure says what it said on stderr.
    action.run = {QStringLiteral("./share"), QStringLiteral("fail")};
    const auto failed = run(plugin, action, input);
    QVERIFY(!failed.succeeded);
    QCOMPARE(failed.text, QStringLiteral("no server configured"));

    // Its time limit stops it.
    action.run = {QStringLiteral("./share"), QStringLiteral("wait")};
    action.timeout_seconds = 1;
    const auto stopped = run(plugin, action, input);
    QVERIFY(!stopped.succeeded);
    QVERIFY2(stopped.text.contains(QStringLiteral("stopped after 1 second")),
             qPrintable(stopped.text));

    // A program not there is said.
    action.run = {QStringLiteral("./missing")};
    action.timeout_seconds = 5;
    const auto missing = run(plugin, action, input);
    QVERIFY(!missing.succeeded && missing.text.contains(QStringLiteral("could not start")));
}

void PluginsTest::pluginsAreFoundAndStayOffUntilEnabled() {
    QTemporaryDir data;
    QVERIFY(data.isValid());
    qputenv("XDG_DATA_HOME", QFile::encodeName(data.path()));
    const auto base = data.filePath(QStringLiteral("melody/plugins"));
    writeFile(base + QStringLiteral("/albumshare/plugin.json"),
              manifest(R"({"id": "share", "label": "Share album", "run": ["./share"]})"));
    writeFile(base + QStringLiteral("/broken/plugin.json"), "{");
    QCOMPARE(pluginFolders().value(0), base);

    const auto found = discoverPlugins();
    QCOMPARE(found.size(), std::size_t{2U});
    QCOMPARE(found[0].name, QStringLiteral("albumshare"));
    QVERIFY(found[0].plugin.has_value());
    QCOMPARE(found[1].name, QStringLiteral("broken"));
    QVERIFY(!found[1].plugin && !found[1].problem.isEmpty());

    // Off until enabled; a broken one never is.
    QSettings{}.remove(QStringLiteral("plugins"));
    QVERIFY(enabledPlugins().empty());
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("albumshare")), true);
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("broken")), true);
    const auto enabled = enabledPlugins();
    QCOMPARE(enabled.size(), std::size_t{1U});
    QCOMPARE(enabled.front().name, QStringLiteral("albumshare"));
    QSettings{}.remove(QStringLiteral("plugins"));
}

// The example plugin docs/plugins.md starts from is one Melody reads.
void PluginsTest::theExamplePluginReads() {
    const QString folder = QStringLiteral(MELODY_SOURCE_DIR "/examples/plugins/copy-folder");
    QFile manifest{folder + QStringLiteral("/plugin.json")};
    QVERIFY(manifest.open(QIODevice::ReadOnly));
    QString problem;
    const auto plugin = parsePluginManifest(manifest.readAll(), folder, &problem);
    QVERIFY2(plugin.has_value(), qPrintable(problem));
    QCOMPARE(plugin->actions.size(), std::size_t{1U});
    QVERIFY(plugin->actions.front().result == PluginAction::Result::copy);
    // And it answers as the guide says.
    const auto albums = pluginAlbums({PluginTrack{.path = "/srv/music/A/B/01.flac",
                                                  .title = {},
                                                  .artist = {},
                                                  .album = "B",
                                                  .album_artist = "A",
                                                  .date = {},
                                                  .track_number = {}}},
                                     {"/srv/music"});
    const auto answered =
        run(*plugin, plugin->actions.front(),
            pluginInput(plugin->actions.front(), PluginEngine{}, albums, {"/srv/music"}));
    QVERIFY2(answered.succeeded, qPrintable(answered.text));
    QCOMPARE(answered.text, QStringLiteral("A/B"));
}

} // namespace trackknife::bench

QTEST_GUILESS_MAIN(trackknife::bench::PluginsTest)
#include "plugins_test.moc"
