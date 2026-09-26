// SPDX-License-Identifier: GPL-3.0-only

#include "bench/catalogue_source.hpp"

#include "trackknife/protocol/message.hpp"

#include "bench/engine_launcher.hpp"
#include "bench/settings_dialog.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/engine/remote_catalogue.hpp"

#include <QObject>
#include <QSettings>

#include <functional>
#include <utility>

namespace trackknife::bench {
namespace {

[[nodiscard]] QString endpointText(const protocol::Endpoint& endpoint) {
    return QString::fromStdString(core::display_raw_path(endpoint.describe()));
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
    [[nodiscard]] core::Result<std::vector<unsigned char>>
    artwork(const std::string&, const core::CancellationToken&) const override {
        return refuse<std::vector<unsigned char>>();
    }
    [[nodiscard]] core::Result<std::size_t> refresh(const std::vector<std::string>&,
                                                    const core::CancellationToken&) override {
        return refuse<std::size_t>();
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

// A catalogue that connects on its first question rather than when it is
// made, so the window's thread can hand one to a worker without waiting on
// an engine that may be switched off.
class DeferredCatalogue final : public engine::Catalogue {
  public:
    using Open = std::function<std::unique_ptr<engine::Catalogue>()>;
    explicit DeferredCatalogue(Open open) : open_(std::move(open)) {}

    [[nodiscard]] core::Result<std::vector<persistence::LibraryRoot>> roots() const override {
        return get().roots();
    }
    [[nodiscard]] core::Result<void> add_root(const std::string& raw_path) override {
        return get().add_root(raw_path);
    }
    [[nodiscard]] core::Result<void> remove_root(const std::string& raw_path) override {
        return get().remove_root(raw_path);
    }
    [[nodiscard]] core::Result<persistence::LibraryPage>
    query(const persistence::LibraryQuery& request,
          const core::CancellationToken& cancellation) const override {
        return get().query(request, cancellation);
    }
    [[nodiscard]] core::Result<std::vector<std::string>>
    paths(const persistence::LibraryQuery& request,
          const core::CancellationToken& cancellation) const override {
        return get().paths(request, cancellation);
    }
    [[nodiscard]] core::Result<persistence::LibraryPage>
    filter(const query::CompiledTkq& compiled, const std::size_t offset, const std::size_t limit,
           const core::CancellationToken& cancellation) const override {
        return get().filter(compiled, offset, limit, cancellation);
    }
    [[nodiscard]] core::Result<std::string> revision() const override { return get().revision(); }
    [[nodiscard]] core::Result<std::vector<unsigned>>
    ratings(const std::vector<std::string>& hashes,
            const core::CancellationToken& cancellation) const override {
        return get().ratings(hashes, cancellation);
    }
    [[nodiscard]] core::Result<void> set_rating(const std::string& hash, const bool album,
                                                const unsigned rating) override {
        return get().set_rating(hash, album, rating);
    }
    [[nodiscard]] core::Result<std::vector<std::string>>
    filter_paths(const query::CompiledTkq& compiled,
                 const core::CancellationToken& cancellation) const override {
        return get().filter_paths(compiled, cancellation);
    }
    [[nodiscard]] core::Result<std::optional<std::string>>
    artwork_source(const std::string& album_key,
                   const core::CancellationToken& cancellation) const override {
        return get().artwork_source(album_key, cancellation);
    }
    [[nodiscard]] core::Result<std::size_t>
    refresh(const std::vector<std::string>& raw_paths,
            const core::CancellationToken& cancellation) override {
        return get().refresh(raw_paths, cancellation);
    }
    [[nodiscard]] core::Result<std::vector<unsigned char>>
    artwork(const std::string& raw_path,
            const core::CancellationToken& cancellation) const override {
        return get().artwork(raw_path, cancellation);
    }
    [[nodiscard]] core::Result<std::vector<persistence::LibraryTrackSnapshot>>
    cached_tracks(const std::vector<std::string>& raw_paths,
                  const core::CancellationToken& cancellation) const override {
        return get().cached_tracks(raw_paths, cancellation);
    }
    [[nodiscard]] core::Result<std::vector<std::array<std::int64_t, 6>>>
    history_facts(const std::vector<persistence::LibraryHistorySource>& sources,
                  const core::CancellationToken& cancellation) const override {
        return get().history_facts(sources, cancellation);
    }
    [[nodiscard]] core::Result<persistence::LibraryScanResult>
    scan(const core::CancellationToken& cancellation,
         persistence::LibraryScanProgress& progress) override {
        return get().scan(cancellation, progress);
    }

  private:
    [[nodiscard]] engine::Catalogue& get() const {
        const std::lock_guard guard{mutex_};
        if (!opened_) {
            opened_ = open_();
        }
        return *opened_;
    }

    Open open_;
    mutable std::mutex mutex_;
    mutable std::unique_ptr<engine::Catalogue> opened_;
};

} // namespace

struct CatalogueSource::Link {
    std::optional<protocol::Endpoint> endpoint;
    std::optional<LocalEngine> local_engine;
    // One connect at a time. Held across the connect itself, which can take
    // seconds; `mutex` never is, so the window asking what the state is does
    // not wait behind a worker trying to reach an engine that is off.
    std::mutex connect_mutex;
    // The fields below. Replaced on reconnect, from whichever worker notices
    // first.
    std::mutex mutex;
    std::shared_ptr<protocol::Client> client;
    QString failure;
    // The name the engine gave for itself once connected (engine.info).
    QString announced;
    // Whether the engine answered and said no, as opposed to not answering.
    // Decided from the error code: a client must never parse the message.
    bool refused{false};

    // With connect_mutex held.
    [[nodiscard]] std::shared_ptr<protocol::Client> connectLocked();
    // Blocks while it connects.
    [[nodiscard]] std::unique_ptr<engine::Catalogue> open();
};

CatalogueSource::CatalogueSource(std::filesystem::path database, const Role role)
    : database_(std::move(database)), role_(role), link_(std::make_shared<Link>()) {
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
                link_->failure = QObject::tr("not an engine address: %1").arg(running);
                return;
            }
        } else {
            local_engine_ = bench::localEngine();
            if (!local_engine_) {
                link_->failure = QObject::tr("this computer's engine is not started here");
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
            link_->failure = QObject::tr("no remote engine is configured");
            return;
        }
        const auto token =
            settings.value(QLatin1String(SettingsDialog::library_engine_token_key), QString{})
                .toString()
                .trimmed()
                .toStdString();
        endpoint_ = protocol::Endpoint::parse(configured.toStdString(), token);
        if (!endpoint_) {
            link_->failure = QObject::tr("not an engine address: %1").arg(configured);
            return;
        }
    }
    // An engine over TCP is not connected to here, on the window's thread at
    // startup: one that is switched off holds a connect for seconds. The
    // first open() connects -- from a worker, as the library panel's are.
    link_->endpoint = endpoint_;
    link_->local_engine = local_engine_;
    if (endpoint_->tcp()) {
        return;
    }
    const std::lock_guard guard{link_->connect_mutex};
    static_cast<void>(link_->connectLocked());
}

CatalogueSource::~CatalogueSource() = default;

std::shared_ptr<protocol::Client> CatalogueSource::Link::connectLocked() {
    {
        const std::lock_guard guard{mutex};
        if (client && client->connected()) {
            return client;
        }
    }
    if (!endpoint) {
        return nullptr;
    }
    auto connected =
        local_engine ? connectLocalEngine(*local_engine) : protocol::Client::connect(*endpoint);
    if (!connected) {
        const std::lock_guard guard{mutex};
        client.reset();
        failure = QString::fromUtf8(connected.error().message);
        refused = connected.error().code == core::ErrorCode::unauthorized;
        return nullptr;
    }
    // A connection that dropped is replaced, not repaired: an engine that
    // restarted is a new process, and catalogues already handed out keep the
    // old connection alive until they finish with it.
    std::shared_ptr<protocol::Client> made{std::move(*connected)};
    // What the engine calls itself, shown in place of its address. An
    // engine too old to say keeps being shown by address.
    QString name;
    if (auto info = made->call("engine.info", protocol::Json::object(), std::chrono::seconds{2});
        info && info->contains("name") && info->at("name").is_string()) {
        name = QString::fromStdString(protocol::displayable_text(info->at("name").get<std::string>()));
    }
    const std::lock_guard guard{mutex};
    client = made;
    failure.clear();
    refused = false;
    if (!name.isEmpty()) {
        announced = name;
    }
    return made;
}

std::unique_ptr<engine::Catalogue> CatalogueSource::Link::open() {
    const std::lock_guard connecting{connect_mutex};
    if (auto connected = connectLocked()) {
        return std::make_unique<engine::RemoteCatalogue>(std::move(connected));
    }
    const std::lock_guard guard{mutex};
    return std::make_unique<UnavailableCatalogue>(
        (failure.isEmpty() ? QObject::tr("no engine is connected")
                           : QObject::tr("no engine is connected: %1").arg(failure))
            .toStdString());
}

std::unique_ptr<engine::Catalogue> CatalogueSource::open() const { return link_->open(); }

std::unique_ptr<engine::Catalogue> CatalogueSource::openDeferred() const {
    return std::make_unique<DeferredCatalogue>([link = link_] { return link->open(); });
}

bool CatalogueSource::usingEngine() const {
    const std::lock_guard guard{link_->mutex};
    return link_->client && link_->client->connected();
}

QString CatalogueSource::failure() const {
    const std::lock_guard guard{link_->mutex};
    return link_->failure;
}

bool CatalogueSource::reviveLocalEngine() const {
    return local_engine_ && connectLocalEngine(*local_engine_).has_value();
}

bool CatalogueSource::restartLocalEngine() const {
    return local_engine_ && bench::restartLocalEngine(*local_engine_).has_value();
}

bool CatalogueSource::stopLocalEngine() const {
    return local_engine_ && bench::stopLocalEngine(*local_engine_).has_value();
}

bool CatalogueSource::localEngineOutdated() const {
    return local_engine_ && bench::localEngineOutdated(*local_engine_);
}

bool CatalogueSource::reachable() const {
    const std::lock_guard guard{link_->mutex};
    return link_->client && link_->client->connected();
}

QString CatalogueSource::describe() const {
    const std::lock_guard guard{link_->mutex};
    const auto& failure = link_->failure;
    const auto& announced = link_->announced;
    const bool connected = link_->client && link_->client->connected();
    if (role_ == Role::local) {
        return connected ? QObject::tr("Library: this computer")
                         : QObject::tr("Library unavailable: this computer's engine did not "
                                       "start (%1)")
                               .arg(failure);
    }
    if (connected) {
        // By the name it gave, as its tab is; by address when it gave none.
        return announced.isEmpty()
                   ? QObject::tr("Library: engine at %1").arg(endpointText(*endpoint_))
                   : QObject::tr("Library: %1").arg(announced);
    }
    if (endpoint_) {
        // A refused password is its own case: the engine is there, and saying
        // "unreachable" would send someone looking at the network.
        return link_->refused ? QObject::tr("Library unavailable: the engine at %1 refused the password")
                              .arg(endpointText(*endpoint_))
                        : QObject::tr("Library unavailable: the engine at %1 is unreachable")
                              .arg(endpointText(*endpoint_));
    }
    return QObject::tr("Library unavailable: %1").arg(failure);
}

QString CatalogueSource::name() const {
    if (role_ == Role::local || !endpoint_) {
        return QObject::tr("This computer");
    }
    {
        const std::lock_guard guard{link_->mutex};
        if (!link_->announced.isEmpty()) {
            return link_->announced;
        }
    }
    return addressName();
}

QString CatalogueSource::addressName() const {
    if (!endpoint_) {
        return {};
    }
    // The host alone reads as a place; a socket path is named by its file.
    if (endpoint_->tcp()) {
        return QString::fromStdString(endpoint_->host);
    }
    return QString::fromStdString(endpoint_->socket.filename().string());
}

} // namespace trackknife::bench
