// SPDX-License-Identifier: GPL-3.0-only
#include "trackknife/discovery/upnp.hpp"

#include <upnp.h>
#include <upnpconfig.h>
#include <upnptools.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <ifaddrs.h>
#include <iostream>
#include <mutex>
#include <net/if.h>
#include <netinet/in.h>
#include <optional>
#include <thread>
#include <utility>

namespace trackknife::discovery {
namespace {
using Document = std::unique_ptr<IXML_Document, decltype(&ixmlDocument_free)>;
using Clock = std::chrono::steady_clock;
constexpr const char* renderer_type = "urn:schemas-upnp-org:device:MediaRenderer:";

core::Error error(std::string message, int code = 0) {
    return {.code = code == 401 ? core::ErrorCode::unsupported : core::ErrorCode::io,
            .message = std::move(message),
            .context = {{.key = "upnp", .value = std::to_string(code)}}};
}
std::string text(const char* value) { return value ? value : ""; }
std::string name(IXML_Node* node) {
    auto value = text(ixmlNode_getNodeName(node));
    const auto colon = value.find(':');
    return colon == std::string::npos ? value : value.substr(colon + 1);
}
std::string content(IXML_Node* node) {
    std::string value;
    for (auto* child = ixmlNode_getFirstChild(node); child;
         child = ixmlNode_getNextSibling(child)) {
        if (ixmlNode_getNodeType(child) == eTEXT_NODE ||
            ixmlNode_getNodeType(child) == eCDATA_SECTION_NODE) {
            value += text(ixmlNode_getNodeValue(child));
        }
    }
    return value;
}
IXML_Node* child_named(IXML_Node* node, const std::string& wanted) {
    for (auto* child = ixmlNode_getFirstChild(node); child;
         child = ixmlNode_getNextSibling(child)) {
        if (name(child) == wanted) {
            return child;
        }
    }
    return nullptr;
}
std::string field(IXML_Node* node, const std::string& wanted) {
    auto* child = child_named(node, wanted);
    return child ? content(child) : std::string{};
}
void walk(IXML_Node* node, const std::function<void(IXML_Node*)>& visit) {
    if (!node) {
        return;
    }
    visit(node);
    for (auto* child = ixmlNode_getFirstChild(node); child;
         child = ixmlNode_getNextSibling(child)) {
        walk(child, visit);
    }
}
Document parse(const std::string& xml) {
    // Network documents are bounded and must never resolve external entities.
    if (xml.size() > 1024U * 1024U || xml.find("<!DOCTYPE") != std::string::npos ||
        xml.find("<!ENTITY") != std::string::npos) {
        return {nullptr, ixmlDocument_free};
    }
    return {ixmlParseBuffer(xml.c_str()), ixmlDocument_free};
}
std::string resolve(const std::string& base, const std::string& relative) {
    if (relative.empty()) {
        return {};
    }
    char* result = nullptr;
    if (UpnpResolveURL2(base.c_str(), relative.c_str(), &result) != UPNP_E_SUCCESS) {
        return {};
    }
    std::string url = text(result);
    std::free(result);
    return url.starts_with("http://") ? url : std::string{};
}
std::string serialized(IXML_Document* document) {
    if (!document) {
        return {};
    }
    auto* value = ixmlPrintDocument(document);
    auto result = text(value);
    ixmlFreeDOMString(value);
    return result;
}

std::string default_interface() {
#ifdef __APPLE__
    ifaddrs* raw = nullptr;
    if (::getifaddrs(&raw) != 0) {
        return {};
    }
    const std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> addresses{raw, freeifaddrs};
    for (auto* address = addresses.get(); address != nullptr; address = address->ifa_next) {
        if (address->ifa_addr == nullptr || address->ifa_addr->sa_family != AF_INET ||
            (address->ifa_flags & (IFF_UP | IFF_RUNNING | IFF_MULTICAST)) !=
                (IFF_UP | IFF_RUNNING | IFF_MULTICAST) ||
            (address->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address->ifa_addr);
        const auto host = ntohl(ipv4->sin_addr.s_addr);
        if ((host & 0xffff0000U) == 0xa9fe0000U) {
            continue;
        }
        return address->ifa_name;
    }
#endif
    return {};
}
} // namespace

std::string upnp_xml_escape(const std::string& value) {
    std::string result;
    for (const auto ch : value) {
        switch (ch) {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '"':
            result += "&quot;";
            break;
        case '\'':
            result += "&apos;";
            break;
        case '\t':
        case '\n':
        case '\r':
            result += ch;
            break;
        default:
            // XML 1.0 has no other control characters, escaped or not: one
            // in a tag made the whole DIDL invalid, and the renderer refused
            // the track.
            if (static_cast<unsigned char>(ch) >= 0x20U) {
                result += ch;
            }
        }
    }
    return result;
}
UpnpValues parse_upnp_values(const std::string& xml) {
    auto doc = parse(xml);
    UpnpValues values;
    walk(reinterpret_cast<IXML_Node*>(doc.get()), [&](IXML_Node* node) {
        if (ixmlNode_getNodeType(node) == eELEMENT_NODE) {
            const auto value = content(node);
            if (!value.empty()) {
                values[name(node)] = value;
            }
        }
    });
    return values;
}
UpnpValues parse_upnp_last_change(const std::string& xml) {
    auto outer = parse_upnp_values(xml);
    auto doc = parse(outer.contains("LastChange") ? outer.at("LastChange") : xml);
    UpnpValues values;
    walk(reinterpret_cast<IXML_Node*>(doc.get()), [&](IXML_Node* node) {
        if (name(node) != "InstanceID" ||
            text(ixmlElement_getAttribute(reinterpret_cast<IXML_Element*>(node), "val")) != "0") {
            return;
        }
        for (auto* child = ixmlNode_getFirstChild(node); child;
             child = ixmlNode_getNextSibling(child)) {
            if (ixmlNode_getNodeType(child) != eELEMENT_NODE) {
                continue;
            }
            auto* element = reinterpret_cast<IXML_Element*>(child);
            const auto channel = text(ixmlElement_getAttribute(element, "channel"));
            if (!channel.empty() && channel != "Master") {
                continue;
            }
            values[name(child)] = text(ixmlElement_getAttribute(element, "val"));
        }
    });
    return values;
}
core::Result<UpnpRenderer> parse_upnp_renderer(const std::string& xml, const std::string& location,
                                               const std::string& address) {
    auto doc = parse(xml);
    if (!doc) {
        return std::unexpected(error("invalid UPnP description"));
    }
    auto* root = child_named(reinterpret_cast<IXML_Node*>(doc.get()), "root");
    const auto base_value = field(root, "URLBase");
    const auto base = base_value.empty() ? location : base_value;
    std::optional<UpnpRenderer> result;
    walk(root, [&](IXML_Node* node) {
        if (result || name(node) != "device" ||
            !field(node, "deviceType").starts_with(renderer_type)) {
            return;
        }
        UpnpRenderer renderer;
        renderer.udn = field(node, "UDN");
        renderer.name = field(node, "friendlyName");
        renderer.manufacturer = field(node, "manufacturer");
        renderer.model = field(node, "modelName");
        renderer.address = address;
        auto* services = child_named(node, "serviceList");
        for (auto* service = ixmlNode_getFirstChild(services); service;
             service = ixmlNode_getNextSibling(service)) {
            if (name(service) != "service") {
                continue;
            }
            UpnpService parsed{.type = field(service, "serviceType"),
                               .control_url = resolve(base, field(service, "controlURL")),
                               .event_url = resolve(base, field(service, "eventSubURL"))};
            if (parsed.type.starts_with("urn:schemas-upnp-org:service:AVTransport:")) {
                renderer.transport = parsed;
            }
            if (parsed.type.starts_with("urn:schemas-upnp-org:service:RenderingControl:")) {
                renderer.rendering = parsed;
            }
            if (parsed.type.starts_with("urn:schemas-upnp-org:service:ConnectionManager:")) {
                renderer.connection = parsed;
            }
        }
        if (!renderer.udn.empty() && !renderer.transport.control_url.empty() &&
            !renderer.connection.control_url.empty()) {
            result = std::move(renderer);
        }
    });
    if (!result) {
        return std::unexpected(error("description has no usable MediaRenderer"));
    }
    return *result;
}

struct UpnpDiscovery::Impl {
    struct Notice {
        std::string udn;
        std::string location;
        int expires{0};
    };
    struct Known {
        UpnpRenderer renderer;
        Clock::time_point expires;
        std::string location;
        std::string root_udn;
        std::vector<std::string> subscriptions;
    };
    UpnpClient_Handle handle{-1};
    Changed changed;
    std::string address;
    mutable std::mutex mutex;
    std::condition_variable wake;
    bool stopping{false};
    std::deque<Notice> notices;
    std::map<std::string, Known> known;
    std::map<std::string, UpnpValues> reports;
    std::thread worker;

#if UPNP_VERSION_MAJOR >= 2
    using EventPointer = void*;
#else
    using EventPointer = const void*;
#endif
    static int callback(Upnp_EventType type, EventPointer event, void* cookie) {
        auto& self = *static_cast<Impl*>(cookie);
        const std::lock_guard lock{self.mutex};
        if (self.stopping) {
            return 0;
        }
        if (type == UPNP_DISCOVERY_ADVERTISEMENT_ALIVE || type == UPNP_DISCOVERY_SEARCH_RESULT ||
            type == UPNP_DISCOVERY_ADVERTISEMENT_BYEBYE) {
            const auto* notice = static_cast<const ::UpnpDiscovery*>(event);
            if (UpnpDiscovery_get_ErrCode(notice) != UPNP_E_SUCCESS) {
                return 0;
            }
            // A bounded queue coalesces repeated announcements by UDN.
            const auto udn = text(UpnpDiscovery_get_DeviceID_cstr(notice));
            std::erase_if(self.notices, [&](const Notice& n) { return n.udn == udn; });
            if (self.notices.size() < 128U) {
                self.notices.push_back({udn, text(UpnpDiscovery_get_Location_cstr(notice)),
                                        type == UPNP_DISCOVERY_ADVERTISEMENT_BYEBYE
                                            ? 0
                                            : UpnpDiscovery_get_Expires(notice)});
                self.wake.notify_one();
            }
        } else if (type == UPNP_EVENT_RECEIVED) {
            const auto* received = static_cast<const UpnpEvent*>(event);
            const auto sid = text(UpnpEvent_get_SID_cstr(received));
            const auto values =
                parse_upnp_last_change(serialized(UpnpEvent_get_ChangedVariables(received)));
            // Unknown/early subscription events are discarded; polling fills the gap.
            for (const auto& [udn, entry] : self.known) {
                if (std::ranges::find(entry.subscriptions, sid) != entry.subscriptions.end()) {
                    self.reports[udn] = values;
                }
            }
        }
        return 0;
    }

