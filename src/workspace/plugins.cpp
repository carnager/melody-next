// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/plugins.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <map>
#include <memory>
#include <utility>

namespace trackknife::bench {
namespace {

constexpr int manifest_schema = 1;
constexpr int input_schema = 1;

[[nodiscard]] QString text(const std::string& value) { return QString::fromUtf8(value); }

// The longest of `roots` holding `path`, on a component boundary.
[[nodiscard]] std::string musicFolderOf(const std::string& path,
                                        const std::vector<std::string>& roots) {
    std::string found;
    for (auto root : roots) {
        while (root.size() > 1U && root.ends_with('/')) {
            root.pop_back();
        }
        const bool holds = root == "/" || path == root || path.starts_with(root + '/');
        if (holds && root.size() > found.size()) {
            found = root;
        }
    }
    return found;
}

[[nodiscard]] std::string relativeTo(const std::string& path, const std::string& root) {
    if (root.empty() || path.size() < root.size()) {
        return {};
    }
    if (path == root) {
        return {};
    }
    return root == "/" ? path.substr(1U) : path.substr(root.size() + 1U);
}

[[nodiscard]] std::string parentOf(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string{} : path.substr(0U, slash == 0U ? 1U : slash);
}

// The folder every one of `paths` is in.
[[nodiscard]] std::string commonFolder(const std::vector<std::string>& folders) {
    if (folders.empty()) {
        return {};
    }
    auto common = folders.front();
    for (const auto& folder : folders) {
        while (!common.empty() && folder != common && !folder.starts_with(common + '/')) {
            common = parentOf(common);
        }
    }
    return common;
}

} // namespace

std::optional<Plugin> parsePluginManifest(const QByteArray& json, const QString& folder,
                                          QString* problem) {
    const auto fail = [problem](QString why) -> std::optional<Plugin> {
        if (problem != nullptr) {
            *problem = std::move(why);
        }
        return std::nullopt;
    };
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(
            QStringLiteral("plugin.json is not a JSON object: %1").arg(error.errorString()));
    }
    const auto object = document.object();
    static const QStringList known{QStringLiteral("schema"), QStringLiteral("name"),
                                   QStringLiteral("description"), QStringLiteral("actions")};
    for (const auto& key : object.keys()) {
        if (!known.contains(key)) {
            return fail(QStringLiteral("plugin.json has an unknown key \"%1\"").arg(key));
        }
    }
    if (object.value(QStringLiteral("schema")).toInt(-1) != manifest_schema) {
        return fail(QStringLiteral("plugin.json is not schema 1"));
    }
    Plugin plugin;
    plugin.name = QFileInfo{folder}.fileName();
    plugin.folder = folder;
    plugin.title = object.value(QStringLiteral("name")).toString().trimmed();
    plugin.description = object.value(QStringLiteral("description")).toString().trimmed();
    if (plugin.title.isEmpty()) {
        return fail(QStringLiteral("plugin.json has no name"));
    }
    const auto actions = object.value(QStringLiteral("actions"));
    if (!actions.isArray() || actions.toArray().isEmpty()) {
        return fail(QStringLiteral("plugin.json has no actions"));
    }
    static const QStringList action_keys{QStringLiteral("id"),     QStringLiteral("label"),
                                         QStringLiteral("for"),    QStringLiteral("run"),
                                         QStringLiteral("result"), QStringLiteral("timeout")};
    for (const auto& value : actions.toArray()) {
        if (!value.isObject()) {
            return fail(QStringLiteral("an action is not an object"));
        }
        const auto entry = value.toObject();
        for (const auto& key : entry.keys()) {
            if (!action_keys.contains(key)) {
                return fail(QStringLiteral("an action has an unknown key \"%1\"").arg(key));
            }
        }
        PluginAction action;
        action.id = entry.value(QStringLiteral("id")).toString().trimmed();
        action.label = entry.value(QStringLiteral("label")).toString().trimmed();
        if (action.id.isEmpty() || action.label.isEmpty()) {
            return fail(QStringLiteral("an action needs an id and a label"));
        }
        const auto scope = entry.value(QStringLiteral("for")).toString(QStringLiteral("albums"));
        if (scope == QStringLiteral("albums")) {
            action.scope = PluginAction::Scope::albums;
        } else if (scope == QStringLiteral("tracks")) {
            action.scope = PluginAction::Scope::tracks;
        } else {
            return fail(
                QStringLiteral("action \"%1\": \"for\" is albums or tracks").arg(action.id));
        }
        const auto run = entry.value(QStringLiteral("run")).toArray();
        for (const auto& word : run) {
            if (!word.isString() || word.toString().isEmpty()) {
                return fail(
                    QStringLiteral("action \"%1\": \"run\" is a list of words").arg(action.id));
            }
            action.run.push_back(word.toString());
        }
        if (action.run.isEmpty()) {
            return fail(QStringLiteral("action \"%1\" has nothing to run").arg(action.id));
        }
        const auto result =
            entry.value(QStringLiteral("result")).toString(QStringLiteral("notify"));
        if (result == QStringLiteral("none")) {
            action.result = PluginAction::Result::none;
        } else if (result == QStringLiteral("notify")) {
            action.result = PluginAction::Result::notify;
        } else if (result == QStringLiteral("copy")) {
            action.result = PluginAction::Result::copy;
        } else if (result == QStringLiteral("notify-and-copy")) {
            action.result = PluginAction::Result::notify_and_copy;
        } else if (result == QStringLiteral("list")) {
            action.result = PluginAction::Result::list;
        } else {
            return fail(QStringLiteral("action \"%1\": \"result\" is none, notify, copy, "
                                       "notify-and-copy or list")
                            .arg(action.id));
        }
        if (entry.contains(QStringLiteral("timeout"))) {
            const auto seconds = entry.value(QStringLiteral("timeout")).toInt(0);
            if (seconds < 1 || seconds > 3600) {
                return fail(QStringLiteral("action \"%1\": \"timeout\" is 1 to 3600 seconds")
                                .arg(action.id));
            }
            action.timeout_seconds = seconds;
        }
        plugin.actions.push_back(std::move(action));
    }
    return plugin;
}

