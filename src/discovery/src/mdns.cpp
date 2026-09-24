// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/discovery/mdns.hpp"

#include "trackknife/discovery/dns.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace trackknife::discovery {
namespace {

using clock = std::chrono::steady_clock;

constexpr std::uint16_t mdns_port = 5353U;
constexpr const char* mdns_group = "224.0.0.251";
constexpr std::uint32_t record_ttl = 120U;

[[nodiscard]] core::Error socket_error(const std::string& what) {
    return core::Error{.code = core::ErrorCode::io,
                       .message = "multicast DNS: " + what + ": " + std::strerror(errno),
                       .context = {}};
}

// This machine's IPv4 addresses that can carry multicast, in network order.
[[nodiscard]] std::vector<std::uint32_t> interface_addresses() {
    std::vector<std::uint32_t> addresses;
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) {
        return addresses;
    }
    for (auto* entry = list; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET ||
            (entry->ifa_flags & IFF_UP) == 0U || (entry->ifa_flags & IFF_LOOPBACK) != 0U ||
            (entry->ifa_flags & IFF_MULTICAST) == 0U) {
            continue;
        }
        const auto address = reinterpret_cast<sockaddr_in*>(entry->ifa_addr)->sin_addr.s_addr;
        if (std::ranges::find(addresses, address) == addresses.end()) {
            addresses.push_back(address);
        }
    }
    ::freeifaddrs(list);
    return addresses;
}

void join_group(const int socket) {
    ip_mreq membership{};
    ::inet_pton(AF_INET, mdns_group, &membership.imr_multiaddr);
    for (const auto address : interface_addresses()) {
        membership.imr_interface.s_addr = address;
        // Already a member (EADDRINUSE) is fine: this runs again as
        // interfaces come and go.
        static_cast<void>(
            ::setsockopt(socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)));
    }
}

// A socket on the mDNS port, shared with any other responder on this
// machine (avahi-daemon among them), in the group on every interface.
[[nodiscard]] core::Result<int> open_socket() {
    const int socket = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (socket < 0) {
        return std::unexpected(socket_error("socket"));
    }
    const int yes = 1;
    static_cast<void>(::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)));
    static_cast<void>(::setsockopt(socket, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes)));
    sockaddr_in bound{};
    bound.sin_family = AF_INET;
    bound.sin_port = htons(mdns_port);
    bound.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&bound), sizeof(bound)) != 0) {
        auto error = socket_error("bind to port 5353");
        ::close(socket);
        return std::unexpected(std::move(error));
    }
    // Heard by this machine too: an engine and Trackknife side by side find
    // each other.
    const unsigned char loop = 1U;
    const unsigned char ttl = 255U;
    static_cast<void>(::setsockopt(socket, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)));
    static_cast<void>(::setsockopt(socket, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)));
    join_group(socket);
    return socket;
}

// To the group, out of the interface with this address.
void send_via(const int socket, const Message& message, const std::uint32_t address) {
    const auto bytes = encode(message);
    sockaddr_in group{};
    group.sin_family = AF_INET;
    group.sin_port = htons(mdns_port);
    ::inet_pton(AF_INET, mdns_group, &group.sin_addr);
    in_addr out{};
    out.s_addr = address;
    static_cast<void>(::setsockopt(socket, IPPROTO_IP, IP_MULTICAST_IF, &out, sizeof(out)));
    static_cast<void>(::sendto(socket, bytes.data(), bytes.size(), MSG_NOSIGNAL,
                               reinterpret_cast<const sockaddr*>(&group), sizeof(group)));
}

// To the group, out of every interface: a machine on two networks is found
// on both.
void send_to_group(const int socket, const Message& message) {
    for (const auto address : interface_addresses()) {
        send_via(socket, message, address);
    }
}

[[nodiscard]] std::string host_label() {
    std::array<char, 256> host{};
    if (::gethostname(host.data(), host.size()) != 0 || host[0] == '\0') {
        return "melodyd";
    }
    std::string name{host.data()};
    // Only the first label: "desk.example.org" is "desk.local" here.
    if (const auto dot = name.find('.'); dot != std::string::npos) {
        name.resize(dot);
    }
    return name;
}

// A DNS label cannot hold a dot, and is at most 63 bytes.
[[nodiscard]] std::string instance_label(std::string name) {
    std::ranges::replace(name, '.', ' ');
    if (name.size() > 63U) {
        name.resize(63U);
    }
    return name.empty() ? std::string{"melodyd"} : name;
}

[[nodiscard]] bool ends_with_name(const std::string& name, const std::string& suffix) {
    return name.size() > suffix.size() + 1U && name[name.size() - suffix.size() - 1U] == '.' &&
           same_name(name.substr(name.size() - suffix.size()), suffix);
}

