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
#include <QUrl>

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <utility>

namespace trackknife::bench {
namespace {

constexpr int manifest_schema = 1;
constexpr int input_schema = 1;
// ADR-0295: a trigger runs at most a week apart, and for at most six hours
// -- a whole library converted.
constexpr int max_trigger_minutes = 7 * 24 * 60;
constexpr int max_trigger_seconds = 6 * 3600;

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
// ADR-0295: a plugin's triggers, each key checked.
[[nodiscard]] std::optional<QString> readTriggers(const QJsonArray& entries, Plugin& plugin) {
    static const QStringList keys{QStringLiteral("id"),     QStringLiteral("label"),
                                  QStringLiteral("on"),     QStringLiteral("every"),
                                  QStringLiteral("run"),    QStringLiteral("result"),
                                  QStringLiteral("timeout")};
    static const std::array whens{PluginTrigger::When::albums_added,
                                  PluginTrigger::When::library_changed, PluginTrigger::When::every,
                                  PluginTrigger::When::start, PluginTrigger::When::hand};
    for (const auto& value : entries) {
        if (!value.isObject()) {
            return QStringLiteral("a trigger is not an object");
        }
        const auto entry = value.toObject();
        for (const auto& key : entry.keys()) {
            if (!keys.contains(key)) {
                return QStringLiteral("a trigger has an unknown key \"%1\"").arg(key);
            }
        }
        PluginTrigger trigger;
        trigger.id = entry.value(QStringLiteral("id")).toString().trimmed();
        if (trigger.id.isEmpty()) {
            return QStringLiteral("a trigger needs an id");
        }
        if (std::ranges::any_of(plugin.triggers,
                                [&trigger](const auto& other) { return other.id == trigger.id; })) {
            return QStringLiteral("trigger \"%1\" is there twice").arg(trigger.id);
        }
        trigger.label = entry.value(QStringLiteral("label")).toString().trimmed();
        // One word, or a list of them.
        auto on = entry.value(QStringLiteral("on"));
        const auto words = on.isString() ? QJsonArray{on} : on.toArray();
        for (const auto& word : words) {
            const auto found = std::ranges::find(whens, word.toString(), pluginTriggerWhenName);
            if (found == whens.end()) {
                return QStringLiteral("trigger \"%1\": \"on\" is albums-added, library-changed, "
                                      "every, start or hand")
                    .arg(trigger.id);
            }
            if (!std::ranges::contains(trigger.on, *found)) {
                trigger.on.push_back(*found);
            }
        }
        if (trigger.on.empty()) {
            return QStringLiteral("trigger \"%1\" says nothing it runs on").arg(trigger.id);
        }
        const bool every = std::ranges::contains(trigger.on, PluginTrigger::When::every);
        const auto interval = entry.value(QStringLiteral("every"));
        if (every != !interval.isUndefined()) {
            return QStringLiteral("trigger \"%1\": \"every\" is given when it runs on every, "
                                  "and only then")
                .arg(trigger.id);
        }
        if (every && interval.isString()) {
            trigger.every_setting = interval.toString();
            if (!std::ranges::contains(plugin.settings, trigger.every_setting,
                                       &PluginSetting::key)) {
                return QStringLiteral("trigger \"%1\": \"every\" names no setting of the plugin")
                    .arg(trigger.id);
            }
        } else if (every) {
            trigger.every_minutes = interval.toInt(0);
            if (trigger.every_minutes < 1 || trigger.every_minutes > max_trigger_minutes) {
                return QStringLiteral("trigger \"%1\": \"every\" is 1 to %2 minutes, or a "
                                      "setting's key")
                    .arg(trigger.id)
                    .arg(max_trigger_minutes);
            }
        }
        if (std::ranges::contains(trigger.on, PluginTrigger::When::hand) &&
            trigger.label.isEmpty()) {
            return QStringLiteral("trigger \"%1\" runs by hand and needs a label").arg(trigger.id);
        }
        for (const auto& word : entry.value(QStringLiteral("run")).toArray()) {
            if (!word.isString() || word.toString().isEmpty()) {
                return QStringLiteral("trigger \"%1\": \"run\" is a list of words").arg(trigger.id);
            }
            trigger.run.push_back(word.toString());
        }
        if (trigger.run.isEmpty()) {
            return QStringLiteral("trigger \"%1\" has nothing to run").arg(trigger.id);
        }
        const auto result = entry.value(QStringLiteral("result")).toString(QStringLiteral("none"));
        if (!QStringList{QStringLiteral("none"), QStringLiteral("notify"), QStringLiteral("copy"),
                         QStringLiteral("notify-and-copy")}
                 .contains(result)) {
            return QStringLiteral(
                       "trigger \"%1\": \"result\" is none, notify, copy or notify-and-copy")
                .arg(trigger.id);
        }
        trigger.notify = result.startsWith(QStringLiteral("notify"));
        trigger.copy = result.endsWith(QStringLiteral("copy"));
        if (entry.contains(QStringLiteral("timeout"))) {
            const auto seconds = entry.value(QStringLiteral("timeout")).toInt(0);
            if (seconds < 1 || seconds > max_trigger_seconds) {
                return QStringLiteral("trigger \"%1\": \"timeout\" is 1 to %2 seconds")
                    .arg(trigger.id)
                    .arg(max_trigger_seconds);
            }
            trigger.timeout_seconds = seconds;
        }
        plugin.triggers.push_back(std::move(trigger));
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<QString> readSettings(const QJsonArray& listed,
                                                  std::vector<PluginSetting>& settings) {
    static const QStringList setting_keys{QStringLiteral("key"),     QStringLiteral("label"),
                                          QStringLiteral("kind"),    QStringLiteral("default"),
                                          QStringLiteral("choices"), QStringLiteral("help"),
                                          QStringLiteral("requires"), QStringLiteral("link")};
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
        if (entry.contains(QStringLiteral("requires"))) {
            setting.needs = entry.value(QStringLiteral("requires")).toString();
            const auto toggle = std::ranges::find(settings, setting.needs, &PluginSetting::key);
            if (toggle == settings.end() || toggle->kind != PluginSetting::Kind::toggle) {
                return wrong("\"requires\" is the key of a toggle listed before it");
            }
        }
        if (entry.contains(QStringLiteral("link"))) {
            setting.link = entry.value(QStringLiteral("link")).toString();
            const QUrl url{setting.link};
            if (!url.isValid() || url.host().isEmpty() ||
                (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https"))) {
                return wrong("its link is an http or https address");
            }
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
                                   QStringLiteral("panels"),      QStringLiteral("providers"),
                                   QStringLiteral("triggers"),    QStringLiteral("settings")};
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
    const auto providers = object.value(QStringLiteral("providers"));
    const auto triggers = object.value(QStringLiteral("triggers"));
    if ((!actions.isUndefined() && !actions.isArray()) ||
        (!panels.isUndefined() && !panels.isArray()) ||
        (!providers.isUndefined() && !providers.isArray()) ||
        (!triggers.isUndefined() && !triggers.isArray())) {
        return fail(
            QStringLiteral("\"actions\", \"panels\", \"providers\" and \"triggers\" are lists"));
    }
    if (actions.toArray().isEmpty() && panels.toArray().isEmpty() &&
        providers.toArray().isEmpty() && triggers.toArray().isEmpty()) {
        return fail(QStringLiteral("plugin.json has no actions, panels, providers or triggers"));
    }
    static const QStringList provider_keys{QStringLiteral("id"), QStringLiteral("provides"),
                                           QStringLiteral("run"), QStringLiteral("timeout")};
    for (const auto& value : providers.toArray()) {
        if (!value.isObject()) {
            return fail(QStringLiteral("a provider is not an object"));
        }
        const auto entry = value.toObject();
        for (const auto& key : entry.keys()) {
            if (!provider_keys.contains(key)) {
                return fail(QStringLiteral("a provider has an unknown key \"%1\"").arg(key));
            }
        }
        PluginProvider provider;
        provider.id = entry.value(QStringLiteral("id")).toString().trimmed();
        provider.provides = entry.value(QStringLiteral("provides")).toString();
        if (provider.id.isEmpty()) {
            return fail(QStringLiteral("a provider needs an id"));
        }
        if (provider.provides != QStringLiteral("lyrics")) {
            return fail(QStringLiteral("provider \"%1\": \"provides\" is lyrics").arg(provider.id));
        }
        for (const auto& word : entry.value(QStringLiteral("run")).toArray()) {
            if (!word.isString() || word.toString().isEmpty()) {
                return fail(
                    QStringLiteral("provider \"%1\": \"run\" is a list of words").arg(provider.id));
            }
            provider.run.push_back(word.toString());
        }
        if (provider.run.isEmpty()) {
            return fail(QStringLiteral("provider \"%1\" has nothing to run").arg(provider.id));
        }
        if (entry.contains(QStringLiteral("timeout"))) {
            const auto seconds = entry.value(QStringLiteral("timeout")).toInt(0);
            if (seconds < 1 || seconds > 120) {
                return fail(QStringLiteral("provider \"%1\": \"timeout\" is 1 to 120 seconds")
                                .arg(provider.id));
            }
            provider.timeout_seconds = seconds;
        }
        plugin.providers.push_back(std::move(provider));
    }
    static const QStringList panel_keys{QStringLiteral("id"), QStringLiteral("label"),
                                        QStringLiteral("run"), QStringLiteral("timeout"),
                                        QStringLiteral("library"), QStringLiteral("refresh"),
                                        QStringLiteral("always")};
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
        if (entry.contains(QStringLiteral("library"))) {
            const auto asked = entry.value(QStringLiteral("library"));
            for (const auto& what : asked.toArray()) {
                if (what.toString() != QStringLiteral("artist-albums")) {
                    return fail(QStringLiteral("panel \"%1\": \"library\" is a list of what it "
                                               "is told: artist-albums")
                                    .arg(panel.id));
                }
                panel.artist_albums = true;
            }
            if (!asked.isArray()) {
                return fail(QStringLiteral("panel \"%1\": \"library\" is a list").arg(panel.id));
            }
        }
        if (entry.contains(QStringLiteral("always"))) {
            if (!entry.value(QStringLiteral("always")).isBool()) {
                return fail(QStringLiteral("panel \"%1\": \"always\" is true or false").arg(panel.id));
            }
            panel.always = entry.value(QStringLiteral("always")).toBool();
        }
        if (entry.contains(QStringLiteral("refresh"))) {
            const auto asked = entry.value(QStringLiteral("refresh"));
            if (!asked.isArray()) {
                return fail(QStringLiteral("panel \"%1\": \"refresh\" is a list").arg(panel.id));
            }
            for (const auto& when : asked.toArray()) {
                if (when.toString() != QStringLiteral("listening-room")) {
                    return fail(QStringLiteral("panel \"%1\": \"refresh\" is a list of when it is "
                                               "asked again: listening-room")
                                    .arg(panel.id));
                }
                panel.refresh_room = true;
            }
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
    // After the settings: a trigger's interval can be one of them.
    if (auto why = readTriggers(triggers.toArray(), plugin)) {
        return fail(*why);
    }
    return plugin;
}

QString pluginTriggerWhenName(const PluginTrigger::When when) {
    switch (when) {
    case PluginTrigger::When::albums_added:
        return QStringLiteral("albums-added");
    case PluginTrigger::When::library_changed:
        return QStringLiteral("library-changed");
    case PluginTrigger::When::every:
        return QStringLiteral("every");
    case PluginTrigger::When::start:
        return QStringLiteral("start");
    case PluginTrigger::When::hand:
        return QStringLiteral("hand");
    }
    return {};
}

int pluginTriggerMinutes(const Plugin& plugin, const PluginTrigger& trigger) {
    if (!std::ranges::contains(trigger.on, PluginTrigger::When::every)) {
        return 0;
    }
    if (trigger.every_setting.isEmpty()) {
        return trigger.every_minutes;
    }
    const auto setting =
        std::ranges::find(plugin.settings, trigger.every_setting, &PluginSetting::key);
    if (setting == plugin.settings.end()) {
        return 0;
    }
    bool number = false;
    const auto minutes =
        pluginSettingValue(plugin.name, *setting).toString().trimmed().toInt(&number);
    return number && minutes > 0 && minutes <= max_trigger_minutes ? minutes : 0;
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

namespace {

// A link is a web page's -- or, ADR-0302, one of Melody's own, a vote at a
// listening party: nothing else is opened from a plugin.
[[nodiscard]] bool openableUrl(const QString& url) {
    return url.startsWith(QStringLiteral("https://")) ||
           url.startsWith(QStringLiteral("http://")) ||
           url.startsWith(QStringLiteral("melody://"));
}

} // namespace

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
    using Role = PluginPanelContent::Role;
    // ADR-0304: a button's role, normal when it names none.
    const auto role_of = [](const QJsonObject& button) -> std::optional<Role> {
        const auto role = button.value(QStringLiteral("role")).toString();
        if (role.isEmpty() || role == QStringLiteral("normal")) {
            return Role::normal;
        }
        if (role == QStringLiteral("primary")) {
            return Role::primary;
        }
        if (role == QStringLiteral("quiet")) {
            return Role::quiet;
        }
        return std::nullopt;
    };
    // An item's or a track's buttons; false when one is not a button.
    const auto read_buttons = [&role_of](const QJsonObject& part,
                                         PluginPanelContent::Part& read) {
        for (const auto& listed : part.value(QStringLiteral("buttons")).toArray()) {
            const auto button = listed.toObject();
            const auto role = role_of(button);
            PluginPanelContent::Button made{
                .text = button.value(QStringLiteral("text")).toString(),
                .action = button.value(QStringLiteral("action")).toString(),
                .value = button.value(QStringLiteral("value")).toString(),
                .on = button.value(QStringLiteral("on")).toBool(false),
                .role = role.value_or(Role::normal),
                .url = button.value(QStringLiteral("url")).toString()};
            // An action to run, or a page to open -- one Melody opens.
            if (!listed.isObject() || !role || made.text.isEmpty() ||
                made.action.isEmpty() == made.url.isEmpty() ||
                (!made.url.isEmpty() && !openableUrl(made.url))) {
                return false;
            }
            read.buttons.push_back(std::move(made));
        }
        return true;
    };
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
        } else if (part.contains(QStringLiteral("link"))) {
            // ADR-0291: a link alone, drawn as a small button among its
            // neighbours; `detail` says what it is about.
            read.kind = Kind::link;
            read.text = part.value(QStringLiteral("link")).toString();
            read.detail = part.value(QStringLiteral("detail")).toString();
            read.url = part.value(QStringLiteral("url")).toString();
            allowed = {QStringLiteral("link"), QStringLiteral("detail"), QStringLiteral("url")};
            if (read.url.isEmpty()) {
                return fail(QStringLiteral("a link needs its url: %1").arg(read.text));
            }
        } else if (part.contains(QStringLiteral("input"))) {
            // ADR-0303: a field to type in, its text told with every action.
            read.kind = Kind::input;
            read.key = part.value(QStringLiteral("input")).toString();
            read.text = part.value(QStringLiteral("label")).toString();
            read.value = part.value(QStringLiteral("value")).toString();
            read.placeholder = part.value(QStringLiteral("placeholder")).toString();
            read.secret = part.value(QStringLiteral("secret")).toBool(false);
            read.action = part.value(QStringLiteral("action")).toString();
            allowed = {QStringLiteral("input"),       QStringLiteral("label"),
                       QStringLiteral("value"),       QStringLiteral("placeholder"),
                       QStringLiteral("secret"),      QStringLiteral("action")};
            if (read.key.isEmpty()) {
                return fail(QStringLiteral("an input needs its key"));
            }
        } else if (part.contains(QStringLiteral("toggle"))) {
            read.kind = Kind::toggle;
            read.key = part.value(QStringLiteral("toggle")).toString();
            read.text = part.value(QStringLiteral("label")).toString();
            read.detail = part.value(QStringLiteral("detail")).toString();
            read.on = part.value(QStringLiteral("on")).toBool(false);
            allowed = {QStringLiteral("toggle"), QStringLiteral("label"), QStringLiteral("detail"),
                       QStringLiteral("on")};
            if (read.key.isEmpty()) {
                return fail(QStringLiteral("a toggle needs its key"));
            }
        } else if (part.contains(QStringLiteral("button"))) {
            read.kind = Kind::button;
            read.text = part.value(QStringLiteral("button")).toString();
            read.action = part.value(QStringLiteral("action")).toString();
            read.value = part.value(QStringLiteral("value")).toString();
            read.on = part.value(QStringLiteral("primary")).toBool(false);
            allowed = {QStringLiteral("button"), QStringLiteral("action"), QStringLiteral("value"),
                       QStringLiteral("primary"), QStringLiteral("role")};
            if (read.action.isEmpty()) {
                return fail(QStringLiteral("a button needs its action: %1").arg(read.text));
            }
            const auto role = role_of(part);
            if (!role) {
                return fail(QStringLiteral("a button's role is primary, normal or quiet"));
            }
            // `primary: true`, as ADR-0303 had it, is the primary role.
            read.role = read.on && *role == Role::normal ? Role::primary : *role;
            read.on = read.role == Role::primary;
        } else if (part.contains(QStringLiteral("track"))) {
            // ADR-0304: a track's row, its title over its artist, as Melody's.
            read.kind = Kind::track;
            read.text = part.value(QStringLiteral("track")).toString();
            read.artist = part.value(QStringLiteral("artist")).toString();
            read.detail = part.value(QStringLiteral("detail")).toString();
            read.cover = part.value(QStringLiteral("cover")).toString();
            read.url = part.value(QStringLiteral("url")).toString();
            read.on = part.value(QStringLiteral("playing")).toBool(false);
            allowed = {QStringLiteral("track"),  QStringLiteral("artist"), QStringLiteral("detail"),
                       QStringLiteral("cover"),  QStringLiteral("url"),    QStringLiteral("playing"),
                       QStringLiteral("buttons")};
            if (!read.cover.isEmpty() && !QDir::isAbsolutePath(read.cover)) {
                return fail(QStringLiteral("a cover is a file's absolute path: %1").arg(read.cover));
            }
            if (!read_buttons(part, read)) {
                return fail(QStringLiteral(
                    "a row's button is {text, action or url, value?, on?, role?}"));
            }
        } else if (part.contains(QStringLiteral("status"))) {
            read.kind = Kind::status;
            read.text = part.value(QStringLiteral("status")).toString();
            read.detail = part.value(QStringLiteral("detail")).toString();
            read.on = part.value(QStringLiteral("live")).toBool(false);
            allowed = {QStringLiteral("status"), QStringLiteral("detail"), QStringLiteral("live"),
                       QStringLiteral("buttons")};
            if (!read_buttons(part, read)) {
                return fail(QStringLiteral(
                    "a status's button is {text, action or url, value?, on?, role?}"));
            }
        } else if (part.contains(QStringLiteral("people"))) {
            read.kind = Kind::people;
            allowed = {QStringLiteral("people")};
            const auto named = part.value(QStringLiteral("people"));
            if (!named.isArray()) {
                return fail(QStringLiteral("people are a list of names"));
            }
            for (const auto& each : named.toArray()) {
                if (!each.isString()) {
                    return fail(QStringLiteral("people are a list of names"));
                }
                if (const auto name = each.toString().trimmed(); !name.isEmpty()) {
                    read.people.push_back(name);
                }
            }
        } else if (part.contains(QStringLiteral("text"))) {
            read.kind = Kind::item;
            read.text = part.value(QStringLiteral("text")).toString();
            read.detail = part.value(QStringLiteral("detail")).toString();
            read.url = part.value(QStringLiteral("url")).toString();
            allowed = {QStringLiteral("text"), QStringLiteral("detail"), QStringLiteral("url"),
                       QStringLiteral("buttons")};
            if (!read_buttons(part, read)) {
                return fail(QStringLiteral(
                    "an item's button is {text, action or url, value?, on?, role?}"));
            }
        } else {
            return fail(QStringLiteral("a part is a heading, paragraph, text, track, status, "
                                       "people, link, image, input, toggle or button"));
        }
        for (const auto& key : part.keys()) {
            if (!allowed.contains(key)) {
                return fail(QStringLiteral("a part has an unknown key \"%1\"").arg(key));
            }
        }
        if (!read.url.isEmpty() && !openableUrl(read.url)) {
            return fail(QStringLiteral("a link is not http, https or melody: %1").arg(read.url));
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

namespace {

// Albums as a run is given them, each with its tracks.
[[nodiscard]] QJsonArray albumsJson(const std::vector<PluginAlbum>& albums,
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
    return listed;
}

[[nodiscard]] QJsonObject engineOf(const PluginEngine& engine) {
    return QJsonObject{{QStringLiteral("name"), engine.name},
                       {QStringLiteral("address"), engine.address},
                       {QStringLiteral("here"), engine.here}};
}

} // namespace

QByteArray pluginInput(const PluginAction& action, const PluginEngine& engine,
                       const std::vector<PluginAlbum>& albums,
                       const std::vector<std::string>& music_folders, const QJsonObject& settings) {
    const QJsonObject input{{QStringLiteral("schema"), input_schema},
                            {QStringLiteral("action"), action.id},
                            {QStringLiteral("engine"), engineOf(engine)},
                            {QStringLiteral("albums"), albumsJson(albums, music_folders)},
                            {QStringLiteral("settings"), settings}};
    return QJsonDocument{input}.toJson(QJsonDocument::Compact);
}

QByteArray pluginTriggerInput(const PluginTrigger& trigger, const PluginTrigger::When when,
                              const PluginEngine& engine, const std::vector<PluginAlbum>& albums,
                              const std::vector<std::string>& music_folders,
                              const QJsonObject& settings) {
    QJsonArray folders;
    for (const auto& folder : music_folders) {
        folders.push_back(text(folder));
    }
    QJsonObject input{{QStringLiteral("schema"), input_schema},
                      {QStringLiteral("trigger"), trigger.id},
                      {QStringLiteral("on"), pluginTriggerWhenName(when)},
                      {QStringLiteral("engine"), engineOf(engine)},
                      {QStringLiteral("music_folders"), folders},
                      {QStringLiteral("settings"), settings}};
    if (when == PluginTrigger::When::albums_added) {
        input.insert(QStringLiteral("albums"), albumsJson(albums, music_folders));
    }
    return QJsonDocument{input}.toJson(QJsonDocument::Compact);
}

namespace {

[[nodiscard]] QJsonObject playingJson(const PluginTrack& playing,
                                      const std::optional<std::int64_t> duration_ms,
                                      const std::vector<std::string>& music_folders) {
    const auto root = musicFolderOf(playing.path, music_folders);
    return QJsonObject{
        {QStringLiteral("path"), text(playing.path)},
        {QStringLiteral("music_folder"), text(root)},
        {QStringLiteral("relative_path"), text(relativeTo(playing.path, root))},
        {QStringLiteral("title"), text(playing.title)},
        {QStringLiteral("artist"), text(playing.artist)},
        {QStringLiteral("album"), text(playing.album)},
        {QStringLiteral("album_artist"), text(playing.album_artist)},
        {QStringLiteral("date"), text(playing.date)},
        {QStringLiteral("track_number"), text(playing.track_number)},
        {QStringLiteral("duration_ms"),
         duration_ms ? QJsonValue{static_cast<double>(*duration_ms)} : QJsonValue{}}};
}

[[nodiscard]] QJsonObject engineJson(const PluginEngine& engine) {
    return QJsonObject{{QStringLiteral("name"), engine.name},
                       {QStringLiteral("address"), engine.address},
                       {QStringLiteral("here"), engine.here}};
}

} // namespace

namespace {

[[nodiscard]] QJsonObject tagsJson(const PluginTags& tags) {
    QJsonObject object;
    for (const auto& [name, values] : tags) {
        QJsonArray listed;
        for (const auto& value : values) {
            listed.push_back(text(value));
        }
        object.insert(text(name), listed);
    }
    return object;
}

[[nodiscard]] std::string firstOf(const PluginTags& tags, const char* name) {
    const auto found = tags.find(name);
    return found != tags.end() && !found->second.empty() ? found->second.front() : std::string{};
}

[[nodiscard]] QString quotedTerm(const std::string& value) {
    auto quoted = text(value);
    quoted.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + quoted + QLatin1Char('"');
}

[[nodiscard]] std::string folded(const std::string& value) {
    return text(value).simplified().toCaseFolded().toStdString();
}

} // namespace

QString pluginArtistQuery(const PluginTags& tags) {
    // The album artist where the track has one; a track without is its
    // artist's, and so are the albums of that artist's tracks without one.
    auto name = firstOf(tags, "albumartist");
    auto id = firstOf(tags, "musicbrainzalbumartistid");
    if (name.empty()) {
        name = firstOf(tags, "artist");
        id = firstOf(tags, "musicbrainzartistid");
    }
    if (name.empty()) {
        return {};
    }
    QStringList either{
        QStringLiteral("albumartist IS %1").arg(quotedTerm(name)),
        QStringLiteral("(albumartist MISSING AND artist IS %1)").arg(quotedTerm(name))};
    if (!id.empty()) {
        either << QStringLiteral("musicbrainzalbumartistid IS %1").arg(quotedTerm(id));
    }
    return either.join(QStringLiteral(" OR "));
}

QJsonArray pluginArtistAlbums(const std::vector<PluginLibraryTrack>& tracks,
                              const std::vector<std::string>& music_folders) {
    struct Album {
        std::string key;
        const PluginLibraryTrack* first{nullptr};
        PluginTags shared;
        std::size_t tracks{0};
    };
    std::vector<Album> albums;
    for (const auto& track : tracks) {
        const auto group = firstOf(track.tags, "musicbrainzreleasegroupid");
        auto artist = firstOf(track.tags, "albumartist");
        if (artist.empty()) {
            artist = firstOf(track.tags, "artist");
        }
        const auto key = !group.empty() ? "group\x1f" + group
                                        : "title\x1f" + folded(firstOf(track.tags, "album")) +
                                              "\x1f" + folded(artist);
        auto album = std::ranges::find(albums, key, &Album::key);
        if (album == albums.end()) {
            albums.push_back(Album{.key = key, .first = &track, .shared = track.tags, .tracks = 0});
            album = std::prev(albums.end());
        } else {
            // What every track of it says alike.
            std::erase_if(album->shared, [&track](const auto& field) {
                const auto found = track.tags.find(field.first);
                return found == track.tags.end() || found->second != field.second;
            });
        }
        ++album->tracks;
    }
    std::ranges::stable_sort(albums, {}, [](const Album& album) {
        return std::pair{firstOf(album.first->tags, "date"), folded(firstOf(album.first->tags, "album"))};
    });
    QJsonArray answered;
    for (const auto& album : albums) {
        const auto& path = album.first->path;
        const auto slash = path.find_last_of('/');
        const auto folder = slash == std::string::npos ? std::string{} : path.substr(0, slash);
        const auto root = musicFolderOf(folder, music_folders);
        auto artist = firstOf(album.first->tags, "albumartist");
        if (artist.empty()) {
            artist = firstOf(album.first->tags, "artist");
        }
        answered.push_back(QJsonObject{
            {QStringLiteral("album"), text(firstOf(album.first->tags, "album"))},
            {QStringLiteral("album_artist"), text(artist)},
            {QStringLiteral("date"), text(firstOf(album.first->tags, "date"))},
            {QStringLiteral("folder"), text(folder)},
            {QStringLiteral("relative_folder"), text(relativeTo(folder, root))},
            {QStringLiteral("tracks"), static_cast<double>(album.tracks)},
            {QStringLiteral("tags"), tagsJson(album.shared)}});
    }
    return answered;
}

QByteArray pluginPanelInput(const PluginPanel& panel, const PluginEngine& engine,
                            const std::optional<PluginTrack>& playing,
                            const std::optional<std::int64_t> duration_ms,
                            const std::vector<std::string>& music_folders,
                            const QJsonObject& settings, const PluginPanelLibrary& library,
                            const std::optional<PluginPanelAction>& action) {
    QJsonValue told{QJsonValue::Null};
    if (playing) {
        auto object = playingJson(*playing, duration_ms, music_folders);
        object.insert(QStringLiteral("tags"), tagsJson(library.tags));
        told = object;
    }
    QJsonObject input{{QStringLiteral("schema"), input_schema},
                      {QStringLiteral("panel"), panel.id},
                      {QStringLiteral("engine"), engineJson(engine)},
                      {QStringLiteral("playing"), told},
                      {QStringLiteral("settings"), settings}};
    if (action) {
        input.insert(QStringLiteral("action"), QJsonObject{{QStringLiteral("id"), action->id},
                                                           {QStringLiteral("value"), action->value}});
        input.insert(QStringLiteral("inputs"), action->inputs);
    }
    if (panel.artist_albums) {
        input.insert(QStringLiteral("artist_albums"),
                     library.artist_tracks
                         ? pluginArtistAlbums(*library.artist_tracks, music_folders)
                         : QJsonArray{});
    }
    return QJsonDocument{input}.toJson(QJsonDocument::Compact);
}

QByteArray pluginProviderInput(const PluginProvider& provider, const PluginEngine& engine,
                               const PluginTrack& playing,
                               const std::optional<std::int64_t> duration_ms,
                               const std::vector<std::string>& music_folders,
                               const QJsonObject& settings) {
    const QJsonObject input{
        {QStringLiteral("schema"), input_schema},
        {QStringLiteral("provide"), provider.provides},
        {QStringLiteral("provider"), provider.id},
        {QStringLiteral("engine"), engineJson(engine)},
        {QStringLiteral("playing"), playingJson(playing, duration_ms, music_folders)},
        {QStringLiteral("settings"), settings}};
    return QJsonDocument{input}.toJson(QJsonDocument::Compact);
}

std::optional<std::optional<PluginLyrics>> parsePluginLyrics(const QByteArray& json,
                                                             QString* problem) {
    const auto fail = [problem](QString why) -> std::optional<std::optional<PluginLyrics>> {
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
        if (key != QStringLiteral("lyrics") && key != QStringLiteral("source")) {
            return fail(QStringLiteral("its answer has an unknown key \"%1\"").arg(key));
        }
    }
    if (!object.contains(QStringLiteral("lyrics"))) {
        return fail(QStringLiteral("its answer has no lyrics, not even null"));
    }
    const auto lyrics = object.value(QStringLiteral("lyrics"));
    if (lyrics.isNull() || (lyrics.isString() && lyrics.toString().trimmed().isEmpty())) {
        return std::optional<PluginLyrics>{};
    }
    if (!lyrics.isString()) {
        return fail(QStringLiteral("its lyrics are text, or null for none"));
    }
    return std::optional{PluginLyrics{.text = lyrics.toString(),
                                      .source = object.value(QStringLiteral("source")).toString()}};
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

void runPluginTrigger(const Plugin& plugin, const PluginTrigger& trigger, QByteArray input,
                      QObject* context, std::function<void(PluginOutcome)> done) {
    runPluginProgram(plugin, trigger.run, trigger.timeout_seconds, std::move(input), context,
                     std::move(done));
}

void runPluginProvider(const Plugin& plugin, const PluginProvider& provider, QByteArray input,
                       QObject* context, std::function<void(PluginOutcome)> done) {
    runPluginProgram(plugin, provider.run, provider.timeout_seconds, std::move(input), context,
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
