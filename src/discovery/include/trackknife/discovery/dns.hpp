// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::discovery {

// The few DNS record types service discovery uses (RFC 6762/6763).
enum class RecordType : std::uint16_t {
    a = 1,
    ptr = 12,
    txt = 16,
    srv = 33,
    any = 255,
};

struct Question final {
    std::string name;
    RecordType type{RecordType::any};

    friend bool operator==(const Question&, const Question&) = default;
};

struct Record final {
    std::string name;
    RecordType type{RecordType::a};
    std::uint32_t ttl{120};
    // By type: PTR's target name; SRV's target and port; TXT's strings;
    // A's IPv4 address in network order.
    std::string target;
    std::uint16_t port{0};
    std::vector<std::string> strings;
    std::uint32_t address{0};

    friend bool operator==(const Record&, const Record&) = default;
};

struct Message final {
    bool response{false};
    std::vector<Question> questions;
    std::vector<Record> answers;
    // Additional records, as responders send SRV, TXT and A with a PTR.
    std::vector<Record> additionals;

    friend bool operator==(const Message&, const Message&) = default;
};

[[nodiscard]] std::vector<std::uint8_t> encode(const Message& message);
// Nothing for a packet that is not a DNS message this can read; records of
// types it does not know are skipped.
[[nodiscard]] std::optional<Message> decode(const std::uint8_t* data, std::size_t size);

// Names compare without regard to ASCII case, as DNS does.
[[nodiscard]] bool same_name(const std::string& left, const std::string& right);

} // namespace trackknife::discovery
