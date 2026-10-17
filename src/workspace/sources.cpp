// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/sources.hpp"

#include "bench/remote_engines.hpp"
#include "bench/settings_keys.hpp"
#include "trackknife/core/local_sources.hpp"

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QVariantList>

#include <algorithm>
#include <filesystem>

namespace trackknife::bench {

std::vector<std::string> loadFolderBookmarks() {
    QSettings settings;
    auto stored = settings.value(QStringLiteral("library/bookmarks")).toList();
    if (!settings.contains(QStringLiteral("library/bookmarks"))) {
        // First run of the bookmark panel: the old manually added library
        // roots become bookmarks, headed by the home directory.
        stored.push_back(QFile::encodeName(QDir::homePath()));
        for (const auto& root : settings.value(QStringLiteral("library/roots")).toList()) {
            if (!root.toByteArray().isEmpty()) {
                stored.push_back(root);
            }
        }
        settings.setValue(QStringLiteral("library/bookmarks"), stored);
    }
    std::vector<std::string> paths;
    for (const auto& entry : stored) {
        const auto bytes = entry.toByteArray();
        if (!bytes.isEmpty()) {
            paths.emplace_back(bytes.constData(), static_cast<std::size_t>(bytes.size()));
        }
    }
    return paths;
}

void storeFolderBookmarks(const std::vector<std::string>& raw_paths) {
    QVariantList stored;
    for (const auto& path : raw_paths) {
        stored.push_back(QByteArray{path.data(), static_cast<qsizetype>(path.size())});
    }
    QSettings{}.setValue(QStringLiteral("library/bookmarks"), stored);
}

QString folderBookmarkLabel(const std::string& raw_path) {
    const auto name = std::filesystem::path{raw_path}.filename().native();
    return QString::fromUtf8(core::display_raw_path(name.empty() ? raw_path : name));
}

QString folderBookmarkTooltip(const std::string& raw_path) {
    return QString::fromUtf8(core::display_raw_path(raw_path));
}

QString libraryEngine() {
    const QSettings settings;
    const auto chosen = settings.value(QLatin1String(SettingsKeys::library_engine_key)).toString();
    const auto engines = loadRemoteEngines();
    const auto known = [&engines](const QString& address) {
        return std::ranges::any_of(
            engines, [&address](const auto& engine) { return engine.address == address; });
    };
    if (chosen == QLatin1String(SettingsKeys::library_engine_local) ||
        (!chosen.isEmpty() && known(chosen))) {
        return chosen;
    }
    // Not chosen yet, or an engine since removed: as Show this computer's
    // library had it.
    if (!engines.empty() &&
        !settings.value(QLatin1String(SettingsKeys::library_show_local_key), true).toBool()) {
        return engines.front().address;
    }
    return QLatin1String(SettingsKeys::library_engine_local);
}

bool localLibraryShown() {
    return libraryEngine() == QLatin1String(SettingsKeys::library_engine_local);
}

QString preferredSource() {
    auto wanted = QSettings{}.value(QStringLiteral("local-library/view")).toString();
    if (wanted == QStringLiteral("0")) {
        return QStringLiteral("folders");
    }
    if (wanted != QStringLiteral("folders") && wanted != QStringLiteral("remote")) {
        return QStringLiteral("library");
    }
    return wanted;
}

void rememberSource(const QString& kind) {
    if (!kind.isEmpty()) {
        QSettings{}.setValue(QStringLiteral("local-library/view"), kind);
    }
}

} // namespace trackknife::bench
