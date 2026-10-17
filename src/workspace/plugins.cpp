// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/plugins.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
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

// Each setting read with its every key, or why one cannot be.
[[nodiscard]] std::optional<QString> readSettings(const QJsonArray& listed,
                                                  std::vector<PluginSetting>& settings) {
    static const QStringList setting_keys{QStringLiteral("key"),     QStringLiteral("label"),
                                          QStringLiteral("kind"),    QStringLiteral("default"),
                                          QStringLiteral("choices"), QStringLiteral("help")};
    static const QRegularExpression key_shape{QStringLiteral("^[a-z0-9][a-z0-9_-]*$")};
    for (const auto& value : listed) {
        if (!value.isObject()) {
            return QStringLiteral("a setting is not an object");
        }
        const auto entry = value.toObject();
        for (const auto& key : entry.keys()) {
            if (!setting_keys.contains(key)) {
                return QStringLiteral("a setting has an unknown key \"%1\"").arg(key);
            }
        }
        PluginSetting setting;
        setting.key = entry.value(QStringLiteral("key")).toString();
        setting.label = entry.value(QStringLiteral("label")).toString().trimmed();
        setting.help = entry.value(QStringLiteral("help")).toString().trimmed();
        if (!key_shape.match(setting.key).hasMatch() || setting.label.isEmpty()) {
            return QStringLiteral("a setting needs a key of a-z, 0-9, - and _, and a label");
        }
        if (std::ranges::any_of(settings,
                                [&](const auto& known) { return known.key == setting.key; })) {
            return QStringLiteral("setting \"%1\" is there twice").arg(setting.key);
        }
        const auto kind = entry.value(QStringLiteral("kind")).toString(QStringLiteral("text"));
        const auto given = entry.value(QStringLiteral("default"));
        const bool has_default = entry.contains(QStringLiteral("default"));
        const auto wrong = [&setting](const char* why) {
            return QStringLiteral("setting \"%1\": %2").arg(setting.key, QLatin1String(why));
        };
        if (kind == QStringLiteral("text")) {
            setting.kind = PluginSetting::Kind::text;
            if (has_default && !given.isString()) {
                return wrong("its default is text");
            }
            setting.fallback = given.toString();
        } else if (kind == QStringLiteral("secret")) {
            setting.kind = PluginSetting::Kind::secret;
            if (has_default) {
                return wrong("a secret has no default");
            }
            setting.fallback = QString{};
        } else if (kind == QStringLiteral("toggle")) {
            setting.kind = PluginSetting::Kind::toggle;
            if (has_default && !given.isBool()) {
                return wrong("its default is true or false");
            }
            setting.fallback = given.toBool(false);
        } else if (kind == QStringLiteral("choice")) {
            setting.kind = PluginSetting::Kind::choice;
            for (const auto& choice : entry.value(QStringLiteral("choices")).toArray()) {
                if (!choice.isString() || choice.toString().isEmpty()) {
                    return wrong("its choices are text");
                }
                setting.choices.push_back(choice.toString());
            }
            if (setting.choices.isEmpty()) {
                return wrong("a choice needs its choices");
            }
            if (has_default && !setting.choices.contains(given.toString())) {
                return wrong("its default is one of its choices");
            }
            setting.fallback = has_default ? given.toString() : setting.choices.front();
        } else {
            return wrong("\"kind\" is text, secret, toggle or choice");
        }
        if (setting.kind != PluginSetting::Kind::choice &&
            entry.contains(QStringLiteral("choices"))) {
            return wrong("only a choice has choices");
        }
        settings.push_back(std::move(setting));
    }
    return std::nullopt;
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
    static const QStringList known{QStringLiteral("schema"),      QStringLiteral("name"),
                                   QStringLiteral("description"), QStringLiteral("actions"),
                                   QStringLiteral("panels"),      QStringLiteral("settings")};
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
    const auto panels = object.value(QStringLiteral("panels"));
    if ((!actions.isUndefined() && !actions.isArray()) ||
        (!panels.isUndefined() && !panels.isArray())) {
        return fail(QStringLiteral("\"actions\" and \"panels\" are lists"));
    }
    if (actions.toArray().isEmpty() && panels.toArray().isEmpty()) {
        return fail(QStringLiteral("plugin.json has no actions and no panels"));
    }
    static const QStringList panel_keys{QStringLiteral("id"), QStringLiteral("label"),
                                        QStringLiteral("run"), QStringLiteral("timeout")};
    for (const auto& value : panels.toArray()) {
        if (!value.isObject()) {
            return fail(QStringLiteral("a panel is not an object"));
        }
        const auto entry = value.toObject();
        for (const auto& key : entry.keys()) {
            if (!panel_keys.contains(key)) {
                return fail(QStringLiteral("a panel has an unknown key \"%1\"").arg(key));
            }
        }
        PluginPanel panel;
        panel.id = entry.value(QStringLiteral("id")).toString().trimmed();
        panel.label = entry.value(QStringLiteral("label")).toString().trimmed();
        if (panel.id.isEmpty() || panel.label.isEmpty()) {
            return fail(QStringLiteral("a panel needs an id and a label"));
        }
        if (std::ranges::any_of(plugin.panels,
                                [&panel](const auto& other) { return other.id == panel.id; })) {
            return fail(QStringLiteral("panel \"%1\" is there twice").arg(panel.id));
        }
        for (const auto& word : entry.value(QStringLiteral("run")).toArray()) {
            if (!word.isString() || word.toString().isEmpty()) {
                return fail(
                    QStringLiteral("panel \"%1\": \"run\" is a list of words").arg(panel.id));
            }
            panel.run.push_back(word.toString());
        }
        if (panel.run.isEmpty()) {
            return fail(QStringLiteral("panel \"%1\" has nothing to run").arg(panel.id));
        }
        if (entry.contains(QStringLiteral("timeout"))) {
            const auto seconds = entry.value(QStringLiteral("timeout")).toInt(0);
            if (seconds < 1 || seconds > 300) {
                return fail(
                    QStringLiteral("panel \"%1\": \"timeout\" is 1 to 300 seconds").arg(panel.id));
            }
            panel.timeout_seconds = seconds;
        }
        plugin.panels.push_back(std::move(panel));
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
    if (object.contains(QStringLiteral("settings"))) {
        const auto settings = object.value(QStringLiteral("settings"));
        if (!settings.isArray()) {
            return fail(QStringLiteral("\"settings\" is a list"));
        }
        if (auto why = readSettings(settings.toArray(), plugin.settings)) {
            return fail(*why);
        }
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

std::optional<PluginPanelContent> parsePluginPanel(const QByteArray& json, QString* problem) {
    const auto fail = [problem](QString why) -> std::optional<PluginPanelContent> {
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
        if (key != QStringLiteral("title") && key != QStringLiteral("parts")) {
            return fail(QStringLiteral("its answer has an unknown key \"%1\"").arg(key));
        }
    }
    const auto parts = object.value(QStringLiteral("parts"));
    if (!parts.isArray()) {
        return fail(QStringLiteral("its answer has no parts"));
    }
    PluginPanelContent content;
    content.title = object.value(QStringLiteral("title")).toString().trimmed();
    using Kind = PluginPanelContent::Part::Kind;
    for (const auto& value : parts.toArray()) {
        if (!value.isObject()) {
            return fail(QStringLiteral("a part is not an object"));
        }
        const auto part = value.toObject();
        // What a part is, by the one key that names it.
        PluginPanelContent::Part read;
        QStringList allowed;
        if (part.contains(QStringLiteral("heading"))) {
            read.kind = Kind::heading;
            read.text = part.value(QStringLiteral("heading")).toString();
            allowed = {QStringLiteral("heading")};
        } else if (part.contains(QStringLiteral("paragraph"))) {
            read.kind = Kind::paragraph;
            read.text = part.value(QStringLiteral("paragraph")).toString();
            allowed = {QStringLiteral("paragraph")};
        } else if (part.contains(QStringLiteral("image"))) {
            read.kind = Kind::image;
            read.image = part.value(QStringLiteral("image")).toString();
            read.text = part.value(QStringLiteral("caption")).toString();
            read.url = part.value(QStringLiteral("url")).toString();
            allowed = {QStringLiteral("image"), QStringLiteral("caption"), QStringLiteral("url")};
            if (!QDir::isAbsolutePath(read.image)) {
                return fail(
                    QStringLiteral("an image is a file's absolute path: %1").arg(read.image));
            }
        } else if (part.contains(QStringLiteral("text"))) {
            read.kind = Kind::item;
            read.text = part.value(QStringLiteral("text")).toString();
            read.detail = part.value(QStringLiteral("detail")).toString();
            read.url = part.value(QStringLiteral("url")).toString();
            allowed = {QStringLiteral("text"), QStringLiteral("detail"), QStringLiteral("url")};
        } else {
            return fail(QStringLiteral("a part is a heading, paragraph, text or image"));
        }
        for (const auto& key : part.keys()) {
            if (!allowed.contains(key)) {
                return fail(QStringLiteral("a part has an unknown key \"%1\"").arg(key));
            }
        }
        // A link is a web page's: nothing else is opened from a plugin.
        if (!read.url.isEmpty() && !read.url.startsWith(QStringLiteral("https://")) &&
            !read.url.startsWith(QStringLiteral("http://"))) {
            return fail(QStringLiteral("a link is not http or https: %1").arg(read.url));
        }
        content.parts.push_back(std::move(read));
    }
    return content;
}

QStringList pluginFolders() {
    // Named, these and no others: for writing a plugin elsewhere, and for
    // tests, on every platform.
    if (const auto named = qEnvironmentVariable("MELODY_PLUGIN_PATH"); !named.isEmpty()) {
        return named.split(QDir::listSeparator(), Qt::SkipEmptyParts);
    }
    QStringList folders;
    const auto add = [&folders](const QString& folder) {
        if (const auto clean = QDir::cleanPath(folder); !folders.contains(clean)) {
            folders.push_back(clean);
        }
    };
    for (const auto& data :
         QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
        add(QDir{data}.filePath(QStringLiteral("melody/plugins")));
    }
    // The official ones, installed beside the program: on macOS the data
    // folders are not where it was installed.
    add(QDir{QCoreApplication::applicationDirPath()}.filePath(
        QStringLiteral("../share/melody/plugins")));
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

QString pluginSettingKey(const QString& plugin, const QString& key) {
    return QStringLiteral("plugins/settings/%1/%2").arg(plugin, key);
}

QString pluginSecretKey(const QString& plugin, const QString& key) {
    return QStringLiteral("plugins/secrets/%1/%2").arg(plugin, key);
}

bool isPluginSecretKey(const QString& settings_key) {
    return settings_key.startsWith(QStringLiteral("plugins/secrets/"));
}

QString pluginSecretsFile() {
    // Beside Melody's settings, wherever they are -- a test's own included.
    return QFileInfo{QSettings{}.fileName()}.dir().filePath(QStringLiteral("plugin-secrets.conf"));
}

namespace {

// The secrets file, made its owner's alone before anything is in it.
[[nodiscard]] std::unique_ptr<QSettings> openSecrets(const bool writing) {
    const auto path = pluginSecretsFile();
    if (writing) {
        QDir{}.mkpath(QFileInfo{path}.absolutePath());
        QFile file{path};
        if (!file.exists() && file.open(QIODevice::WriteOnly)) {
            file.close();
        }
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    return std::make_unique<QSettings>(path, QSettings::IniFormat);
}

} // namespace

void savePluginSecret(const QString& settings_key, const QString& value) {
    if (!isPluginSecretKey(settings_key)) {
        return;
    }
    auto secrets = openSecrets(true);
    const auto key = settings_key.mid(QStringLiteral("plugins/secrets/").size());
    if (value.isEmpty()) {
        secrets->remove(key);
    } else {
        secrets->setValue(key, value);
    }
    secrets->sync();
    QFile::setPermissions(pluginSecretsFile(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

QVariant pluginSettingValue(const QString& plugin, const PluginSetting& setting) {
    if (setting.kind == PluginSetting::Kind::secret) {
        return openSecrets(false)->value(QStringLiteral("%1/%2").arg(plugin, setting.key),
                                         QString{});
    }
    const auto stored = QSettings{}.value(pluginSettingKey(plugin, setting.key));
    if (!stored.isValid()) {
        return setting.fallback;
    }
    if (setting.kind == PluginSetting::Kind::toggle) {
        return stored.toBool();
    }
    // A choice no longer offered is the fallback's.
    if (setting.kind == PluginSetting::Kind::choice &&
        !setting.choices.contains(stored.toString())) {
        return setting.fallback;
    }
    return stored.toString();
}

QJsonObject pluginSettings(const Plugin& plugin) {
    QJsonObject settings;
    for (const auto& setting : plugin.settings) {
        const auto value = pluginSettingValue(plugin.name, setting);
        settings.insert(setting.key, setting.kind == PluginSetting::Kind::toggle
                                         ? QJsonValue{value.toBool()}
                                         : QJsonValue{value.toString()});
    }
    return settings;
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
                       const std::vector<std::string>& music_folders, const QJsonObject& settings) {
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
        {QStringLiteral("albums"), listed},
        {QStringLiteral("settings"), settings}};
    return QJsonDocument{input}.toJson(QJsonDocument::Compact);
}

QByteArray pluginPanelInput(const PluginPanel& panel, const PluginEngine& engine,
                            const std::optional<PluginTrack>& playing,
                            const std::optional<std::int64_t> duration_ms,
                            const std::vector<std::string>& music_folders,
                            const QJsonObject& settings) {
    QJsonValue track{QJsonValue::Null};
    if (playing) {
        const auto root = musicFolderOf(playing->path, music_folders);
        track = QJsonObject{
            {QStringLiteral("path"), text(playing->path)},
            {QStringLiteral("music_folder"), text(root)},
            {QStringLiteral("relative_path"), text(relativeTo(playing->path, root))},
            {QStringLiteral("title"), text(playing->title)},
            {QStringLiteral("artist"), text(playing->artist)},
            {QStringLiteral("album"), text(playing->album)},
            {QStringLiteral("album_artist"), text(playing->album_artist)},
            {QStringLiteral("date"), text(playing->date)},
            {QStringLiteral("track_number"), text(playing->track_number)},
            {QStringLiteral("duration_ms"),
             duration_ms ? QJsonValue{static_cast<double>(*duration_ms)} : QJsonValue{}}};
    }
    const QJsonObject input{
        {QStringLiteral("schema"), input_schema},
        {QStringLiteral("panel"), panel.id},
        {QStringLiteral("engine"), QJsonObject{{QStringLiteral("name"), engine.name},
                                               {QStringLiteral("address"), engine.address},
                                               {QStringLiteral("here"), engine.here}}},
        {QStringLiteral("playing"), track},
        {QStringLiteral("settings"), settings}};
    return QJsonDocument{input}.toJson(QJsonDocument::Compact);
}

namespace {

void runPluginProgram(const Plugin& plugin, const QStringList& run, const int timeout_seconds,
                      QByteArray input, QObject* context, std::function<void(PluginOutcome)> done);

} // namespace

void runPluginAction(const Plugin& plugin, const PluginAction& action, QByteArray input,
                     QObject* context, std::function<void(PluginOutcome)> done) {
    runPluginProgram(plugin, action.run, action.timeout_seconds, std::move(input), context,
                     std::move(done));
}

void runPluginPanel(const Plugin& plugin, const PluginPanel& panel, QByteArray input,
                    QObject* context, std::function<void(PluginOutcome)> done) {
    runPluginProgram(plugin, panel.run, panel.timeout_seconds, std::move(input), context,
                     std::move(done));
}

namespace {

void runPluginProgram(const Plugin& plugin, const QStringList& run, const int timeout_seconds,
                      QByteArray input, QObject* context, std::function<void(PluginOutcome)> done) {
    auto* process = new QProcess(context);
    process->setWorkingDirectory(plugin.folder);
    auto program = run.front();
    if (program.startsWith(QStringLiteral("./"))) {
        program = QDir{plugin.folder}.filePath(program.mid(2));
    }
    process->setProgram(program);
    process->setArguments(run.mid(1));
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
        timer, &QTimer::timeout, process, [process, finish, seconds = timeout_seconds] {
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
    timer->start(timeout_seconds * 1000);
}

} // namespace

} // namespace trackknife::bench
