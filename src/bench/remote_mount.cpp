// SPDX-License-Identifier: GPL-3.0-only

#include "bench/remote_mount.hpp"

#include "bench/settings_dialog.hpp"

#include <QFile>
#include <QSettings>

#include <filesystem>
#include <system_error>

namespace trackknife::bench {
namespace {

[[nodiscard]] std::string trimmed_folder(std::string folder) {
    while (folder.size() > 1U && folder.back() == '/') {
        folder.pop_back();
    }
    return folder;
}

[[nodiscard]] std::string setting(const char* key) {
    const auto text = QSettings{}.value(QLatin1String(key), QString{}).toString().trimmed();
    return trimmed_folder(QFile::encodeName(text).toStdString());
}

// `path` with `from` at its start replaced by `to`; `path` itself when
// either is empty (the same paths on both machines).
[[nodiscard]] std::optional<std::string> rebase(const std::string& path, const std::string& from,
                                                const std::string& to) {
    if (from.empty() || to.empty()) {
        return path;
    }
    if (!path_within(path, from)) {
        return std::nullopt;
    }
    return to + path.substr(from.size());
}

} // namespace

bool path_within(const std::string_view path, const std::string_view folder) {
    if (folder.empty() || !path.starts_with(folder)) {
        return false;
    }
    return path.size() == folder.size() || folder.back() == '/' || path[folder.size()] == '/';
}

RemoteMount RemoteMount::configured() {
    return RemoteMount{.remote_folder = setting(SettingsDialog::library_remote_folder_key),
                       .local_folder = setting(SettingsDialog::library_remote_mount_key)};
}

std::optional<std::string> RemoteMount::to_local(const std::string& remote_path) const {
    auto local = rebase(remote_path, remote_folder, local_folder);
    std::error_code error;
    if (!local || !std::filesystem::is_regular_file(*local, error)) {
        return std::nullopt;
    }
    return local;
}

std::optional<std::string>
RemoteMount::to_remote(const std::string& local_path,
                       const std::vector<std::string>& remote_roots) const {
    auto remote = rebase(local_path, local_folder, remote_folder);
    if (!remote) {
        return std::nullopt;
    }
    for (const auto& root : remote_roots) {
        if (path_within(*remote, root)) {
            return remote;
        }
    }
    return std::nullopt;
}

} // namespace trackknife::bench
