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

// What a caller gets with no engine to ask. ADR-0226: there is no database
// of this process's to fall back on, so every question is answered with the
// reason rather than with an empty library that looks real.
class UnavailableCatalogue final : public engine::Catalogue {
  public:
    explicit UnavailableCatalogue(std::string reason) : reason_(std::move(reason)) {}

    [[nodiscard]] core::Result<std::vector<persistence::LibraryRoot>> roots() const override {
        return refuse<std::vector<persistence::LibraryRoot>>();
    }
    [[nodiscard]] core::Result<void> add_root(const std::string&) override {
        return refuse<void>();
    }
    [[nodiscard]] core::Result<void> remove_root(const std::string&) override {
        return refuse<void>();
    }
    [[nodiscard]] core::Result<persistence::LibraryPage>
    query(const persistence::LibraryQuery&, const core::CancellationToken&) const override {
        return refuse<persistence::LibraryPage>();
    }
    [[nodiscard]] core::Result<std::vector<std::string>>
    paths(const persistence::LibraryQuery&, const core::CancellationToken&) const override {
        return refuse<std::vector<std::string>>();
    }
    [[nodiscard]] core::Result<persistence::LibraryPage>
    filter(const query::CompiledTkq&, std::size_t, std::size_t,
           const core::CancellationToken&) const override {
        return refuse<persistence::LibraryPage>();
    }
    [[nodiscard]] core::Result<std::vector<unsigned>>
    ratings(const std::vector<std::string>&, const core::CancellationToken&) const override {
        return refuse<std::vector<unsigned>>();
    }
    [[nodiscard]] core::Result<void> set_rating(const std::string&, bool, unsigned) override {
        return refuse<void>();
    }
    [[nodiscard]] core::Result<std::vector<std::string>>
    filter_paths(const query::CompiledTkq&, const core::CancellationToken&) const override {
        return refuse<std::vector<std::string>>();
    }
    [[nodiscard]] core::Result<std::optional<std::string>>
    artwork_source(const std::string&, const core::CancellationToken&) const override {
        return refuse<std::optional<std::string>>();
    }
    [[nodiscard]] core::Result<std::vector<persistence::LibraryTrackSnapshot>>
    cached_tracks(const std::vector<std::string>&, const core::CancellationToken&) const override {
        return refuse<std::vector<persistence::LibraryTrackSnapshot>>();
    }
    [[nodiscard]] core::Result<std::vector<std::array<std::int64_t, 6>>>
    history_facts(const std::vector<persistence::LibraryHistorySource>&,
                  const core::CancellationToken&) const override {
        return refuse<std::vector<std::array<std::int64_t, 6>>>();
    }
    [[nodiscard]] core::Result<persistence::LibraryScanResult>
    scan(const core::CancellationToken&, persistence::LibraryScanProgress&) override {
        return refuse<persistence::LibraryScanResult>();
    }

  private:
    template <typename T> [[nodiscard]] core::Result<T> refuse() const {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::backend, .message = reason_, .context = {}});
    }
    std::string reason_;
};

} // namespace

CatalogueSource::CatalogueSource(std::filesystem::path database, const Role role)
    : database_(std::move(database)), role_(role) {
    const QSettings settings;
    if (role == Role::local) {
        // ADR-0227: this computer's engine is always there -- the one the
        // workspace starts, or one named for development and tests.
        const auto running =
            settings
                .value(QLatin1String(SettingsDialog::library_local_engine_socket_key), QString{})
                .toString();
        if (!running.isEmpty()) {
            endpoint_ = protocol::Endpoint::parse(running.toStdString(), {});
            if (!endpoint_) {
                failure_ = QObject::tr("not an engine address: %1").arg(running);
                return;
            }
        } else {
            local_engine_ = bench::localEngine();
            if (!local_engine_) {
                failure_ = QObject::tr("this computer's engine is not started here");
                return;
            }
            endpoint_ = protocol::Endpoint{
                .socket = local_engine_->socket, .host = {}, .port = 0, .token = {}};
        }
    } else {
        // The one engine elsewhere, if one is configured (ADR-0227).
        const auto configured =
            settings.value(QLatin1String(SettingsDialog::library_engine_socket_key), QString{})
                .toString();
        if (configured.isEmpty()) {
            failure_ = QObject::tr("no remote engine is configured");
            return;
        }
        const auto token =
            settings.value(QLatin1String(SettingsDialog::library_engine_token_key), QString{})
                .toString()
                .trimmed()
                .toStdString();
        endpoint_ = protocol::Endpoint::parse(configured.toStdString(), token);
        if (!endpoint_) {
            failure_ = QObject::tr("not an engine address: %1").arg(configured);
            return;
        }
    }
    const std::lock_guard guard{mutex_};
    static_cast<void>(connectLocked());
}