std::optional<PluginList> parsePluginList(const QByteArray& json, QString* problem) {
    const auto fail = [problem](QString why) -> std::optional<PluginList> {
        if (problem != nullptr) {
            *problem = std::move(why);
        }
        return std::nullopt;
    };
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(QStringLiteral("its answer is not a JSON object: %1").arg(error.errorString()));
    }
    const auto object = document.object();
    for (const auto& key : object.keys()) {
        if (key != QStringLiteral("title") && key != QStringLiteral("items")) {
            return fail(QStringLiteral("its answer has an unknown key \"%1\"").arg(key));
        }
    }
    PluginList list;
    list.title = object.value(QStringLiteral("title")).toString().trimmed();
    const auto items = object.value(QStringLiteral("items"));
    if (!items.isArray()) {
        return fail(QStringLiteral("its answer has no items"));
    }
    for (const auto& value : items.toArray()) {
        const auto item = value.toObject();
        if (!value.isObject() || !item.value(QStringLiteral("text")).isString()) {
            return fail(QStringLiteral("an item has no text"));
        }
        for (const auto& key : item.keys()) {
            if (key != QStringLiteral("text") && key != QStringLiteral("detail") &&
                key != QStringLiteral("url")) {
                return fail(QStringLiteral("an item has an unknown key \"%1\"").arg(key));
            }
        }
        PluginList::Item read{.text = item.value(QStringLiteral("text")).toString(),
                              .detail = item.value(QStringLiteral("detail")).toString(),
                              .url = item.value(QStringLiteral("url")).toString()};
        // A link is a web page's: nothing else is opened from a plugin.
        if (!read.url.isEmpty() && !read.url.startsWith(QStringLiteral("https://")) &&
            !read.url.startsWith(QStringLiteral("http://"))) {
            return fail(QStringLiteral("a link is not http or https: %1").arg(read.url));
        }
        list.items.push_back(std::move(read));
    }
    return list;
}

QStringList pluginFolders() {
    QStringList folders;
    for (const auto& data :
         QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
        folders.push_back(QDir{data}.filePath(QStringLiteral("melody/plugins")));
    }
    return folders;
}

std::vector<FoundPlugin> discoverPlugins() {
    std::vector<FoundPlugin> found;
    for (const auto& base : pluginFolders()) {
        const QDir directory{base};
        for (const auto& name :
             directory.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            if (std::ranges::any_of(found,
                                    [&name](const auto& known) { return known.name == name; })) {
                continue;
            }
            FoundPlugin plugin{
                .name = name, .folder = directory.filePath(name), .plugin = {}, .problem = {}};
            QFile manifest{QDir{plugin.folder}.filePath(QStringLiteral("plugin.json"))};
            if (!manifest.open(QIODevice::ReadOnly)) {
                continue;
            }
            plugin.plugin = parsePluginManifest(manifest.readAll(), plugin.folder, &plugin.problem);
            found.push_back(std::move(plugin));
        }
    }
    return found;
}

QString pluginEnabledKey(const QString& name) { return QStringLiteral("plugins/enabled/") + name; }

bool pluginEnabled(const QString& name) {
    return QSettings{}.value(pluginEnabledKey(name), false).toBool();
}

std::vector<Plugin> enabledPlugins() {
    std::vector<Plugin> plugins;
    for (auto& found : discoverPlugins()) {
        if (found.plugin && pluginEnabled(found.name)) {
            plugins.push_back(std::move(*found.plugin));
        }
    }
    return plugins;
}

