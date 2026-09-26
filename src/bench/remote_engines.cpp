// SPDX-License-Identifier: GPL-3.0-only

#include "bench/remote_engines.hpp"

#include "bench/settings_dialog.hpp"

#include <QSettings>

namespace trackknife::bench {
namespace {

constexpr auto array_key = "engines/remote";

[[nodiscard]] QString text(const QSettings& settings, const char* key) {
    return settings.value(QLatin1String(key), QString{}).toString().trimmed();
}

} // namespace

QString RemoteEngineSetting::effectivePassword() const {
    return password.isEmpty() ? text(QSettings{}, SettingsDialog::engine_password_key) : password;
}

RemoteMount RemoteEngineSetting::mount() const {
    return RemoteMount{.remote_folder = music_folder.toStdString(),
                       .local_folder = reachable_at.toStdString()};
}

std::vector<RemoteEngineSetting> loadRemoteEngines() {
    QSettings settings;
    std::vector<RemoteEngineSetting> engines;
    if (!settings.contains(QLatin1String(array_key) + QStringLiteral("/size"))) {
        // An older release's one remote, taken over once.
        const auto address = text(settings, SettingsDialog::library_engine_socket_key);
        if (!address.isEmpty()) {
            engines.push_back(
                {.address = address,
                 .password = text(settings, SettingsDialog::library_engine_token_key),
                 .music_folder = text(settings, SettingsDialog::library_remote_folder_key),
                 .reachable_at = text(settings, SettingsDialog::library_remote_mount_key),
                 .id = text(settings, SettingsDialog::library_engine_id_key)});
        }
        saveRemoteEngines(engines);
        return engines;
    }
    const auto count = settings.beginReadArray(QLatin1String(array_key));
    engines.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        settings.setArrayIndex(index);
        RemoteEngineSetting engine{
            .address = settings.value("address").toString().trimmed(),
            .password = settings.value("password").toString().trimmed(),
            .music_folder = settings.value("music-folder").toString().trimmed(),
            .reachable_at = settings.value("reachable-at").toString().trimmed(),
            .id = settings.value("id").toString().trimmed()};
        if (!engine.address.isEmpty()) {
            engines.push_back(std::move(engine));
        }
    }
    settings.endArray();
    // The old keys name the first engine, and what writes them -- a release
    // before this one, the Settings page before it lists engines -- is the
    // newer word on it. The same address keeps its id; another may be
    // another engine, and its id is learned anew.
    const auto address = text(settings, SettingsDialog::library_engine_socket_key);
    if (!address.isEmpty()) {
        RemoteEngineSetting first{
            .address = address,
            .password = text(settings, SettingsDialog::library_engine_token_key),
            .music_folder = text(settings, SettingsDialog::library_remote_folder_key),
            .reachable_at = text(settings, SettingsDialog::library_remote_mount_key),
            .id = {}};
        if (engines.empty()) {
            engines.push_back(first);
        } else if (engines.front().address == address) {
            first.id = engines.front().id;
            engines.front() = first;
        } else {
            engines.front() = first;
        }
    } else if (!engines.empty()) {
        engines.erase(engines.begin());
    }
    return engines;
}

void saveRemoteEngines(const std::vector<RemoteEngineSetting>& engines) {
    QSettings settings;
    settings.remove(QLatin1String(array_key));
    settings.beginWriteArray(QLatin1String(array_key), static_cast<int>(engines.size()));
    for (int index = 0; index < static_cast<int>(engines.size()); ++index) {
        const auto& engine = engines[static_cast<std::size_t>(index)];
        settings.setArrayIndex(index);
        settings.setValue("address", engine.address);
        settings.setValue("password", engine.password);
        settings.setValue("music-folder", engine.music_folder);
        settings.setValue("reachable-at", engine.reachable_at);
        settings.setValue("id", engine.id);
    }
    settings.endArray();
    // What an older release reads, kept to the first: the one it knows of.
    const auto first = engines.empty() ? RemoteEngineSetting{} : engines.front();
    settings.setValue(QLatin1String(SettingsDialog::library_engine_socket_key), first.address);
    settings.setValue(QLatin1String(SettingsDialog::library_engine_token_key), first.password);
    settings.setValue(QLatin1String(SettingsDialog::library_remote_folder_key), first.music_folder);
    settings.setValue(QLatin1String(SettingsDialog::library_remote_mount_key), first.reachable_at);
    settings.setValue(QLatin1String(SettingsDialog::library_engine_id_key), first.id);
}

void rememberEngineId(const QString& address, const QString& id) {
    auto engines = loadRemoteEngines();
    bool changed = false;
    for (auto& engine : engines) {
        if (engine.address == address && engine.id != id) {
            engine.id = id;
            changed = true;
        }
    }
    if (changed) {
        saveRemoteEngines(engines);
    }
}

} // namespace trackknife::bench
