// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::bench {

// ADR-0288: a plugin is a program apart, in a folder of its own with a
// plugin.json, that Melody runs and talks to in JSON. Level 1: actions in
// the context menus of track lists and the library.
struct PluginAction {
    enum class Scope : std::uint8_t { albums, tracks };
    // What the output is for; `list` is JSON for a window of rows.
    enum class Result : std::uint8_t { none, notify, copy, notify_and_copy, list };

    QString id;
    QString label;
    // One run per album chosen, or one with every track.
    Scope scope{Scope::albums};
    // The program and its arguments, without a shell; a first word starting
    // with "./" is in the plugin's folder.
    QStringList run;
    Result result{Result::notify};
    int timeout_seconds{120};
};

// ADR-0291: a side panel the plugin fills for what is playing -- run when
// the track changes and the panel is open, its answer drawn by Melody.
struct PluginPanel {
    QString id;
    QString label;
    QStringList run;
    int timeout_seconds{30};
};

// What a plugin asks to be set, shown under it in Settings › Plugins and
// handed to it with every run: an address, an account's token.
struct PluginSetting {
    enum class Kind : std::uint8_t { text, secret, toggle, choice };

    QString key;
    QString label;
    QString help;
    Kind kind{Kind::text};
    // Its value until one is set: text, a choice, or on/off; a secret has none.
    QVariant fallback;
    QStringList choices;
};

struct Plugin {
    // The folder's name: what it is enabled by.
    QString name;
    QString title;
    QString description;
    QString folder;
    std::vector<PluginAction> actions;
    std::vector<PluginPanel> panels;
    std::vector<PluginSetting> settings;
};

// A manifest read, or why it cannot be: every key checked, none guessed.
[[nodiscard]] std::optional<Plugin> parsePluginManifest(const QByteArray& json,
                                                        const QString& folder, QString* problem);

// The folders plugins are looked for in: melody/plugins under the user's
// data folder first, then the system's ($XDG_DATA_DIRS), then the one
// installed beside the program. $MELODY_PLUGIN_PATH, set, names them instead.
[[nodiscard]] QStringList pluginFolders();

struct FoundPlugin {
    // The folder's name, and the plugin when its manifest reads.
    QString name;
    QString folder;
    std::optional<Plugin> plugin;
    QString problem;
};
// Every plugin found, by name, the user's before the system's of a name.
[[nodiscard]] std::vector<FoundPlugin> discoverPlugins();

// Settings key a plugin is enabled under; off until it is (ADR-0288).
[[nodiscard]] QString pluginEnabledKey(const QString& name);
[[nodiscard]] bool pluginEnabled(const QString& name);
// The plugins found, read and enabled.
[[nodiscard]] std::vector<Plugin> enabledPlugins();

// A setting's place among Melody's settings. A secret's is a draft's only:
// saved, it goes to pluginSecretsFile(), readable by its owner alone, never
// with the other settings.
[[nodiscard]] QString pluginSettingKey(const QString& plugin, const QString& key);
[[nodiscard]] QString pluginSecretKey(const QString& plugin, const QString& key);
[[nodiscard]] bool isPluginSecretKey(const QString& settings_key);
[[nodiscard]] QString pluginSecretsFile();
void savePluginSecret(const QString& settings_key, const QString& value);
// What it is set to, else its fallback.
[[nodiscard]] QVariant pluginSettingValue(const QString& plugin, const PluginSetting& setting);
// Every one of the plugin's, by key, as a run is given them.
[[nodiscard]] QJsonObject pluginSettings(const Plugin& plugin);

// What a plugin is told about the tracks it is run for.
struct PluginTrack {
    std::string path;
    std::string title;
    std::string artist;
    std::string album;
    std::string album_artist;
    std::string date;
    std::string track_number;
};

struct PluginEngine {
    QString name;
    QString address;
    bool here{true};
};

// The tracks as albums -- by album artist, album and date, else by folder --
// each with its folder and that folder relative to the music folder it is
// in. Pure, for tests.
struct PluginAlbum {
    std::string artist;
    std::string album;
    std::string date;
    std::string music_folder;
    std::string folder;
    std::string relative_folder;
    std::vector<PluginTrack> tracks;
};
[[nodiscard]] std::vector<PluginAlbum> pluginAlbums(std::vector<PluginTrack> tracks,
                                                    const std::vector<std::string>& music_folders);

// The JSON a run is given on stdin, its settings among it.
[[nodiscard]] QByteArray pluginInput(const PluginAction& action, const PluginEngine& engine,
                                     const std::vector<PluginAlbum>& albums,
                                     const std::vector<std::string>& music_folders,
                                     const QJsonObject& settings = {});

// What a `list` action answers: a heading and rows, each of text, a detail
// and a link -- the parts Melody draws, none of the plugin's own.
struct PluginList {
    struct Item {
        QString text;
        QString detail;
        // http or https only; empty, none.
        QString url;
    };
    QString title;
    std::vector<Item> items;
};
// Its output read, or why it cannot be.
[[nodiscard]] std::optional<PluginList> parsePluginList(const QByteArray& json, QString* problem);

// ADR-0291: what a panel answers -- a heading and parts, each Melody's to
// draw: a heading, a paragraph, an item (text, detail beneath, a link) or an
// image (a file the plugin keeps, a link on it).
struct PluginPanelContent {
    struct Part {
        enum class Kind : std::uint8_t { heading, paragraph, item, image };
        Kind kind{Kind::item};
        QString text;
        QString detail;
        // http or https only.
        QString url;
        // An image's, an absolute path on this computer.
        QString image;
    };
    QString title;
    std::vector<Part> parts;
};
[[nodiscard]] std::optional<PluginPanelContent> parsePluginPanel(const QByteArray& json,
                                                                 QString* problem);

// The JSON a panel's run is given: what plays -- null when nothing does --
// the engine it plays on and the plugin's settings.
[[nodiscard]] QByteArray pluginPanelInput(const PluginPanel& panel, const PluginEngine& engine,
                                          const std::optional<PluginTrack>& playing,
                                          std::optional<std::int64_t> duration_ms,
                                          const std::vector<std::string>& music_folders,
                                          const QJsonObject& settings);

struct PluginOutcome {
    bool succeeded{false};
    // What it printed, trimmed; or, when it failed, why.
    QString text;
};

// Runs the action once with `input`, off the UI thread, and answers on
// `context`'s: its stdout, or its failure -- a non-zero exit with what it
// said on stderr, a program not found, its time limit. Stopped when
// `context` goes.
void runPluginAction(const Plugin& plugin, const PluginAction& action, QByteArray input,
                     QObject* context, std::function<void(PluginOutcome)> done);
// The same for a panel. Deleting `context` stops it.
void runPluginPanel(const Plugin& plugin, const PluginPanel& panel, QByteArray input,
                    QObject* context, std::function<void(PluginOutcome)> done);

} // namespace trackknife::bench
