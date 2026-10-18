// SPDX-License-Identifier: GPL-3.0-only

// ADR-0288: plugins as actions -- their manifest, what they are told, how
// they are run, and that they stay off until enabled.

#include "workspace/plugins.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
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
    void aListIsReadOrRefused();
    void settingsAreReadKeptAndHandedOn();
    void panelsAreReadAndTheirAnswersToo();
    void providersAreReadAndTheirLyricsToo();

  private:
    // Settings of this test's own, not in the config folder of whoever runs it.
    QTemporaryDir settings_home_;
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
    QVERIFY(settings_home_.isValid());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_home_.path());
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
                  .actions = {},
                  .panels = {},
                  .providers = {},
                  .settings = {}};
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
#ifndef __APPLE__
    // The user's data folder first, where it is XDG's.
    qunsetenv("MELODY_PLUGIN_PATH");
    qputenv("XDG_DATA_HOME", QFile::encodeName(data.path()));
    QCOMPARE(pluginFolders().value(0), data.filePath(QStringLiteral("melody/plugins")));
#endif
    // Named, those folders and no others -- none of the system's, such as an
    // installed Melody's official ones. The first's of a name wins.
    const auto base = data.filePath(QStringLiteral("mine"));
    const auto other = data.filePath(QStringLiteral("other"));
    qputenv("MELODY_PLUGIN_PATH", QFile::encodeName(base + QDir::listSeparator() + other));
    QCOMPARE(pluginFolders(), (QStringList{base, other}));
    writeFile(base + QStringLiteral("/albumshare/plugin.json"),
              manifest(R"({"id": "share", "label": "Share album", "run": ["./share"]})"));
    writeFile(base + QStringLiteral("/broken/plugin.json"), "{");
    writeFile(other + QStringLiteral("/albumshare/plugin.json"), "{");

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

    // The official web search reads too -- not run: it opens a browser.
    const QString search = QStringLiteral(MELODY_SOURCE_DIR "/plugins/web-search");
    QFile search_manifest{search + QStringLiteral("/plugin.json")};
    QVERIFY(search_manifest.open(QIODevice::ReadOnly));
    const auto sites = parsePluginManifest(search_manifest.readAll(), search, &problem);
    QVERIFY2(sites.has_value(), qPrintable(problem));
    QCOMPARE(sites->actions.size(), std::size_t{5U});
    QCOMPARE(sites->actions.back().label, QStringLiteral("AllMusic"));

    // ADR-0291: the official About panel reads, and answers what Melody
    // draws of the artist and the album -- from what it keeps, so asking
    // nothing of the network here.
    const QString about = QStringLiteral(MELODY_SOURCE_DIR "/plugins/about");
    QFile about_manifest{about + QStringLiteral("/plugin.json")};
    QVERIFY(about_manifest.open(QIODevice::ReadOnly));
    const auto about_panel = parsePluginManifest(about_manifest.readAll(), about, &problem);
    QVERIFY2(about_panel.has_value(), qPrintable(problem));
    QCOMPARE(about_panel->panels.size(), std::size_t{1U});
    QTemporaryDir cache;
    QVERIFY(cache.isValid());
    const auto kept = QDir{cache.path()}.filePath(QStringLiteral("melody/plugins/about"));
    QVERIFY(QDir{}.mkpath(kept));
    const auto mbid = QStringLiteral("05106775-5d45-4131-aecc-1177f813ba11");
    const auto group = QStringLiteral("fc164cde-a62d-3011-9ac8-b969a92b83cf");
    const auto id_key = QCryptographicHash::hash(QStringLiteral("testament\x1fthe legacy").toUtf8(),
                                                 QCryptographicHash::Sha1)
                            .toHex();
    writeFile(kept + QStringLiteral("/id-") + QString::fromLatin1(id_key),
              QStringLiteral(R"({"artist": "%1", "album": "%2"})").arg(mbid, group).toUtf8());
    writeFile(kept + QStringLiteral("/album-%1-en").arg(group),
              QStringLiteral(R"json({"title": "The Legacy", "artist": "Testament",
                  "kind": "Album", "date": "1987-04-21", "label": "Megaforce Worldwide",
                  "summary": "The Legacy is the debut studio album by Testament.",
                  "links": {"MusicBrainz": "https://musicbrainz.org/release-group/%1",
                            "Discogs": "https://www.discogs.com/master/67184"}})json")
                  .arg(group)
                  .toUtf8());
    writeFile(kept + QStringLiteral("/artist-%1-en").arg(mbid),
              QStringLiteral(R"json({"name": "Testament", "kind": "Group",
                  "where": "Berkeley, United States", "when": "since 1983",
                  "summary": "Testament is an American thrash metal band.",
                  "page": "https://en.wikipedia.org/wiki/Testament_(band)", "image": "",
                  "links": {"Wikipedia": "https://en.wikipedia.org/wiki/Testament_(band)",
                            "MusicBrainz": "https://musicbrainz.org/artist/%1"}})json")
                  .arg(mbid)
                  .toUtf8());
    qputenv("XDG_CACHE_HOME", QFile::encodeName(cache.path()));
    const auto told = run(*about_panel,
                          PluginAction{.id = QStringLiteral("artist"),
                                       .label = QStringLiteral("About"),
                                       .scope = PluginAction::Scope::tracks,
                                       .run = about_panel->panels.front().run,
                                       .result = PluginAction::Result::none,
                                       .timeout_seconds = 30},
                          pluginPanelInput(about_panel->panels.front(), PluginEngine{},
                                           PluginTrack{.path = "/srv/music/Testament/01.flac",
                                                       .title = "Over the Wall",
                                                       .artist = "Testament",
                                                       .album = "The Legacy",
                                                       .album_artist = "Testament",
                                                       .date = "1987",
                                                       .track_number = "1"},
                                           std::nullopt, {}, pluginSettings(*about_panel)));
    QVERIFY2(told.succeeded, qPrintable(told.text));
    const auto drawn = parsePluginPanel(told.text.toUtf8(), &problem);
    QVERIFY2(drawn.has_value(), qPrintable(problem));
    QCOMPARE(drawn->title, QStringLiteral("Testament"));
    QVERIFY(std::ranges::any_of(drawn->parts, [](const auto& part) {
        return part.kind == PluginPanelContent::Part::Kind::paragraph &&
               part.text == QStringLiteral("Testament is an American thrash metal band.");
    }));
    QVERIFY(std::ranges::any_of(drawn->parts, [](const auto& part) {
        return part.kind == PluginPanelContent::Part::Kind::item &&
               part.text == QStringLiteral("Group from Berkeley, United States") &&
               part.detail == QStringLiteral("since 1983");
    }));
    // The album beneath, under its name: when, what, on which label.
    const auto heading = std::ranges::find_if(drawn->parts, [](const auto& part) {
        return part.kind == PluginPanelContent::Part::Kind::heading &&
               part.text == QStringLiteral("The Legacy");
    });
    QVERIFY(heading != drawn->parts.end());
    QCOMPARE(std::next(heading)->text, QStringLiteral("Album · 1987"));
    QCOMPARE(std::next(heading)->detail, QStringLiteral("Megaforce Worldwide"));
    QCOMPARE(std::next(heading, 2)->text,
             QStringLiteral("The Legacy is the debut studio album by Testament."));
    // Its links: the one MusicBrainz names, and a search where it names none.
    const auto link = [&drawn](const QString& site) {
        const auto found = std::ranges::find_if(drawn->parts, [&site](const auto& part) {
            return part.text == site && part.detail == QStringLiteral("The Legacy");
        });
        return found == drawn->parts.end() ? QString{} : found->url;
    };
    QCOMPARE(link(QStringLiteral("Discogs")),
             QStringLiteral("https://www.discogs.com/master/67184"));
    QVERIFY(
        link(QStringLiteral("Bandcamp")).startsWith(QStringLiteral("https://bandcamp.com/search")));

    // Either half off by its setting: the album alone, titled by it.
    QSettings{}.setValue(pluginSettingKey(about_panel->name, QStringLiteral("artist")), false);
    const auto album_only = run(*about_panel,
                                PluginAction{.id = QStringLiteral("about"),
                                             .label = QStringLiteral("About"),
                                             .scope = PluginAction::Scope::tracks,
                                             .run = about_panel->panels.front().run,
                                             .result = PluginAction::Result::none,
                                             .timeout_seconds = 30},
                                pluginPanelInput(about_panel->panels.front(), PluginEngine{},
                                                 PluginTrack{.path = "/srv/music/Testament/01.flac",
                                                             .title = "Over the Wall",
                                                             .artist = "Testament",
                                                             .album = "The Legacy",
                                                             .album_artist = "Testament",
                                                             .date = "1987",
                                                             .track_number = "1"},
                                                 std::nullopt, {}, pluginSettings(*about_panel)));
    QSettings{}.remove(pluginSettingKey(about_panel->name, QStringLiteral("artist")));
    QVERIFY2(album_only.succeeded, qPrintable(album_only.text));
    const auto album_drawn = parsePluginPanel(album_only.text.toUtf8(), &problem);
    QVERIFY2(album_drawn.has_value(), qPrintable(problem));
    QCOMPARE(album_drawn->title, QStringLiteral("The Legacy"));
    QVERIFY(std::ranges::none_of(album_drawn->parts, [](const auto& part) {
        return part.text.contains(QStringLiteral("Berkeley"));
    }));
}

