// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/catalogue_methods.hpp"

#include "trackknife/protocol/message.hpp"
#include "trackknife/query/tkq.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace trackknife::engine {
namespace {

using protocol::Json;

[[nodiscard]] core::Error bad_params(std::string message, std::string member) {
    return core::Error{.code = core::ErrorCode::invalid_argument,
                       .message = std::move(message),
                       .context = {{.key = "param", .value = std::move(member)}}};
}

[[nodiscard]] core::Result<std::string> required_string(const Json& params, const char* name) {
    const auto found = params.find(name);
    if (found == params.end() || !found->is_string()) {
        return std::unexpected(bad_params("a string parameter is required", name));
    }
    return found->get<std::string>();
}

// A list of raw paths arrives base64 encoded, because a path is bytes.
[[nodiscard]] Json encoded_paths(const std::vector<std::string>& raw_paths) {
    auto encoded = Json::array();
    for (const auto& raw_path : raw_paths) {
        encoded.push_back(protocol::encode_raw_path(raw_path));
    }
    return encoded;
}

} // namespace

void register_catalogue_methods(protocol::Dispatcher& dispatcher, Catalogue& catalogue) {
    dispatcher.on("catalogue.roots", [&catalogue](const Json&) -> core::Result<Json> {
        auto roots = catalogue.roots();
        if (!roots) {
            return std::unexpected(std::move(roots.error()));
        }
        auto rendered = Json::array();
        for (const auto& root : *roots) {
            rendered.push_back(Json{{"path", protocol::encode_raw_path(root.raw_path)},
                                    {"available", root.available},
                                    {"error", root.error}});
        }
        return Json{{"roots", std::move(rendered)}};
    });

    dispatcher.on("catalogue.filter_paths", [&catalogue](const Json& params) -> core::Result<Json> {
        auto source = required_string(params, "query");
        if (!source) {
            return std::unexpected(std::move(source.error()));
        }
        // A malformed query is the caller's mistake, and the compiler already
        // says which part; that error is forwarded rather than restated.
        auto compiled = query::compile_tkq(*source);
        if (!compiled) {
            return std::unexpected(std::move(compiled.error()));
        }
        auto paths = catalogue.filter_paths(*compiled);
        if (!paths) {
            return std::unexpected(std::move(paths.error()));
        }
        return Json{{"paths", encoded_paths(*paths)}};
    });

    dispatcher.on("catalogue.ratings", [&catalogue](const Json& params) -> core::Result<Json> {
        const auto found = params.find("hashes");
        if (found == params.end() || !found->is_array()) {
            return std::unexpected(bad_params("an array of hashes is required", "hashes"));
        }
        std::vector<std::string> hashes;
        for (const auto& entry : *found) {
            if (!entry.is_string()) {
                return std::unexpected(bad_params("each hash must be a string", "hashes"));
            }
            hashes.push_back(entry.get<std::string>());
        }
        auto ratings = catalogue.ratings(hashes);
        if (!ratings) {
            return std::unexpected(std::move(ratings.error()));
        }
        return Json{{"ratings", *ratings}};
    });

    dispatcher.on("catalogue.set_rating", [&catalogue](const Json& params) -> core::Result<Json> {
        auto hash = required_string(params, "hash");
        if (!hash) {
            return std::unexpected(std::move(hash.error()));
        }
        const auto album = params.value("album", false);
        // Not is_number_unsigned(): a JSON 7 arrives as a signed integer
        // unless the sender went out of its way, and requiring unsignedness
        // would reject every ordinary client for no benefit. The range check
        // is what actually matters.
        const auto rating = params.find("rating");
        if (rating == params.end() || !rating->is_number_integer() ||
            rating->get<std::int64_t>() < 0) {
            return std::unexpected(bad_params("rating must be a non-negative integer", "rating"));
        }
        auto stored =
            catalogue.set_rating(*hash, album, static_cast<unsigned>(rating->get<std::int64_t>()));
        if (!stored) {
            return std::unexpected(std::move(stored.error()));
        }
        // A void operation still answers, so the caller learns it completed.
        return Json{};
    });

    dispatcher.on(
        "catalogue.artwork_source", [&catalogue](const Json& params) -> core::Result<Json> {
            auto key = required_string(params, "album_key");
            if (!key) {
                return std::unexpected(std::move(key.error()));
            }
            auto source = catalogue.artwork_source(*key);
            if (!source) {
                return std::unexpected(std::move(source.error()));
            }
            // An album with no artwork is a success carrying null,
            // not a not_found: the album exists, the cover does not.
            //
            // Built by assignment rather than brace initialisation:
            // Json{nullptr} is an array holding null, not null, and
            // the difference only shows up on the wire.
            Json answer = Json::object();
            answer["source"] = *source ? Json(protocol::encode_raw_path(**source)) : Json(nullptr);
            return answer;
        });
}

} // namespace trackknife::engine
