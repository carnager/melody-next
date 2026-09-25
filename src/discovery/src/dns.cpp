// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/discovery/dns.hpp"

#include <algorithm>
#include <cctype>

namespace trackknife::discovery {
namespace {

constexpr std::uint16_t class_in = 1U;
// mDNS: the top bit of a record's class asks others to replace, not add
// (cache flush); of a question's, to answer by unicast. Neither changes what
// the record means.
constexpr std::uint16_t class_mask = 0x7FFFU;

class Writer final {
  public:
    void u8(const std::uint8_t value) { bytes_.push_back(value); }
    void u16(const std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value >> 8U));
        u8(static_cast<std::uint8_t>(value & 0xFFU));
    }
    void u32(const std::uint32_t value) {
        u16(static_cast<std::uint16_t>(value >> 16U));
        u16(static_cast<std::uint16_t>(value & 0xFFFFU));
    }
    void name(const std::string& dotted) {
        std::size_t start = 0;
        while (start < dotted.size()) {
            auto end = dotted.find('.', start);
            if (end == std::string::npos) {
                end = dotted.size();
            }
            const auto length = std::min<std::size_t>(end - start, 63U);
            u8(static_cast<std::uint8_t>(length));
            bytes_.insert(bytes_.end(), dotted.begin() + static_cast<std::ptrdiff_t>(start),
                          dotted.begin() + static_cast<std::ptrdiff_t>(start + length));
            start = end + 1U;
        }
        u8(0U);
    }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    void patch_u16(const std::size_t at, const std::uint16_t value) {
        bytes_[at] = static_cast<std::uint8_t>(value >> 8U);
        bytes_[at + 1U] = static_cast<std::uint8_t>(value & 0xFFU);
    }
    [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(bytes_); }

  private:
    std::vector<std::uint8_t> bytes_;
};

void write_record(Writer& out, const Record& record) {
    out.name(record.name);
    out.u16(static_cast<std::uint16_t>(record.type));
    out.u16(class_in);
    out.u32(record.ttl);
    const auto length_at = out.size();
    out.u16(0U);
    const auto start = out.size();
    switch (record.type) {
    case RecordType::ptr:
        out.name(record.target);
        break;
    case RecordType::srv:
        out.u16(0U); // priority
        out.u16(0U); // weight
        out.u16(record.port);
        out.name(record.target);
        break;
    case RecordType::txt:
        if (record.strings.empty()) {
            out.u8(0U);
        }
        for (const auto& text : record.strings) {
            const auto length = std::min<std::size_t>(text.size(), 255U);
            out.u8(static_cast<std::uint8_t>(length));
            for (std::size_t index = 0; index < length; ++index) {
                out.u8(static_cast<std::uint8_t>(text[index]));
            }
        }
        break;
    case RecordType::a:
        out.u32(record.address);
        break;
    case RecordType::any:
        break;
    }
    out.patch_u16(length_at, static_cast<std::uint16_t>(out.size() - start));
}

class Reader final {
  public:
    Reader(const std::uint8_t* data, const std::size_t size) : data_(data), size_(size) {}

    [[nodiscard]] bool u8(std::uint8_t& value) {
        if (at_ + 1U > size_) {
            return false;
        }
        value = data_[at_++];
        return true;
    }
    [[nodiscard]] bool u16(std::uint16_t& value) {
        std::uint8_t high = 0;
        std::uint8_t low = 0;
        if (!u8(high) || !u8(low)) {
            return false;
        }
        value = static_cast<std::uint16_t>((high << 8U) | low);
        return true;
    }
    [[nodiscard]] bool u32(std::uint32_t& value) {
        std::uint16_t high = 0;
        std::uint16_t low = 0;
        if (!u16(high) || !u16(low)) {
            return false;
        }
        value = (static_cast<std::uint32_t>(high) << 16U) | low;
        return true;
    }
    // A name, following compression pointers (bounded, so a loop cannot).
    [[nodiscard]] bool name(std::string& dotted) {
        dotted.clear();
        auto position = at_;
        bool jumped = false;
        for (int hops = 0; hops < 32; ++hops) {
            if (position >= size_) {
                return false;
            }
            const auto length = data_[position];
            if ((length & 0xC0U) == 0xC0U) {
                if (position + 1U >= size_) {
                    return false;
                }
                const auto target =
                    static_cast<std::size_t>(((length & 0x3FU) << 8U) | data_[position + 1U]);
                if (!jumped) {
                    at_ = position + 2U;
                    jumped = true;
                }
                position = target;
                continue;
            }
            if (length == 0U) {
                if (!jumped) {
                    at_ = position + 1U;
                }
                return true;
            }
            if (position + 1U + length > size_) {
                return false;
            }
            if (!dotted.empty()) {
                dotted.push_back('.');
            }
            dotted.append(reinterpret_cast<const char*>(data_ + position + 1U), length);
            position += 1U + length;
        }
        return false;
    }
    [[nodiscard]] std::size_t position() const noexcept { return at_; }
    void seek(const std::size_t position) noexcept { at_ = position; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] const std::uint8_t* data() const noexcept { return data_; }