CatalogueSource::~CatalogueSource() = default;

std::shared_ptr<protocol::Client> CatalogueSource::connectLocked() const {
    if (client_ && client_->connected()) {
        return client_;
    }
    if (!endpoint_) {
        return nullptr;
    }
    auto client =
        local_engine_ ? connectLocalEngine(*local_engine_) : protocol::Client::connect(*endpoint_);
    if (!client) {
        client_.reset();
        failure_ = QString::fromUtf8(client.error().message);
        refused_ = client.error().code == core::ErrorCode::unauthorized;
        return nullptr;
    }
    // A connection that dropped is replaced, not repaired: an engine that
    // restarted is a new process, and catalogues already handed out keep the
    // old connection alive until they finish with it.
    client_ = std::shared_ptr<protocol::Client>{std::move(*client)};
    failure_.clear();
    refused_ = false;
    return client_;
}

std::unique_ptr<engine::Catalogue> CatalogueSource::open() const {
    const std::lock_guard guard{mutex_};
    if (auto client = connectLocked()) {
        return std::make_unique<engine::RemoteCatalogue>(std::move(client));
    }
    return std::make_unique<UnavailableCatalogue>(
        (failure_.isEmpty() ? QObject::tr("no engine is connected")
                            : QObject::tr("no engine is connected: %1").arg(failure_))
            .toStdString());
}

bool CatalogueSource::usingEngine() const {
    const std::lock_guard guard{mutex_};
    return client_ && client_->connected();
}

QString CatalogueSource::failure() const {
    const std::lock_guard guard{mutex_};
    return failure_;
}

bool CatalogueSource::reviveLocalEngine() const {
    return local_engine_ && connectLocalEngine(*local_engine_).has_value();
}

QString CatalogueSource::describe() const {
    const std::lock_guard guard{mutex_};
    const bool connected = client_ && client_->connected();
    if (role_ == Role::local) {
        return connected ? QObject::tr("Library: this computer")
                         : QObject::tr("Library unavailable: this computer's engine did not "
                                       "start (%1)")
                               .arg(failure_);
    }
    if (connected) {
        return QObject::tr("Library: engine at %1").arg(endpointText(*endpoint_));
    }
    if (endpoint_) {
        // A refused password is its own case: the engine is there, and saying
        // "unreachable" would send someone looking at the network.
        return refused_ ? QObject::tr("Library unavailable: the engine at %1 refused the password")
                              .arg(endpointText(*endpoint_))
                        : QObject::tr("Library unavailable: the engine at %1 is unreachable")
                              .arg(endpointText(*endpoint_));
    }
    return QObject::tr("Library unavailable: %1").arg(failure_);
}

QString CatalogueSource::name() const {
    if (role_ == Role::local || !endpoint_) {
        return QObject::tr("This computer");
    }
    // The host alone reads as a place; a socket path is named by its file.
    if (endpoint_->tcp()) {
        return QString::fromStdString(endpoint_->host);
    }
    return QString::fromStdString(endpoint_->socket.filename().string());
}

} // namespace trackknife::bench
