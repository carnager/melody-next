// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// What the file-work encodings share: reading an object field by field with
// the first problem kept, and writing optional and listed values. Internal to
// the engine library's wire files.

#include "trackknife/engine/file_work_wire.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trackknife::engine::wire::detail {

// Reads one object, keeping the first thing found wrong; every accessor
// answers a default once something is, and the caller checks ok() at the end.
class Reader final {
  public:
    explicit Reader(const Json& object) : object_(object) {
        if (!object.is_object()) {
            fail("expected an object");
        }
    }

    [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
    [[nodiscard]] core::Error error() const { return *error_; }
    void fail(const std::string& what) {
        if (!error_) {
            error_ = core::Error{.code = core::ErrorCode::invalid_argument,
                                 .message = "malformed file-work document: " + what,
                                 .context = {}};
        }
    }
    void adopt(const core::Error& error) {
        if (!error_) {
            error_ = error;
        }
    }

    [[nodiscard]] const Json* find(const std::string_view name) {
        if (!ok()) {
            return nullptr;
        }
        const auto found = object_.find(name);
        return found == object_.end() || found->is_null() ? nullptr : &*found;
    }
    [[nodiscard]] const Json* need(const std::string_view name) {
        const auto* found = find(name);
        if (found == nullptr) {
            fail(std::string{name} + " is missing");
        }
        return found;
    }

    [[nodiscard]] std::size_t index(const std::string_view name) {
        const auto* value = need(name);
        if (value == nullptr) {
            return 0U;
        }
        if (value->is_number_unsigned()) {
            return value->get<std::size_t>();
        }
        if (value->is_number_integer() && value->get<std::int64_t>() >= 0) {
            return static_cast<std::size_t>(value->get<std::int64_t>());
        }
        fail(std::string{name} + " must be a non-negative integer");
        return 0U;
    }
    [[nodiscard]] std::optional<std::size_t> optional_index(const std::string_view name) {
        if (find(name) == nullptr) {
            return std::nullopt;
        }
        return index(name);
    }
    [[nodiscard]] std::vector<std::size_t> indexes(const std::string_view name) {
        std::vector<std::size_t> result;
        const auto* value = need(name);
        if (value == nullptr) {
            return result;
        }
        if (!value->is_array()) {
            fail(std::string{name} + " must be a list");
            return result;
        }
        for (const auto& entry : *value) {
            if (entry.is_number_unsigned()) {
                result.push_back(entry.get<std::size_t>());
            } else {
                fail(std::string{name} + " must hold non-negative integers");
            }
        }
        return result;
    }
    [[nodiscard]] bool flag(const std::string_view name) {
        const auto* value = need(name);
        if (value != nullptr && !value->is_boolean()) {
            fail(std::string{name} + " must be true or false");
            return false;
        }
        return value != nullptr && value->get<bool>();
    }
    [[nodiscard]] std::string text(const std::string_view name) {
        const auto* value = need(name);
        return value == nullptr ? std::string{} : take(decode_text(*value));
    }
    [[nodiscard]] std::optional<std::string> optional_text(const std::string_view name) {
        const auto* value = find(name);
        return value == nullptr ? std::nullopt : std::optional{take(decode_text(*value))};
    }
    [[nodiscard]] std::vector<std::string> texts(const std::string_view name) {
        std::vector<std::string> result;
        const auto* value = need(name);
        if (value == nullptr) {
            return result;
        }
        if (!value->is_array()) {
            fail(std::string{name} + " must be a list");
            return result;
        }
        for (const auto& entry : *value) {
            result.push_back(take(decode_text(entry)));
        }
        return result;
    }
    [[nodiscard]] std::string bytes(const std::string_view name) {
        const auto* value = need(name);
        if (value == nullptr) {
            return {};
        }
        if (value->is_string()) {
            if (auto decoded = protocol::decode_raw_path(value->get<std::string>())) {
                return std::move(*decoded);
            }
        }
        fail(std::string{name} + " must be encoded bytes");
        return {};
    }
    [[nodiscard]] core::LocalSourceRevision revision(const std::string_view name) {
        const auto* value = need(name);
        return value == nullptr ? core::LocalSourceRevision{} : take(decode_revision(*value));
    }
    [[nodiscard]] std::optional<core::LocalSourceRevision>
    optional_revision(const std::string_view name) {
        const auto* value = find(name);
        return value == nullptr ? std::nullopt : std::optional{take(decode_revision(*value))};
    }
    [[nodiscard]] std::optional<core::Error> optional_error(const std::string_view name) {
        const auto* value = find(name);
        return value == nullptr ? std::nullopt : std::optional{take(decode_error(*value))};
    }
    template <typename Enum, std::size_t Count>
    [[nodiscard]] Enum named(const std::string_view name,
                             const std::array<std::pair<Enum, std::string_view>, Count>& names) {
        const auto* value = need(name);
        if (value != nullptr && value->is_string()) {
            for (const auto& [kind, spelled] : names) {
                if (spelled == value->get_ref<const std::string&>()) {
                    return kind;
                }
            }
        }
        fail(std::string{name} + " is not one of its known values");
        return names.front().first;
    }
    template <typename T>
    [[nodiscard]] std::vector<T> list(const std::string_view name,
                                      const std::function<T(Reader&)>& read) {
        std::vector<T> result;
        const auto* value = need(name);
        if (value == nullptr) {
            return result;
        }
        if (!value->is_array()) {
            fail(std::string{name} + " must be a list");
            return result;
        }
        for (const auto& entry : *value) {
            Reader nested{entry};
            auto item = read(nested);
            if (!nested.ok()) {
                adopt(nested.error());
                return result;
            }
            result.push_back(std::move(item));
        }
        return result;
    }
    template <typename T>
    [[nodiscard]] std::optional<T> optional_object(const std::string_view name,
                                                   const std::function<T(Reader&)>& read) {
        const auto* value = find(name);
        if (value == nullptr) {
            return std::nullopt;
        }
        Reader nested{*value};
        auto item = read(nested);
        if (!nested.ok()) {
            adopt(nested.error());
            return std::nullopt;
        }
        return item;
    }

  private:
    template <typename T> [[nodiscard]] T take(core::Result<T> result) {
        if (!result) {
            adopt(result.error());
            return T{};
        }
        return std::move(*result);
    }

    const Json& object_;
    std::optional<core::Error> error_;
};

template <typename Enum, std::size_t Count>
[[nodiscard]] std::string
name_of(const Enum value, const std::array<std::pair<Enum, std::string_view>, Count>& names) {
    for (const auto& [kind, spelled] : names) {
        if (kind == value) {
            return std::string{spelled};
        }
    }
    return std::string{names.front().second};
}

[[nodiscard]] inline Json encode_texts(const std::vector<std::string>& texts) {
    auto list = Json::array();
    for (const auto& text : texts) {
        list.push_back(encode_text(text));
    }
    return list;
}

[[nodiscard]] inline Json
encode_optional_revision(const std::optional<core::LocalSourceRevision>& value) {
    return value ? encode(*value) : Json();
}

[[nodiscard]] inline Json encode_optional_text(const std::optional<std::string>& value) {
    return value ? encode_text(*value) : Json();
}

[[nodiscard]] inline Json encode_optional_error(const std::optional<core::Error>& value) {
    return value ? encode(*value) : Json();
}

template <typename T, typename Encode>
[[nodiscard]] Json encode_list(const std::vector<T>& values, Encode encode_one) {
    auto list = Json::array();
    for (const auto& value : values) {
        list.push_back(encode_one(value));
    }
    return list;
}

[[nodiscard]] Json encode_commit(const operations::MetadataCommitResult& commit);
[[nodiscard]] operations::MetadataCommitResult read_commit(Reader& in);

} // namespace trackknife::engine::wire::detail
