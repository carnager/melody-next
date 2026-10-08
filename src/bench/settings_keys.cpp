// SPDX-License-Identifier: GPL-3.0-only

#include "bench/settings_keys.hpp"

#include "trackknife/metadata/ratings.hpp"

#include <QFile>
#include <QSettings>

#include <algorithm>

namespace trackknife::bench {

QString SettingsKeys::remoteEnginePassword() {
    const QSettings settings;
    const auto own =
        settings.value(QLatin1String(library_engine_token_key), QString{}).toString().trimmed();
    return own.isEmpty() ? settings.value(QLatin1String(engine_password_key), QString{}).toString()
                         : own;
}

std::string SettingsKeys::ratingBackupTag() {
    const QSettings settings;
    if (!settings.value(QLatin1String(rating_backup_key), false).toBool()) {
        return {};
    }
    const auto tag = settings
                         .value(QLatin1String(rating_backup_tag_key),
                                QString::fromLatin1(metadata::default_rating_backup_tag))
                         .toString()
                         .trimmed()
                         .toStdString();
    return metadata::rating_backup_tag_problem(tag) ? std::string{} : tag;
}

metadata::ArtworkStoragePolicy SettingsKeys::artworkPolicy() {
    const QSettings settings;
    return {.embed = settings.value(QLatin1String(artwork_embed_key), true).toBool(),
            .write_folder_image =
                settings.value(QLatin1String(artwork_folder_image_key), false).toBool(),
            .folder_image_name =
                QFile::encodeName(settings
                                      .value(QLatin1String(artwork_folder_image_name_key),
                                             QStringLiteral("cover.jpg"))
                                      .toString())
                    .toStdString(),
            .fetch_source = settings
                                .value(QLatin1String(artwork_fetch_source_key),
                                       QStringLiteral("coverartarchive"))
                                .toString()
                                .toStdString(),
            .max_embedded_edge = static_cast<std::uint32_t>(std::max(
                0, settings.value(QLatin1String(artwork_max_embedded_edge_key), 0).toInt())),
            .max_folder_edge = static_cast<std::uint32_t>(std::max(
                0, settings.value(QLatin1String(artwork_max_folder_edge_key), 0).toInt()))};
}

} // namespace trackknife::bench