// What a `list` action answers: a heading and rows of text, detail and a
// web link -- nothing else opened from a plugin.
void PluginsTest::aListIsReadOrRefused() {
    QString problem;
    const auto list = parsePluginList(
        R"({"title": "Testament: 2 missing", "items": [
             {"text": "The Legacy", "detail": "1987 · Album",
              "url": "https://musicbrainz.org/release-group/x"},
             {"text": "Souls of Black"}]})",
        &problem);
    QVERIFY2(list.has_value(), qPrintable(problem));
    QCOMPARE(list->title, QStringLiteral("Testament: 2 missing"));
    QCOMPARE(list->items.size(), std::size_t{2U});
    QCOMPARE(list->items[0].detail, QStringLiteral("1987 · Album"));
    QCOMPARE(list->items[1].url, QString{});
    QVERIFY(parsePluginList(R"({"items": []})", &problem).has_value());

    const auto refused = [&problem](const QByteArray& json) {
        problem.clear();
        return !parsePluginList(json, &problem) && !problem.isEmpty();
    };
    QVERIFY(refused("no json"));
    QVERIFY(refused(R"({"title": "x"})"));
    QVERIFY(refused(R"({"items": [{"detail": "no text"}]})"));
    QVERIFY(refused(R"({"items": [{"text": "a", "colour": "red"}]})"));
    QVERIFY(refused(R"({"items": [{"text": "a", "url": "file:///etc/passwd"}]})"));
    QVERIFY(problem.contains(QStringLiteral("http")));
    // A manifest may ask for one.
    QVERIFY(parsePluginManifest(R"({"schema": 1, "name": "A", "actions": [
                                    {"id": "a", "label": "A", "run": ["a"], "result": "list"}]})",
                                QStringLiteral("/p/x"), &problem)
                ->actions.front()
                .result == PluginAction::Result::list);
}

} // namespace trackknife::bench