    void run() {
        auto search_at = Clock::now();
        for (;;) {
            std::optional<Notice> notice;
            std::vector<UpnpRenderer> expired;
            {
                std::unique_lock lock{mutex};
                wake.wait_for(lock, std::chrono::seconds{1},
                              [&] { return stopping || !notices.empty(); });
                if (stopping) {
                    return;
                }
                if (!notices.empty()) {
                    notice = std::move(notices.front());
                    notices.pop_front();
                }
                for (auto& [udn, entry] : known) {
                    if (entry.renderer.online && entry.expires <= Clock::now()) {
                        entry.renderer.online = false;
                        reports.erase(udn);
                        expired.push_back(entry.renderer);
                    }
                }
            }
            for (const auto& renderer : expired) {
                changed(renderer);
            }
            if (Clock::now() >= search_at) {
                const auto searched =
                    UpnpSearchAsync(handle, 3, "urn:schemas-upnp-org:device:MediaRenderer:1", this);
                if (searched != UPNP_E_SUCCESS) {
                    std::cerr << "melodyd: UPnP discovery search failed: " << searched << '\n';
                }
                search_at = Clock::now() + std::chrono::seconds{30};
            }
            if (!notice) {
                continue;
            }
            std::optional<UpnpRenderer> gone;
            {
                const std::lock_guard lock{mutex};
                const auto found = std::ranges::find_if(known, [&notice](const auto& item) {
                    return item.first == notice->udn || item.second.root_udn == notice->udn;
                });
                if (found != known.end()) {
                    auto& entry = found->second;
                    if (notice->expires <= 0) {
                        entry.renderer.online = false;
                        reports.erase(found->first);
                        gone = entry.renderer;
                    } else if (entry.renderer.online) {
                        entry.expires = Clock::now() +
                                        std::chrono::seconds{std::clamp(notice->expires, 1, 3600)};
                        continue;
                    }
                } else if (known.size() >= 128U) {
                    continue;
                }
            }
            if (gone) {
                changed(*gone);
                continue;
            }
            if (notice->expires <= 0 || !notice->location.starts_with("http://")) {
                continue;
            }
            IXML_Document* raw = nullptr;
            const auto downloaded = UpnpDownloadXmlDoc(notice->location.c_str(), &raw);
            Document doc{raw, ixmlDocument_free};
            if (downloaded != UPNP_E_SUCCESS) {
                continue;
            }
            auto renderer = parse_upnp_renderer(serialized(doc.get()), notice->location, address);
            if (!renderer) {
                continue;
            }
            std::string root_udn;
            {
                const std::lock_guard lock{mutex};
                if (const auto previous = known.find(renderer->udn); previous != known.end()) {
                    root_udn = previous->second.root_udn;
                }
            }
            if (notice->udn != renderer->udn) {
                root_udn = notice->udn;
            }
            Known entry{.renderer = *renderer,
                        .expires = Clock::now() +
                                   std::chrono::seconds{std::clamp(notice->expires, 1, 3600)},
                        .location = notice->location,
                        .root_udn = std::move(root_udn),
                        .subscriptions = {}};
            for (const auto* service : {&renderer->transport, &renderer->rendering}) {
                if (service->event_url.empty()) {
                    continue;
                }
                int timeout = 300;
                Upnp_SID sid{};
                if (UpnpSubscribe(handle, service->event_url.c_str(), &timeout, sid) ==
                    UPNP_E_SUCCESS) {
                    entry.subscriptions.emplace_back(sid);
                }
            }
            std::vector<std::string> old;
            {
                const std::lock_guard lock{mutex};
                if (known.contains(renderer->udn)) {
                    old = known.at(renderer->udn).subscriptions;
                }
                known[renderer->udn] = std::move(entry);
                reports.erase(renderer->udn);
            }
            for (const auto& sid : old) {
                UpnpUnSubscribe(handle, sid.c_str());
            }
            changed(*renderer);
        }
    }
}; // namespace trackknife::discovery
UpnpDiscovery::UpnpDiscovery(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
core::Result<std::shared_ptr<UpnpDiscovery>> UpnpDiscovery::start(Changed changed,
                                                                  const std::string& interface) {
    const auto selected_interface = interface.empty() ? default_interface() : interface;
    const auto initialized =
        UpnpInit2(selected_interface.empty() ? nullptr : selected_interface.c_str(), 0);
    if (initialized != UPNP_E_SUCCESS) {
        return std::unexpected(error("could not start UPnP discovery", initialized));
    }
    UpnpSetMaxContentLength(1024U * 1024U);
    auto impl = std::make_unique<Impl>();
    impl->changed = std::move(changed);
    impl->address = text(UpnpGetServerIpAddress());
    std::cerr << "melodyd: UPnP discovery using "
              << (selected_interface.empty() ? "the default interface" : selected_interface)
              << " at " << impl->address << '\n';
    const auto registered = UpnpRegisterClient(Impl::callback, impl.get(), &impl->handle);
    if (registered != UPNP_E_SUCCESS) {
        UpnpFinish();
        return std::unexpected(error("could not register UPnP control point", registered));
    }
    auto discovery = std::shared_ptr<UpnpDiscovery>{new UpnpDiscovery{std::move(impl)}};
    return discovery;
}
std::string UpnpDiscovery::address() const { return impl_->address; }
void UpnpDiscovery::begin() {
    impl_->worker = std::thread{[this] { impl_->run(); }};
}
void UpnpDiscovery::stop() {
    {
        const std::lock_guard lock{impl_->mutex};
        impl_->stopping = true;
        impl_->wake.notify_one();
    }
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
}
UpnpDiscovery::~UpnpDiscovery() {
    stop();
    UpnpUnRegisterClient(impl_->handle);
    UpnpFinish();
}
core::Result<UpnpValues> UpnpDiscovery::action(const UpnpService& service,
                                               const std::string& action_name,
                                               const UpnpValues& arguments) {
    if (service.control_url.empty()) {
        return std::unexpected(error("renderer has no such service", 401));
    }
    IXML_Document* raw = nullptr;
    if (arguments.empty()) {
        raw = UpnpMakeAction(action_name.c_str(), service.type.c_str(), 0, nullptr);
    }
    for (const auto& [key, value] : arguments) {
        UpnpAddToAction(&raw, action_name.c_str(), service.type.c_str(), key.c_str(),
                        value.c_str());
    }
    Document request{raw, ixmlDocument_free};
    IXML_Document* response = nullptr;
    const auto status = UpnpSendAction(impl_->handle, service.control_url.c_str(),
                                       service.type.c_str(), nullptr, request.get(), &response);
    Document reply{response, ixmlDocument_free};
    if (status != UPNP_E_SUCCESS) {
        return std::unexpected(error("UPnP " + action_name + " failed", status));
    }
    return parse_upnp_values(serialized(reply.get()));
}
UpnpValues UpnpDiscovery::take_events(const std::string& udn) {
    const std::lock_guard lock{impl_->mutex};
    const auto found = impl_->reports.find(udn);
    if (found == impl_->reports.end()) {
        return {};
    }
    auto values = std::move(found->second);
    impl_->reports.erase(found);
    return values;
}
} // namespace trackknife::discovery
