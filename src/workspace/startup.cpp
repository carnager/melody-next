// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/startup.hpp"

#include "bench/settings_keys.hpp"
#include "workspace/tab_store.hpp"

#include "trackknife/core/engine_state.hpp"
#include "trackknife/persistence/workspace_backup.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include <filesystem>

namespace trackknife::bench {

namespace {

// The same place under Trackknife's name (ADR-0287): each of the path's last
// two components that is the application's name now, the old one.
[[nodiscard]] QString underOldName(const QString& path) {
    const auto name = QCoreApplication::applicationName();
    auto parts = QDir::cleanPath(path).split(QLatin1Char('/'));
    for (auto index = parts.size() - 1; index >= 0 && index >= parts.size() - 2; --index) {
        if (parts[index] == name) {
            parts[index] = QStringLiteral("trackknife");
        } else if (parts[index].startsWith(name + QLatin1Char('.'))) {
            // The settings file, melody.conf or however its format names it.
            parts[index] = QStringLiteral("trackknife") + parts[index].sliced(name.size());
        }
    }
    return parts.join(QLatin1Char('/'));
}

} // namespace

void adoptTrackknifeNames() {
    // ADR-0287: settings, the window's data and its cache move from
    // Trackknife's names, once. The data folder was the engine's too: only
    // the window's own entries move here, the engine moves its own.
    const auto new_settings = QSettings{}.fileName();
    if (const auto old_settings = underOldName(new_settings); old_settings != new_settings &&
                                                              !QFile::exists(new_settings) &&
                                                              QFile::exists(old_settings)) {
        QDir{}.mkpath(QFileInfo{new_settings}.absolutePath());
        if (QFile::rename(old_settings, new_settings)) {
            // The rating backup copy kept under the name it had by default
            // then, so no file is written again for the new default.
            QSettings settings;
            if (settings.value(QLatin1String(SettingsKeys::rating_backup_key), false).toBool() &&
                !settings.contains(QLatin1String(SettingsKeys::rating_backup_tag_key))) {
                settings.setValue(QLatin1String(SettingsKeys::rating_backup_tag_key),
                                  QStringLiteral("TRACKKNIFE_RATING"));
            }
        }
    }
    const auto new_data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (const QDir old_data{underOldName(new_data)};
        old_data.path() != new_data && old_data.exists()) {
        QDir{}.mkpath(new_data);
        for (const auto& name : old_data.entryList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
            const bool windows =
                name == QStringLiteral("tabs") || name == QStringLiteral("lastfm-v1.json") ||
                name == QStringLiteral("melodyd.log") || name.startsWith(QStringLiteral("before-"));
            if (windows && !QFileInfo::exists(new_data + QLatin1Char('/') + name)) {
                QDir{}.rename(old_data.filePath(name), new_data + QLatin1Char('/') + name);
            }
        }
    }
    const auto new_cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (const auto old_cache = underOldName(new_cache);
        old_cache != new_cache && !QDir{new_cache}.exists() && QDir{old_cache}.exists()) {
        QDir{}.mkpath(QFileInfo{new_cache}.absolutePath());
        QDir{}.rename(old_cache, new_cache);
    }
}

void isolateTestEngineState() {
    if (QStandardPaths::isTestModeEnabled() && qEnvironmentVariableIsEmpty("MELODY_STATE_DIR")) {
        qputenv("MELODY_STATE_DIR", QFile::encodeName(QStandardPaths::writableLocation(
                                        QStandardPaths::AppDataLocation)));
    }
}

QString applyPendingWorkspaceRestore() {
    QString restore_notice;
    QSettings settings;
    const auto pending =
        settings.value(QStringLiteral("recovery/pending-workspace-restore")).toString();
    const auto pending_settings =
        settings.value(QStringLiteral("recovery/pending-settings-restore")).toString();
    if (!pending.isEmpty()) {
        // ADR-0287: the database is the engine's, in its folder.
        const auto data =
            QFile::decodeName(QByteArray::fromStdString(core::engine_state_directory().native()));
        QDir{}.mkpath(data);
        const auto live = std::filesystem::path{
            QFile::encodeName(data + QStringLiteral("/lists.sqlite")).toStdString()};
        const auto rollback_name =
            QStringLiteral("/lists-before-restore-%1.sqlite")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss")));
        const auto rollback =
            std::filesystem::path{QFile::encodeName(data + rollback_name).toStdString()};
        const auto backup = std::filesystem::path{QFile::encodeName(pending).toStdString()};
        auto restored = persistence::restore_workspace_database_backup(backup, live, rollback);
        if (restored) {
            QString settings_error;
            if (!pending_settings.isEmpty()) {
                QSettings imported{pending_settings, QSettings::IniFormat};
                if (imported.value(QStringLiteral("backup/format")).toInt() != 1 ||
                    imported.status() != QSettings::NoError) {
                    settings_error = QStringLiteral("; settings backup was invalid");
                } else {
                    settings.clear();
                    for (const auto& key : imported.allKeys()) {
                        if (key.startsWith(QStringLiteral("values/"))) {
                            settings.setValue(key.sliced(7), imported.value(key));
                        }
                    }
                }
            }
            // ADR-0286: and the tabs kept beside it, the ones here set aside.
            if (const auto tabs = pending + QStringLiteral(".tabs");
                QDir{tabs}.exists() && !TabStore::restoreFrom(tabs)) {
                settings_error += QStringLiteral("; the tabs could not be restored");
            }
            settings.remove(QStringLiteral("recovery/pending-workspace-restore"));
            settings.remove(QStringLiteral("recovery/pending-settings-restore"));
            settings.sync();
            restore_notice =
                QStringLiteral("Workspace restored. Previous database: %1%2")
                    .arg(QFile::decodeName(QByteArray::fromStdString(rollback.native())),
                         settings_error);
        } else {
            restore_notice = QStringLiteral("Workspace restore failed: %1")
                                 .arg(QString::fromStdString(restored.error().message));
        }
    }
    return restore_notice;
}

} // namespace trackknife::bench
