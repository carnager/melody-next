// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/startup.hpp"

#include "trackknife/persistence/workspace_backup.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include <filesystem>

namespace trackknife::bench {

void adoptInterimIdentity() {
    const auto new_settings = QSettings{}.fileName();
    const auto old_settings =
        QFileInfo{new_settings}.dir().filePath(QStringLiteral("trackbench.conf"));
    if (!QFile::exists(new_settings) && QFile::exists(old_settings)) {
        QFile::rename(old_settings, new_settings);
    }
    const auto new_data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const auto old_data = QFileInfo{new_data}.dir().filePath(QStringLiteral("trackbench"));
    if (!QDir{new_data}.exists() && QDir{old_data}.exists()) {
        QDir{}.rename(old_data, new_data);
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
        const auto data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir{}.mkpath(data);
        const auto live = std::filesystem::path{
            QFile::encodeName(data + QStringLiteral("/lists.sqlite")).toStdString()};
        const auto rollback_name =
            QStringLiteral("/lists-before-restore-%1.sqlite")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss")));
        const auto rollback =
            std::filesystem::path{QFile::encodeName(data + rollback_name).toStdString()};
        const auto backup = std::filesystem::path{QFile::encodeName(pending).toStdString()};
        auto restored =
            persistence::restore_workspace_database_backup(backup, live, rollback);
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