namespace trackknife::bench {

void PluginsTest::settingsAreReadKeptAndHandedOn() {
    const auto with = [](const QString& settings) {
        return QStringLiteral(R"({"schema": 1, "name": "Watch", "actions": [{"id": "a",
                                 "label": "A", "run": ["x"]}], "settings": [%1]})")
            .arg(settings)
            .toUtf8();
    };
    QString problem;
    auto plugin = parsePluginManifest(
        with(R"({"key": "url", "label": "Address", "default": "http://localhost:8080",
                 "help": "Where it is"},
                {"key": "token", "label": "API token", "kind": "secret"},
                {"key": "verbose", "label": "Say more", "kind": "toggle", "default": true},
                {"key": "mode", "label": "Mode", "kind": "choice", "choices": ["a", "b"],
                 "default": "b"})"),
        QStringLiteral("/plugins/watch"), &problem);
    QVERIFY2(plugin.has_value(), qPrintable(problem));
    QCOMPARE(plugin->settings.size(), 4U);
    QCOMPARE(plugin->settings[0].kind, PluginSetting::Kind::text);
    QCOMPARE(plugin->settings[0].help, QStringLiteral("Where it is"));
    QCOMPARE(plugin->settings[1].kind, PluginSetting::Kind::secret);
    QCOMPARE(plugin->settings[3].choices, (QStringList{QStringLiteral("a"), QStringLiteral("b")}));

    // Each kept to its shape, or refused saying why.
    for (const auto* broken :
         {R"({"key": "URL", "label": "x"})", R"({"key": "a", "label": ""})",
          R"({"key": "a", "label": "x"}, {"key": "a", "label": "y"})",
          R"({"key": "a", "label": "x", "kind": "secret", "default": "s"})",
          R"({"key": "a", "label": "x", "kind": "toggle", "default": "yes"})",
          R"({"key": "a", "label": "x", "kind": "choice"})",
          R"({"key": "a", "label": "x", "kind": "choice", "choices": ["b"], "default": "c"})",
          R"({"key": "a", "label": "x", "choices": ["b"]})",
          R"({"key": "a", "label": "x", "kind": "colour"})",
          R"({"key": "a", "label": "x", "hidden": true})", R"("a")"}) {
        problem.clear();
        QVERIFY2(
            !parsePluginManifest(with(QString::fromUtf8(broken)), QStringLiteral("/p"), &problem),
            broken);
        QVERIFY(!problem.isEmpty());
    }

    // Unset, each is its fallback.
    plugin->name = QStringLiteral("watch");
    QSettings{}.remove(QStringLiteral("plugins"));
    QFile::remove(pluginSecretsFile());
    auto told = pluginSettings(*plugin);
    QCOMPARE(told.value(QStringLiteral("url")).toString(), QStringLiteral("http://localhost:8080"));
    QCOMPARE(told.value(QStringLiteral("token")).toString(), QString{});
    QCOMPARE(told.value(QStringLiteral("verbose")).toBool(false), true);
    QCOMPARE(told.value(QStringLiteral("mode")).toString(), QStringLiteral("b"));

    // Set: the secret apart, its owner's alone, never among the settings.
    QSettings{}.setValue(pluginSettingKey(QStringLiteral("watch"), QStringLiteral("url")),
                         QStringLiteral("http://nas:8080"));
    QSettings{}.setValue(pluginSettingKey(QStringLiteral("watch"), QStringLiteral("verbose")),
                         false);
    QSettings{}.setValue(pluginSettingKey(QStringLiteral("watch"), QStringLiteral("mode")),
                         QStringLiteral("gone"));
    const auto secret_key = pluginSecretKey(QStringLiteral("watch"), QStringLiteral("token"));
    QVERIFY(isPluginSecretKey(secret_key));
    QVERIFY(!isPluginSecretKey(pluginSettingKey(QStringLiteral("watch"), QStringLiteral("url"))));
    savePluginSecret(secret_key, QStringLiteral("s3cret"));
    const auto mode = QFile::permissions(pluginSecretsFile());
    QVERIFY(mode.testFlag(QFileDevice::ReadOwner) && mode.testFlag(QFileDevice::WriteOwner));
    QVERIFY(!(mode & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther |
                      QFileDevice::WriteOther)));
    QSettings{}.sync();
    QFile main{QSettings{}.fileName()};
    QVERIFY(main.open(QIODevice::ReadOnly));
    QVERIFY(!main.readAll().contains("s3cret"));
    told = pluginSettings(*plugin);
    QCOMPARE(told.value(QStringLiteral("url")).toString(), QStringLiteral("http://nas:8080"));
    QCOMPARE(told.value(QStringLiteral("token")).toString(), QStringLiteral("s3cret"));
    QCOMPARE(told.value(QStringLiteral("verbose")).toBool(true), false);
    QCOMPARE(told.value(QStringLiteral("mode")).toString(), QStringLiteral("b"));

    // Handed to a run with the rest.
    const auto input =
        QJsonDocument::fromJson(pluginInput(plugin->actions.front(), PluginEngine{}, {}, {}, told));
    QCOMPARE(input.object()
                 .value(QStringLiteral("settings"))
                 .toObject()
                 .value(QStringLiteral("token"))
                 .toString(),
             QStringLiteral("s3cret"));

    // Emptied, the secret is gone from the file.
    savePluginSecret(secret_key, {});
    QCOMPARE(pluginSettings(*plugin).value(QStringLiteral("token")).toString(), QString{});
    QSettings{}.remove(QStringLiteral("plugins"));
}

