// SPDX-License-Identifier: GPL-3.0-only

#include "bench/catalogue_source.hpp"

#include "bench/engine_launcher.hpp"
#include "bench/settings_dialog.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/engine/remote_catalogue.hpp"

#include <QObject>
#include <QSettings>

#include <utility>

namespace trackknife::bench {
namespace {

[[nodiscard]] QString endpointText(const protocol::Endpoint& endpoint) {
    return QString::fromStdString(core::escape_raw_path(endpoint.describe()));
}

} // namespace

CatalogueSource::CatalogueSource(std::filesystem::path database) : database_(std::move(database)) {
    const auto configured =
        QSettings{}
            .value(QLatin1String(SettingsDialog::library_engine_socket_key), QString{})
            .toString();
    if (configured.isEmpty()) {
        // ADR-0226: no engine named means this machine's, started if need be.
        local_engine_ = bench::localEngine();
        if (!local_engine_) {
            return;
        }
        endpoint_ =
            protocol::Endpoint{.socket = local_engine_->socket, .host = {}, .port = 0, .token = {}};
        auto client = connectLocalEngine(*local_engine_);
        if (client) {
            client_ = std::move(*client);
            return;
        }
        failure_ = QString::fromUtf8(client.error().message);
        return;
    }
    const auto token =
        QSettings{}
            .value(QLatin1String(SettingsDialog::library_engine_token_key), QString{})
            .toString()
            .trimmed()
            .toStdString();
    endpoint_ = protocol::Endpoint::parse(configured.toStdString(), token);
    if (!endpoint_) {
        failure_ = QObject::tr("not an engine address: %1").arg(configured);
        return;
    }
    auto client = protocol::Client::connect(*endpoint_);
    if (client) {
        client_ = std::move(*client);
        return;
    }
    failure_ = QString::fromUtf8(client.error().message);
    refused_ = client.error().code == core::ErrorCode::unauthorized;
}

CatalogueSource::~CatalogueSource() = default;

std::unique_ptr<engine::Catalogue> CatalogueSource::open() const {
    if (client_) {
        return std::make_unique<engine::RemoteCatalogue>(*client_);
    }
    return std::make_unique<engine::LocalCatalogue>(database_);
}

bool CatalogueSource::reviveLocalEngine() const {
    return local_engine_ && connectLocalEngine(*local_engine_).has_value();
}

QString CatalogueSource::describe() const {
    if (client_ && local_engine_) {
        return QObject::tr("Library: engine on this computer");
    }
    if (local_engine_) {
        return QObject::tr("Library: this process — the local engine did not start: %1")
            .arg(failure_);
    }
    if (client_) {
        return QObject::tr("Library: engine at %1").arg(endpointText(*endpoint_));
    }
    if (endpoint_) {
        // A refused token is its own case: the engine is there, and saying
        // "unreachable" would send someone looking at the network.
        return refused_ ? QObject::tr("Library: this process — engine at %1 refused the token")
                              .arg(endpointText(*endpoint_))
                        : QObject::tr("Library: this process — engine at %1 is unreachable")
                              .arg(endpointText(*endpoint_));
    }
    return QObject::tr("Library: this process");
}

} // namespace trackknife::bench
