// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <functional>
#include <map>
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
    // ADR-0301: `"library": ["artist-albums"]` -- told the playing
    // artist's albums in the library.
    bool artist_albums{false};
    // ADR-0302: `"refresh": ["listening-room"]` -- asked again whenever the
    // listening room changes, not only with the track.
    bool refresh_room{false};
    // ADR-0303: `"always": true` -- asked with nothing playing too, `playing`
    // then null: a panel of controls, not about the track.
    bool always{false};
};

// ADR-0293: something a plugin finds for Melody when Melody has none --
// lyrics, first: asked about what plays when the engine finds no lyrics in
// its files, a run for each question.
struct PluginProvider {
    QString id;
    // What it finds: "lyrics".
    QString provides;
    QStringList run;
    int timeout_seconds{30};
};

// ADR-0295: a program a plugin runs on its own -- when albums come into the
// library, when the library changes, every so often, when Melody starts, or
// when asked from the Workspace menu -- nothing chosen and nothing playing.
struct PluginTrigger {
    enum class When : std::uint8_t { albums_added, library_changed, every, start, hand };

    QString id;
    // Its entry under Workspace › Run, when it runs by hand.
    QString label;
    std::vector<When> on;
    // When it runs every so often: so many minutes, or the setting holding
    // them -- none, or not a number, and it does not.
    int every_minutes{0};
    QString every_setting;
    QStringList run;
    // What it prints, said as a notification; else only a failure is said.
    bool notify{false};
    // ADR-0302: and put on the clipboard -- an invite, to paste to a friend.
    bool copy{false};
    int timeout_seconds{600};
};
// How a trigger's `on` is written: "albums-added", "library-changed", ….
[[nodiscard]] QString pluginTriggerWhenName(PluginTrigger::When when);

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
    // ADR-0300: the key of a toggle listed before it -- an optional feature
    // -- without which it is greyed out; empty, it always applies.
    QString needs;
    // A page about it, http or https, shown beside it.
    QString link;
};

struct Plugin {
    // The folder's name: what it is enabled by.
    QString name;
    QString title;
    QString description;
    QString folder;
    std::vector<PluginAction> actions;
    std::vector<PluginPanel> panels;
    std::vector<PluginProvider> providers;
    std::vector<PluginTrigger> triggers;
    std::vector<PluginSetting> settings;
};

// How many minutes apart a trigger runs, its setting read; 0, it does not.
[[nodiscard]] int pluginTriggerMinutes(const Plugin& plugin, const PluginTrigger& trigger);

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

// The JSON a trigger's run is given: what started it, the engine holding the
// library, its music folders, the plugin's settings -- and, for albums come
// in, the albums, as an action is given them.
[[nodiscard]] QByteArray pluginTriggerInput(const PluginTrigger& trigger, PluginTrigger::When when,
                                            const PluginEngine& engine,
                                            const std::vector<PluginAlbum>& albums,
                                            const std::vector<std::string>& music_folders,
                                            const QJsonObject& settings);

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
// draw: a heading, a paragraph, an item (text, detail beneath, a link), an
// image (a file the plugin keeps, a link on it) or a link alone -- links side
// by side are drawn as a row of small buttons, what each is about their tip.
struct PluginPanelContent {
    // ADR-0304: how a button is drawn -- the one to press in the accent, an
    // ordinary one, or a quiet one that keeps out of the way (Stop, Remove).
    enum class Role : std::uint8_t { normal, primary, quiet };
    // ADR-0303: a button a panel's program answers, run again when it is
    // pressed: `action` and `value` are what it is told.
    struct Button {
        QString text;
        QString action;
        QString value;
        // Drawn as chosen -- a vote of one's own -- or as the one to press.
        bool on{false};
        Role role{Role::normal};
    };
    struct Part {
        enum class Kind : std::uint8_t {
            heading,
            paragraph,
            item,
            image,
            link,
            input,
            toggle,
            button,
            // ADR-0304: drawn as Melody draws its own -- a track's row, a
            // status line, the people somewhere.
            track,
            status,
            people,
        };
        Kind kind{Kind::item};
        QString text;
        QString detail;
        // http or https only.
        QString url;
        // An image's, an absolute path on this computer.
        QString image;
        // ADR-0303. An input's or a toggle's key -- what its value is told as.
        QString key;
        // An input's text, or placeholder; a button's action and value; a
        // toggle's state.
        QString value;
        QString placeholder;
        QString action;
        bool secret{false};
        // A toggle's state; a track that plays; a status that is live.
        bool on{false};
        // An item's or a track's buttons, at its right.
        std::vector<Button> buttons;
        // ADR-0304. A track's artist, under its title (`text`).
        QString artist;
        // A track's cover, an absolute path on this computer.
        QString cover;
        // The people a `people` part names.
        std::vector<QString> people;
        // A button's.
        Role role{Role::normal};
    };
    QString title;
    std::vector<Part> parts;
};
[[nodiscard]] std::optional<PluginPanelContent> parsePluginPanel(const QByteArray& json,
                                                                 QString* problem);