  private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t at_{0};
};

// One record, or nothing when unreadable; `known` says whether it is a type
// this reads (others are skipped, not failed).
[[nodiscard]] bool read_record(Reader& in, Record& record, bool& known) {
    std::uint16_t type = 0;
    std::uint16_t klass = 0;
    std::uint16_t length = 0;
    if (!in.name(record.name) || !in.u16(type) || !in.u16(klass) || !in.u32(record.ttl) ||
        !in.u16(length)) {
        return false;
    }
    const auto end = in.position() + length;
    if (end > in.size()) {
        return false;
    }
    known = (klass & class_mask) == class_in;
    record.type = static_cast<RecordType>(type);
    switch (record.type) {
    case RecordType::ptr:
        known = known && in.name(record.target);
        break;
    case RecordType::srv: {
        std::uint16_t priority = 0;
        std::uint16_t weight = 0;
        known = known && in.u16(priority) && in.u16(weight) && in.u16(record.port) &&
                in.name(record.target);
        break;
    }
    case RecordType::txt:
        while (known && in.position() < end) {
            std::uint8_t text_length = 0;
            if (!in.u8(text_length) || in.position() + text_length > end) {
                known = false;
                break;
            }
            if (text_length > 0U) {
                record.strings.emplace_back(
                    reinterpret_cast<const char*>(in.data() + in.position()), text_length);
            }
            in.seek(in.position() + text_length);
        }
        break;
    case RecordType::a:
        known = known && length == 4U && in.u32(record.address);
        break;
    default:
        known = false;
        break;
    }
    in.seek(end);
    return true;
}

} // namespace

std::vector<std::uint8_t> encode(const Message& message) {
    Writer out;
    out.u16(0U); // mDNS ids are zero
    out.u16(message.response ? 0x8400U : 0U);
    out.u16(static_cast<std::uint16_t>(message.questions.size()));
    out.u16(static_cast<std::uint16_t>(message.answers.size()));
    out.u16(0U);
    out.u16(static_cast<std::uint16_t>(message.additionals.size()));
    for (const auto& question : message.questions) {
        out.name(question.name);
        out.u16(static_cast<std::uint16_t>(question.type));
        out.u16(static_cast<std::uint16_t>(class_in | (question.unicast ? 0x8000U : 0U)));
    }
    for (const auto& record : message.answers) {
        write_record(out, record);
    }
    for (const auto& record : message.additionals) {
        write_record(out, record);
    }
    return out.take();
}

std::optional<Message> decode(const std::uint8_t* data, const std::size_t size) {
    Reader in{data, size};
    std::uint16_t id = 0;
    std::uint16_t flags = 0;
    std::uint16_t questions = 0;
    std::uint16_t answers = 0;
    std::uint16_t authorities = 0;
    std::uint16_t additionals = 0;
    if (!in.u16(id) || !in.u16(flags) || !in.u16(questions) || !in.u16(answers) ||
        !in.u16(authorities) || !in.u16(additionals)) {
        return std::nullopt;
    }
    Message message;
    message.response = (flags & 0x8000U) != 0U;
    for (std::uint16_t index = 0; index < questions; ++index) {
        Question question;
        std::uint16_t type = 0;
        std::uint16_t klass = 0;
        if (!in.name(question.name) || !in.u16(type) || !in.u16(klass)) {
            return std::nullopt;
        }
        question.type = static_cast<RecordType>(type);
        question.unicast = (klass & 0x8000U) != 0U;
        message.questions.push_back(std::move(question));
    }
    const auto read_section = [&in](const std::uint16_t count, std::vector<Record>& into) {
        for (std::uint16_t index = 0; index < count; ++index) {
            Record record;
            bool known = false;
            if (!read_record(in, record, known)) {
                return false;
            }
            if (known) {
                into.push_back(std::move(record));
            }
        }
        return true;
    };
    std::vector<Record> ignored;
    if (!read_section(answers, message.answers) || !read_section(authorities, ignored) ||
        !read_section(additionals, message.additionals)) {
        return std::nullopt;
    }
    return message;
}

bool same_name(const std::string& left, const std::string& right) {
    return std::ranges::equal(left, right, [](const char a, const char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    });
}

} // namespace trackknife::discovery
