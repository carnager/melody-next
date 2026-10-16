// SPDX-License-Identifier: GPL-3.0-only

#include "config_methods.hpp"

#include "trackknife/core/config_file.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

namespace trackknife::daemon {
namespace {

using core::ConfigFile;
using protocol::Json;

// Never sent back: that one is set is all a client is told.
constexpr std::array secret_keys{std::string_view{"password"},
                                 std::string_view{"agent-password"}};

[[nodiscard]] bool secret(const std::string_view key) {
    return std::ranges::contains(secret_keys, key);
}

[[nodiscard]] core::Error invalid(std::string message) {
    return core::Error{
        .code = core::ErrorCode::invalid_argument, .message = std::move(message), .context = {}};
}

[[nodiscard]] std::optional<std::string> read_text(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        return std::nullopt;
    }
    return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] Json described(const std::vector<ConfigFile::Entry>& entries) {
    auto settings = Json::object();
    auto secrets = Json::array();
    for (const auto& entry : entries) {
        if (secret(entry.key)) {
            secrets.push_back(entry.key);
        } else {
            settings[entry.key] = entry.value;
        }
    }
    return Json{{"settings", std::move(settings)}, {"secrets", std::move(secrets)}};
}

} // namespace

void register_config_methods(protocol::Dispatcher& dispatcher, const Settings& settings,
                             std::function<void(bool for_good)> stop) {
    const auto path = settings.config_path;
    const auto from_flags = settings.from_flags;

    dispatcher.on("config.get", [path, from_flags](const Json&) -> core::Result<Json> {
        const auto text = read_text(path);
        auto parsed = ConfigFile::parse(text.value_or(std::string{}), path.string());
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        auto answer = described(parsed->top());
        answer["path"] = path.string();
        answer["exists"] = text.has_value();
        answer["engines"] = Json::array();
        for (const auto& section : parsed->sections()) {
            auto engine = described(section.entries);
            engine["name"] = section.name;
            answer["engines"].push_back(std::move(engine));
        }
        answer["from_flags"] = from_flags;
        return answer;
    });

    dispatcher.on("config.set", [path, from_flags](const Json& params) -> core::Result<Json> {
        const auto changes = params.find("changes");
        if (changes == params.end() || !changes->is_array() || changes->empty()) {
            return std::unexpected(invalid("config.set wants changes: [{key, value, engine?}]"));
        }
        const auto text = read_text(path);
        auto file = ConfigFile::parse(text ? *text : std::string{config_skeleton()},
                                      path.string());
        if (!file) {
            return std::unexpected(file.error());
        }
        auto overridden = Json::array();
        for (const auto& change : *changes) {
            if (!change.is_object() || !change.contains("key") || !change.at("key").is_string()) {
                return std::unexpected(invalid("each change names its key"));
            }
            const auto key = change.at("key").get<std::string>();
            std::string engine;
            if (const auto named = change.find("engine"); named != change.end()) {
                if (!named->is_string() || named->get<std::string>().empty()) {
                    return std::unexpected(invalid("engine is the name of an [engine] section"));
                }
                engine = named->get<std::string>();
            }
            const std::string_view kind = engine.empty() ? "" : "engine";
            const auto value = change.find("value");
            if (value == change.end() || value->is_null()) {
                static_cast<void>(file->unset(kind, engine, key));
            } else if (!value->is_string()) {
                return std::unexpected(invalid(key + ": a value is text"));
            } else if (auto set = file->set(kind, engine, key, value->get<std::string>()); !set) {
                return std::unexpected(set.error());
            }
            if (engine.empty() && std::ranges::contains(from_flags, key)) {
                overridden.push_back(key);
            }
        }
        // As melodyd will read it, or not at all.
        if (auto checked = check_config(file->text(), path.string()); !checked) {
            return std::unexpected(checked.error());
        }
        if (auto written = core::write_config_file(path, file->text()); !written) {
            return std::unexpected(written.error());
        }
        return Json{{"path", path.string()}, {"restart", true}, {"overridden", overridden}};
    });

    dispatcher.on("engine.restart", [stop](const Json&) {
        stop(false);
        return core::Result<Json>{Json{{"restarting", true}}};
    });
    dispatcher.on("engine.stop", [stop](const Json&) {
        stop(true);
        return core::Result<Json>{Json{{"stopping", true}}};
    });
}

} // namespace trackknife::daemon