// ADR-0301: a track as the library has it -- its path and every tag, named
// as queries name them (a known field folded, `musicbrainzalbumartistid`;
// another as spelled, lowercase), the values as written.
using PluginTags = std::map<std::string, std::vector<std::string>>;
struct PluginLibraryTrack {
    std::string path;
    PluginTags tags;
};
// What a panel is told of the library: the playing track's tags, and --
// for a panel that asks -- the tracks of the playing artist's albums.
struct PluginPanelLibrary {
    PluginTags tags;
    std::optional<std::vector<PluginLibraryTrack>> artist_tracks;
};
// The query that finds the albums of the artist a track's tags name: by
// album artist, else artist, and by MusicBrainz id where the tags have one.
// Empty when they name none.
[[nodiscard]] QString pluginArtistQuery(const PluginTags& tags);
// Those tracks as albums -- one for each release group, else album and
// album artist -- oldest first, with the tags every track of each shares.
[[nodiscard]] QJsonArray pluginArtistAlbums(const std::vector<PluginLibraryTrack>& tracks,
                                            const std::vector<std::string>& music_folders);

// The JSON a panel's run is given: what plays -- null when nothing does --
// its tags, the engine it plays on, the plugin's settings and, asked for,
// the artist's albums.
// ADR-0303: what was pressed in a panel -- a button's action and value, a
// toggle as its key and "on" or "off" -- and every input's text as it stood.
struct PluginPanelAction {
    QString id;
    QString value;
    QJsonObject inputs;
};
[[nodiscard]] QByteArray pluginPanelInput(const PluginPanel& panel, const PluginEngine& engine,
                                          const std::optional<PluginTrack>& playing,
                                          std::optional<std::int64_t> duration_ms,
                                          const std::vector<std::string>& music_folders,
                                          const QJsonObject& settings,
                                          const PluginPanelLibrary& library = {},
                                          const std::optional<PluginPanelAction>& action = {});

// What a lyrics provider answers: the lyrics, as LRC or plain text, and
// what it names their source; none found, nullopt. Its answer unread, why.
struct PluginLyrics {
    QString text;
    QString source;
};
[[nodiscard]] std::optional<std::optional<PluginLyrics>> parsePluginLyrics(const QByteArray& json,
                                                                           QString* problem);
// The JSON a provider's run is given: what it is asked to find, about what
// plays, the engine and its settings.
[[nodiscard]] QByteArray pluginProviderInput(const PluginProvider& provider,
                                             const PluginEngine& engine, const PluginTrack& playing,
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
// And for a provider.
void runPluginProvider(const Plugin& plugin, const PluginProvider& provider, QByteArray input,
                       QObject* context, std::function<void(PluginOutcome)> done);
// And for a trigger.
void runPluginTrigger(const Plugin& plugin, const PluginTrigger& trigger, QByteArray input,
                      QObject* context, std::function<void(PluginOutcome)> done);

} // namespace trackknife::bench
