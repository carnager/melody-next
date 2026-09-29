// SPDX-License-Identifier: GPL-3.0-only

#include "bench/settings_keys.hpp"

#include <QSettings>

namespace trackknife::bench {

QString SettingsKeys::remoteEnginePassword() {
    const QSettings settings;
    const auto own =
        settings.value(QLatin1String(library_engine_token_key), QString{}).toString().trimmed();
    return own.isEmpty() ? settings.value(QLatin1String(engine_password_key), QString{}).toString()
                         : own;
}

} // namespace trackknife::bench