// Waits for a packet, up to `timeout`; its sender goes to `from`.
[[nodiscard]] std::size_t receive(const int socket, std::array<std::uint8_t, 9000>& buffer,
                                  std::uint32_t& from, const std::chrono::milliseconds timeout) {
    pollfd ready{.fd = socket, .events = POLLIN, .revents = 0};
    if (::poll(&ready, 1, static_cast<int>(timeout.count())) <= 0) {
        return 0U;
    }
    sockaddr_in sender{};
    socklen_t length = sizeof(sender);
    const auto received = ::recvfrom(socket, buffer.data(), buffer.size(), 0,
                                     reinterpret_cast<sockaddr*>(&sender), &length);
    if (received <= 0) {
        return 0U;
    }
    from = sender.sin_addr.s_addr;
    return static_cast<std::size_t>(received);
}

// The engines' service, unless the environment names another: tests do, so
// their engines are neither found by nor find the real ones on the network.
[[nodiscard]] std::string resolved_service(std::string service) {
    if (service == engine_service) {
        if (const char* other = std::getenv("TRACKKNIFE_DISCOVERY_SERVICE");
            other != nullptr && *other != '\0') {
            return other;
        }
    }
    return service;
}

} // namespace

// --- Announcer ---------------------------------------------------------------

core::Result<std::unique_ptr<Announcer>> Announcer::start(Advertisement advertisement,
                                                          std::string service) {
    auto socket = open_socket();
    if (!socket) {
        return std::unexpected(std::move(socket.error()));
    }
    return std::unique_ptr<Announcer>{
        new Announcer{*socket, std::move(advertisement), resolved_service(std::move(service))}};
}

Announcer::Announcer(const int socket, Advertisement advertisement, std::string service)
    : socket_(socket), advertisement_(std::move(advertisement)), service_(std::move(service)) {
    advertisement_.instance = instance_label(std::move(advertisement_.instance));
    instance_name_ = advertisement_.instance + "." + service_ + ".local";
    host_name_ = host_label() + ".local";
    worker_ = std::thread{[this] { run(); }};
}

Announcer::~Announcer() {
    running_.store(false);
    if (worker_.joinable()) {
        worker_.join();
    }
    // Gone now, rather than when the others' records run out.
    answer(true);
    ::close(socket_);
}

void Announcer::answer(const bool goodbye) {
    const auto ttl = goodbye ? 0U : record_ttl;
    Message message;
    message.response = true;
    message.answers.push_back(Record{.name = service_ + ".local",
                                     .type = RecordType::ptr,
                                     .ttl = ttl,
                                     .target = instance_name_,
                                     .port = 0,
                                     .strings = {},
                                     .address = 0});
    message.additionals.push_back(Record{.name = instance_name_,
                                         .type = RecordType::srv,
                                         .ttl = ttl,
                                         .target = host_name_,
                                         .port = advertisement_.port,
                                         .strings = {},
                                         .address = 0});
    Record text{.name = instance_name_,
                .type = RecordType::txt,
                .ttl = ttl,
                .target = {},
                .port = 0,
                .strings = {},
                .address = 0};
    for (const auto& [key, value] : advertisement_.txt) {
        text.strings.push_back(key + "=" + value);
    }
    message.additionals.push_back(std::move(text));
    // Each network hears the address this machine has on it, and only that
    // (RFC 6762 §15): gemenon told the LAN its Docker bridge's address as
    // well, and a phone that picked it could not reach the engine.
    for (const auto address : interface_addresses()) {
        auto on_this_network = message;
        on_this_network.additionals.push_back(Record{.name = host_name_,
                                                     .type = RecordType::a,
                                                     .ttl = ttl,
                                                     .target = {},
                                                     .port = 0,
                                                     .strings = {},
                                                     .address = ntohl(address)});
        send_via(socket_, on_this_network, address);
    }
}

void Announcer::run() {
    // Announced three times as it starts (RFC 6762 §8.3), then now and then,
    // so one lost packet does not hide it.
    const auto started = clock::now();
    int announced = 0;
    auto next_announcement = started;
    auto last_answer = started - std::chrono::seconds{10};
    std::array<std::uint8_t, 9000> buffer{};
    while (running_.load()) {
        const auto now = clock::now();
        if (now >= next_announcement) {
            join_group(socket_);
            answer(false);
            ++announced;
            next_announcement =
                now + (announced < 3 ? std::chrono::seconds{1} : std::chrono::seconds{60});
            last_answer = now;
        }
        std::uint32_t from = 0;
        const auto size = receive(socket_, buffer, from, std::chrono::milliseconds{250});
        if (size == 0U) {
            continue;
        }
        const auto message = decode(buffer.data(), size);
        if (!message || message->response) {
            continue;
        }
        const bool asked = std::ranges::any_of(message->questions, [this](const Question& q) {
            return same_name(q.name, service_ + ".local") || same_name(q.name, instance_name_) ||
                   same_name(q.name, host_name_);
        });
        // At most once a second, however many ask.
        if (asked && clock::now() - last_answer >= std::chrono::seconds{1}) {
            answer(false);
            last_answer = clock::now();
        }
    }
}

