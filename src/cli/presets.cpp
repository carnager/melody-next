// SPDX-License-Identifier: GPL-3.0-only

#include "presets.hpp"

#include <sqlite3.h>

#include <memory>
#include <string_view>
#include <utility>

namespace trackknife::cli {
namespace {

[[nodiscard]] core::Error failure(const core::ErrorCode code, std::string message,
                                  const std::filesystem::path& path) {
    return core::Error{.code = code,
                       .message = std::move(message),
                       .context = {{.key = "path", .value = path.string()}}};
}

[[nodiscard]] std::string column(sqlite3_stmt* statement, const int index) {
    const auto* bytes = static_cast<const char*>(sqlite3_column_blob(statement, index));
    const auto size = sqlite3_column_bytes(statement, index);
    return bytes == nullptr || size <= 0 ? std::string{}
                                         : std::string{bytes, static_cast<std::size_t>(size)};
}

} // namespace

core::Result<std::vector<NamingLayout>> read_naming_layouts(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return std::unexpected(
            failure(core::ErrorCode::not_found, "Melody keeps no presets here", path));
    }
    sqlite3* opened = nullptr;
    const int status =
        sqlite3_open_v2(path.c_str(), &opened, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database{opened, &sqlite3_close};
    if (status != SQLITE_OK) {
        return std::unexpected(
            failure(core::ErrorCode::database,
                    std::string{"cannot open Melody's presets: "} + sqlite3_errstr(status), path));
    }
    // Melody may be saving: wait for it a moment rather than fail.
    sqlite3_busy_timeout(database.get(), 2'000);
    constexpr std::string_view query =
        "SELECT name, relative_directory_expression, basename_expression "
        "FROM output_layout_profiles ORDER BY name, id";
    sqlite3_stmt* prepared = nullptr;
    if (sqlite3_prepare_v2(database.get(), query.data(), static_cast<int>(query.size()), &prepared,
                           nullptr) != SQLITE_OK) {
        return std::unexpected(failure(core::ErrorCode::database,
                                       std::string{"cannot read Melody's naming layouts: "} +
                                           sqlite3_errmsg(database.get()),
                                       path));
    }
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement{prepared,
                                                                               &sqlite3_finalize};
    std::vector<NamingLayout> layouts;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
        layouts.push_back(NamingLayout{.name = column(statement.get(), 0),
                                       .directory = column(statement.get(), 1),
                                       .basename = column(statement.get(), 2)});
    }
    if (step != SQLITE_DONE) {
        return std::unexpected(failure(core::ErrorCode::database,
                                       std::string{"cannot read Melody's naming layouts: "} +
                                           sqlite3_errmsg(database.get()),
                                       path));
    }
    return layouts;
}

} // namespace trackknife::cli