// ADR-0291: a panel in a manifest, the answer it gives, and what it is told.
void PluginsTest::panelsAreReadAndTheirAnswersToo() {
    QString problem;
    const auto plugin = parsePluginManifest(
        R"({"schema": 1, "name": "About", "panels": [{"id": "artist", "label": "Artist",
            "run": ["./about"], "timeout": 20}]})",
        QStringLiteral("/plugins/about"), &problem);
    QVERIFY2(plugin.has_value(), qPrintable(problem));
    QVERIFY(plugin->actions.empty());
    QCOMPARE(plugin->panels.size(), 1U);
    QCOMPARE(plugin->panels.front().label, QStringLiteral("Artist"));
    QCOMPARE(plugin->panels.front().timeout_seconds, 20);
    for (const auto* broken :
         {R"({"schema": 1, "name": "A"})", R"({"schema": 1, "name": "A", "panels": []})",
          R"({"schema": 1, "name": "A", "panels": [{"id": "p", "label": "P"}]})",
          R"({"schema": 1, "name": "A", "panels": [{"id": "p", "label": "P", "run": ["x"],
              "for": "selection"}]})",
          R"({"schema": 1, "name": "A", "panels": [{"id": "p", "label": "P", "run": ["x"]},
              {"id": "p", "label": "Q", "run": ["x"]}]})",
          R"({"schema": 1, "name": "A", "panels": [{"id": "p", "label": "P", "run": ["x"],
              "timeout": 900}]})"}) {
        problem.clear();
        QVERIFY2(!parsePluginManifest(broken, QStringLiteral("/p"), &problem), broken);
        QVERIFY(!problem.isEmpty());
    }

    // Its answer: parts Melody draws, each named by its one key.
    const auto content = parsePluginPanel(
        R"({"title": "Testament", "parts": [{"heading": "About"},
            {"paragraph": "Thrash from Berkeley."},
            {"text": "The Legacy", "detail": "1987", "url": "https://example.org/1"},
            {"image": "/tmp/cover.jpg", "caption": "Live", "url": "https://example.org/2"}]})",
        &problem);
    QVERIFY2(content.has_value(), qPrintable(problem));
    using Kind = PluginPanelContent::Part::Kind;
    QCOMPARE(content->title, QStringLiteral("Testament"));
    QCOMPARE(content->parts.size(), 4U);
    QCOMPARE(content->parts[0].kind, Kind::heading);
    QCOMPARE(content->parts[1].kind, Kind::paragraph);
    QCOMPARE(content->parts[2].kind, Kind::item);
    QCOMPARE(content->parts[2].detail, QStringLiteral("1987"));
    QCOMPARE(content->parts[3].kind, Kind::image);
    QCOMPARE(content->parts[3].text, QStringLiteral("Live"));
    QVERIFY(parsePluginPanel(R"({"parts": []})", &problem).has_value());
    for (const auto* broken :
         {R"({"title": "x"})", R"({"parts": [{"heading": "a", "url": "https://x"}]})",
          R"({"parts": [{"quote": "a"}]})", R"({"parts": [{"image": "cover.jpg"}]})",
          R"({"parts": [{"text": "a", "url": "file:///etc/passwd"}]})",
          R"({"parts": [], "html": "<b>"})", "["}) {
        problem.clear();
        QVERIFY2(!parsePluginPanel(broken, &problem), broken);
        QVERIFY(!problem.isEmpty());
    }

    // What it is told: what plays, within its music folder, and its settings.
    const auto told = QJsonDocument::fromJson(pluginPanelInput(
        plugin->panels.front(),
        PluginEngine{.name = QStringLiteral("gemenon"), .address = {}, .here = true},
        PluginTrack{.path = "/srv/music/Testament/1987 The Legacy/01.flac",
                    .title = "Over the Wall",
                    .artist = "Testament",
                    .album = "The Legacy",
                    .album_artist = "Testament",
                    .date = "1987",
                    .track_number = "1"},
        250'000, {"/srv/music"}, QJsonObject{{QStringLiteral("lang"), QStringLiteral("en")}}));
    const auto root = told.object();
    QCOMPARE(root.value(QStringLiteral("panel")).toString(), QStringLiteral("artist"));
    const auto playing = root.value(QStringLiteral("playing")).toObject();
    QCOMPARE(playing.value(QStringLiteral("artist")).toString(), QStringLiteral("Testament"));
    QCOMPARE(playing.value(QStringLiteral("relative_path")).toString(),
             QStringLiteral("Testament/1987 The Legacy/01.flac"));
    QCOMPARE(playing.value(QStringLiteral("duration_ms")).toInteger(), 250'000);
    QCOMPARE(root.value(QStringLiteral("settings")).toObject().value(QStringLiteral("lang")),
             QJsonValue{QStringLiteral("en")});
    const auto nothing = QJsonDocument::fromJson(pluginPanelInput(
        plugin->panels.front(), PluginEngine{}, std::nullopt, std::nullopt, {}, {}));
    QVERIFY(nothing.object().value(QStringLiteral("playing")).isNull());
}

// ADR-0293: a lyrics provider in a manifest, what it is told and what it
// answers -- and the official one, answering from what it keeps.
void PluginsTest::providersAreReadAndTheirLyricsToo() {
    QString problem;
    const auto plugin = parsePluginManifest(
        R"({"schema": 1, "name": "Words", "providers": [{"id": "lyrics", "provides": "lyrics",
            "run": ["./words"], "timeout": 20}]})",
        QStringLiteral("/plugins/words"), &problem);
    QVERIFY2(plugin.has_value(), qPrintable(problem));
    QCOMPARE(plugin->providers.size(), 1U);
    QCOMPARE(plugin->providers.front().provides, QStringLiteral("lyrics"));
    QCOMPARE(plugin->providers.front().timeout_seconds, 20);
    for (const auto* broken :
         {R"({"schema": 1, "name": "W", "providers": [{"id": "l", "provides": "covers",
             "run": ["x"]}]})",
          R"({"schema": 1, "name": "W", "providers": [{"id": "l", "provides": "lyrics"}]})",
          R"({"schema": 1, "name": "W", "providers": [{"id": "l", "provides": "lyrics",
              "run": ["x"], "timeout": 500}]})",
          R"({"schema": 1, "name": "W", "providers": [{"id": "l", "provides": "lyrics",
              "run": ["x"], "when": "always"}]})"}) {
        problem.clear();
        QVERIFY2(!parsePluginManifest(broken, QStringLiteral("/p"), &problem), broken);
        QVERIFY(!problem.isEmpty());
    }

    const auto found =
        parsePluginLyrics(R"({"lyrics": "[00:01.00]One", "source": "LRCLIB"})", &problem);
    QVERIFY(found && *found);
    QCOMPARE((*found)->text, QStringLiteral("[00:01.00]One"));
    QCOMPARE((*found)->source, QStringLiteral("LRCLIB"));
    const auto none = parsePluginLyrics(R"({"lyrics": null})", &problem);
    QVERIFY(none && !*none);
    QVERIFY(parsePluginLyrics(R"({"lyrics": "  "})", &problem).value_or(std::nullopt) ==
            std::nullopt);
    for (const auto* broken :
         {R"({})", R"({"lyrics": 3})", R"({"lyrics": null, "html": 1})", "x"}) {
        problem.clear();
        QVERIFY2(!parsePluginLyrics(broken, &problem), broken);
        QVERIFY(!problem.isEmpty());
    }
    const auto told =
        QJsonDocument::fromJson(pluginProviderInput(plugin->providers.front(), PluginEngine{},
                                                    PluginTrack{.path = "/srv/music/A/01.flac",
                                                                .title = "Samson",
                                                                .artist = "Regina Spektor",
                                                                .album = "Begin to Hope",
                                                                .album_artist = {},
                                                                .date = {},
                                                                .track_number = {}},
                                                    190'000, {"/srv/music"}, {}));
    QCOMPARE(told.object().value(QStringLiteral("provide")).toString(), QStringLiteral("lyrics"));
    QCOMPARE(
        told.object().value(QStringLiteral("playing")).toObject().value(QStringLiteral("title")),
        QJsonValue{QStringLiteral("Samson")});

    // The official LRCLIB provider reads, and answers from what it keeps.
    const QString lrclib = QStringLiteral(MELODY_SOURCE_DIR "/plugins/lrclib");
    QFile manifest{lrclib + QStringLiteral("/plugin.json")};
    QVERIFY(manifest.open(QIODevice::ReadOnly));
    const auto official = parsePluginManifest(manifest.readAll(), lrclib, &problem);
    QVERIFY2(official.has_value(), qPrintable(problem));
    QCOMPARE(official->providers.size(), 1U);
    QTemporaryDir cache;
    QVERIFY(cache.isValid());
    const auto kept = QDir{cache.path()}.filePath(QStringLiteral("melody/plugins/lrclib"));
    QVERIFY(QDir{}.mkpath(kept));
    const auto key = QCryptographicHash::hash(
                         QStringList{QStringLiteral("regina spektor"), QStringLiteral("samson"),
                                     QStringLiteral("begin to hope"), QStringLiteral("190")}
                             .join(QChar{0x1f})
                             .toUtf8(),
                         QCryptographicHash::Sha1)
                         .toHex();
    writeFile(kept + QLatin1Char('/') + QString::fromLatin1(key),
              R"({"lyrics": "[00:12.00]You are my sweetest downfall"})");
    qputenv("XDG_CACHE_HOME", QFile::encodeName(cache.path()));
    const auto answered = run(*official,
                              PluginAction{.id = QStringLiteral("lyrics"),
                                           .label = QStringLiteral("Lyrics"),
                                           .scope = PluginAction::Scope::tracks,
                                           .run = official->providers.front().run,
                                           .result = PluginAction::Result::none,
                                           .timeout_seconds = 30},
                              pluginProviderInput(official->providers.front(), PluginEngine{},
                                                  PluginTrack{.path = "/srv/music/A/01.flac",
                                                              .title = "Samson",
                                                              .artist = "Regina Spektor",
                                                              .album = "Begin to Hope",
                                                              .album_artist = {},
                                                              .date = {},
                                                              .track_number = {}},
                                                  190'000, {}, {}));
    QVERIFY2(answered.succeeded, qPrintable(answered.text));
    const auto lyrics = parsePluginLyrics(answered.text.toUtf8(), &problem);
    QVERIFY2(lyrics && *lyrics, qPrintable(problem));
    QCOMPARE((*lyrics)->text, QStringLiteral("[00:12.00]You are my sweetest downfall"));
    QCOMPARE((*lyrics)->source, QStringLiteral("LRCLIB"));
}

} // namespace trackknife::bench

QTEST_GUILESS_MAIN(trackknife::bench::PluginsTest)
#include "plugins_test.moc"