// --- Browser -----------------------------------------------------------------

core::Result<std::unique_ptr<Browser>>
Browser::start(std::function<void(const std::vector<Found>&)> changed, std::string service) {
    auto socket = open_socket();
    if (!socket) {
        return std::unexpected(std::move(socket.error()));
    }
    return std::unique_ptr<Browser>{
        new Browser{*socket, std::move(changed), resolved_service(std::move(service))}};
}

Browser::Browser(const int socket, std::function<void(const std::vector<Found>&)> changed,
                 std::string service)
    : socket_(socket), changed_(std::move(changed)), service_(std::move(service)) {
    worker_ = std::thread{[this] { run(); }};
}

Browser::~Browser() {
    running_.store(false);
    if (worker_.joinable()) {
        worker_.join();
    }
    ::close(socket_);
}

std::vector<Found> Browser::found() const {
    const std::lock_guard guard{mutex_};
    std::vector<Found> listed;
    listed.reserve(entries_.size());
    for (const auto& [name, entry] : entries_) {
        listed.push_back(entry.found);
    }
    return listed;
}

void Browser::ask() {
    Message question;
    question.questions.push_back(Question{.name = service_ + ".local", .type = RecordType::ptr});
    send_to_group(socket_, question);
}

void Browser::run() {
    // Asked quickly at first, as a window opening wants the list now, then
    // now and then for what starts later without being heard.
    const std::array<std::chrono::seconds, 3> early{std::chrono::seconds{0}, std::chrono::seconds{1},
                                                    std::chrono::seconds{3}};
    const auto started = clock::now();
    std::size_t asked = 0;
    auto next_question = started;
    std::array<std::uint8_t, 9000> buffer{};
    while (running_.load()) {
        const auto now = clock::now();
        if (now >= next_question) {
            join_group(socket_);
            ask();
            ++asked;
            next_question = asked < early.size() ? started + early[asked]
                                                 : now + std::chrono::seconds{30};
        }
        std::uint32_t from = 0;
        const auto size = receive(socket_, buffer, from, std::chrono::milliseconds{250});
        if (size > 0U) {
            take(buffer.data(), size, from);
        }
        expire();
    }
}

void Browser::take(const std::uint8_t* data, const std::size_t size, const std::uint32_t from) {
    const auto message = decode(data, size);
    if (!message || !message->response) {
        return;
    }
    std::vector<const Record*> records;
    for (const auto& record : message->answers) {
        records.push_back(&record);
    }
    for (const auto& record : message->additionals) {
        records.push_back(&record);
    }
    const auto service_name = service_ + ".local";
    bool changed = false;
    for (const auto* srv : records) {
        if (srv->type != RecordType::srv || !ends_with_name(srv->name, service_name)) {
            continue;
        }
        const auto instance = srv->name.substr(0, srv->name.size() - service_name.size() - 1U);
        const std::lock_guard guard{mutex_};
        if (srv->ttl == 0U) {
            changed = entries_.erase(srv->name) > 0U || changed;
            continue;
        }
        Found found;
        found.instance = instance;
        found.port = srv->port;
        std::array<char, INET_ADDRSTRLEN> text{};
        in_addr sender{};
        sender.s_addr = from;
        found.address = ::inet_ntop(AF_INET, &sender, text.data(), text.size());
        for (const auto* txt : records) {
            if (txt->type == RecordType::txt && same_name(txt->name, srv->name)) {
                for (const auto& pair : txt->strings) {
                    const auto equals = pair.find('=');
                    if (equals != std::string::npos) {
                        found.txt[pair.substr(0, equals)] = pair.substr(equals + 1U);
                    }
                }
            }
        }
        auto& entry = entries_[srv->name];
        if (!(entry.found == found)) {
            entry.found = std::move(found);
            changed = true;
        }
        entry.expires = clock::now() + std::chrono::seconds{std::max<std::uint32_t>(srv->ttl, 5U)};
    }
    if (changed) {
        notify();
    }
}

void Browser::expire() {
    bool changed = false;
    {
        const std::lock_guard guard{mutex_};
        const auto now = clock::now();
        changed = std::erase_if(entries_, [now](const auto& pair) {
                      return pair.second.expires <= now;
                  }) > 0U;
    }
    if (changed) {
        notify();
    }
}

void Browser::notify() {
    if (changed_) {
        changed_(found());
    }
}

} // namespace trackknife::discovery
