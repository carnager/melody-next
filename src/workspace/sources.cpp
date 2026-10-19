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
    // Folders, from before they went, is the library.
    if (wanted != QStringLiteral("remote") && wanted != QStringLiteral("playlists")) {
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
