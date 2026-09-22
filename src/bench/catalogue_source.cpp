// SPDX-License-Identifier: GPL-3.0-only

#include "bench/catalogue_source.hpp"

#include "bench/settings_dialog.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/engine/remote_catalogue.hpp"

#include <QObject>
#include <QSettings>

#include <utility>

namespace trackknife::bench {
namespace {

[[nodiscard]] QString pathText(const std::filesystem::path& path) {
    return QString::fromStdString(core::escape_raw_path(path.string()));
}

} // namespace

CatalogueSource::CatalogueSource(std::filesystem::path database) : database_(std::move(database)) {
    const auto configured =
        QSettings{}
            .value(QLatin1String(SettingsDialog::library_engine_socket_key), QString{})
            .toString();
    if (configured.isEmpty()) {
        return;
    }
    socket_ = configured.toStdString();
    auto client = protocol::Client::connect(socket_);
    if (client) {
        client_ = std::move(*client);
        return;
    }
    failure_ = QString::fromUtf8(client.error().message);
}

CatalogueSource::~CatalogueSource() = default;

std::unique_ptr<engine::Catalogue> CatalogueSource::open() const {
    if (client_) {
        return std::make_unique<engine::RemoteCatalogue>(*client_);
    }
    return std::make_unique<engine::LocalCatalogue>(database_);
}

QString CatalogueSource::describe() const {
    if (client_) {
        return QObject::tr("Library: engine at %1").arg(pathText(socket_));
    }
    if (!socket_.empty()) {
        return QObject::tr("Library: this process — engine at %1 is unreachable")
            .arg(pathText(socket_));
    }
    return QObject::tr("Library: this process");
}

} // namespace trackknife::bench