std::vector<PluginAlbum> pluginAlbums(std::vector<PluginTrack> tracks,
                                      const std::vector<std::string>& music_folders) {
    // In the order first met; an album without tags is its folder.
    std::vector<PluginAlbum> albums;
    std::map<std::string, std::size_t> index;
    for (auto& track : tracks) {
        const auto& artist = track.album_artist.empty() ? track.artist : track.album_artist;
        const auto key = track.album.empty() ? "\x01" + parentOf(track.path)
                                             : artist + '\x1f' + track.album + '\x1f' + track.date;
        auto [at, fresh] = index.emplace(key, albums.size());
        if (fresh) {
            albums.push_back(PluginAlbum{.artist = artist,
                                         .album = track.album,
                                         .date = track.date,
                                         .music_folder = {},
                                         .folder = {},
                                         .relative_folder = {},
                                         .tracks = {}});
        }
        albums[at->second].tracks.push_back(std::move(track));
    }
    for (auto& album : albums) {
        std::vector<std::string> folders;
        folders.reserve(album.tracks.size());
        for (const auto& track : album.tracks) {
            folders.push_back(parentOf(track.path));
        }
        album.folder = commonFolder(folders);
        album.music_folder = musicFolderOf(album.folder, music_folders);
        album.relative_folder = relativeTo(album.folder, album.music_folder);
    }
    return albums;
}

QByteArray pluginInput(const PluginAction& action, const PluginEngine& engine,
                       const std::vector<PluginAlbum>& albums,
                       const std::vector<std::string>& music_folders) {
    QJsonArray listed;
    for (const auto& album : albums) {
        QJsonArray tracks;
        for (const auto& track : album.tracks) {
            const auto root = musicFolderOf(track.path, music_folders);
            tracks.push_back(
                QJsonObject{{QStringLiteral("path"), text(track.path)},
                            {QStringLiteral("relative_path"), text(relativeTo(track.path, root))},
                            {QStringLiteral("title"), text(track.title)},
                            {QStringLiteral("artist"), text(track.artist)},
                            {QStringLiteral("track_number"), text(track.track_number)}});
        }
        listed.push_back(
            QJsonObject{{QStringLiteral("artist"), text(album.artist)},
                        {QStringLiteral("album"), text(album.album)},
                        {QStringLiteral("date"), text(album.date)},
                        {QStringLiteral("music_folder"), text(album.music_folder)},
                        {QStringLiteral("folder"), text(album.folder)},
                        {QStringLiteral("relative_folder"), text(album.relative_folder)},
                        {QStringLiteral("tracks"), tracks}});
    }
    const QJsonObject input{
        {QStringLiteral("schema"), input_schema},
        {QStringLiteral("action"), action.id},
        {QStringLiteral("engine"), QJsonObject{{QStringLiteral("name"), engine.name},
                                               {QStringLiteral("address"), engine.address},
                                               {QStringLiteral("here"), engine.here}}},
        {QStringLiteral("albums"), listed}};
    return QJsonDocument{input}.toJson(QJsonDocument::Compact);
}

void runPluginAction(const Plugin& plugin, const PluginAction& action, QByteArray input,
                     QObject* context, std::function<void(PluginOutcome)> done) {
    auto* process = new QProcess(context);
    process->setWorkingDirectory(plugin.folder);
    auto program = action.run.front();
    if (program.startsWith(QStringLiteral("./"))) {
        program = QDir{plugin.folder}.filePath(program.mid(2));
    }
    process->setProgram(program);
    process->setArguments(action.run.mid(1));
    auto finished = std::make_shared<bool>(false);
    auto finish = [process, finished, done = std::move(done)](PluginOutcome outcome) {
        if (*finished) {
            return;
        }
        *finished = true;
        done(std::move(outcome));
        process->deleteLater();
    };
    auto* timer = new QTimer(process);
    timer->setSingleShot(true);
    QObject::connect(
        timer, &QTimer::timeout, process, [process, finish, seconds = action.timeout_seconds] {
            process->kill();
            finish(PluginOutcome{.succeeded = false,
                                 .text = QStringLiteral("stopped after %1 seconds").arg(seconds)});
        });
    QObject::connect(
        process, &QProcess::errorOccurred, process,
        [process, finish](const QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                finish(PluginOutcome{.succeeded = false,
                                     .text = QStringLiteral("could not start %1: %2")
                                                 .arg(process->program(), process->errorString())});
            }
        });
    QObject::connect(
        process, &QProcess::finished, process,
        [process, finish](const int code, const QProcess::ExitStatus status) {
            const auto out = QString::fromUtf8(process->readAllStandardOutput()).trimmed();
            const auto err = QString::fromUtf8(process->readAllStandardError()).trimmed();
            if (status == QProcess::NormalExit && code == 0) {
                finish(PluginOutcome{.succeeded = true, .text = out});
            } else {
                finish(PluginOutcome{.succeeded = false,
                                     .text = !err.isEmpty() ? err
                                             : status == QProcess::CrashExit
                                                 ? QStringLiteral("it crashed")
                                                 : QStringLiteral("it ended with %1").arg(code)});
            }
        });
    process->start();
    process->write(input);
    process->closeWriteChannel();
    timer->start(action.timeout_seconds * 1000);
}

} // namespace trackknife::bench
