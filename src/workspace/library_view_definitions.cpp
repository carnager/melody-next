// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/library_view_definitions.hpp"

#include "trackknife/titleformat/compiler.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

#include <algorithm>
#include <iterator>

namespace trackknife::bench {
namespace {

const QString settings_key = QStringLiteral("library/views");

[[nodiscard]] core::Error invalid(const QString& message) {
    return core::Error{
        .code = core::ErrorCode::invalid_argument, .message = message.toStdString(), .context = {}};
}

[[nodiscard]] persistence::LibraryViewLevel level(const char* format, const char* sort = "",
                                                  const bool descending = false) {
    return {.format = format, .sort = sort, .descending = descending};
}

} // namespace

QString libraryViewLevelError(const QString& source) {
    if (source.trimmed().isEmpty()) {
        return QStringLiteral("A level needs an expression");
    }
    const auto compiled = titleformat::compile(
        source.toStdString(), {.context = titleformat::FormatContextKind::tree_level,
                               .dialect = {},
                               .parse_options = {}});
    if (compiled.isValid()) {
        return {};
    }
    auto message = QString::fromStdString(!compiled.parse_diagnostics.empty()
                                              ? compiled.parse_diagnostics.front().message
                                              : compiled.diagnostics.front().message);
    // The usual stumble: text in parentheses, which tkfmt-1 reads as syntax.
    if (message.contains(QStringLiteral("parenthesis")) ||
        message.contains(QStringLiteral("comma"))) {
        message += QStringLiteral(" — write a literal ( ) or , as \\( \\) or \\,");
    }
    return message;
}

std::vector<LibraryViewDefinition> builtinLibraryViews() {
    return {
        {.id = default_library_view_id,
         .name = QStringLiteral("Artist › Album"),
         .levels = {level("%albumartist%"), level("%album%", "%date%")},
         .builtin = true,
         .own_tree = true},
        {.id = recent_library_view_id,
         .name = QStringLiteral("Recently added"),
         .levels = {level("%albumartist% — %album%", "$info(albumdayssinceadded)")},
         .builtin = true,
         .own_tree = true},
        {.id = folders_library_view_id,
         .name = QStringLiteral("Folders"),
         .levels = {},
         .builtin = true,
         .own_tree = true},
        {.id = QStringLiteral("genre-artist-album"),
         .name = QStringLiteral("Genre › Artist › Album"),
         .levels = {level("$each(genre)"), level("%albumartist%"), level("%album%", "%date%")},
         .builtin = true},
        {.id = QStringLiteral("year-album"),
         .name = QStringLiteral("Year › Album"),
         .levels = {level("$left(%date%,4)", "", true), level("%albumartist% — %album%")},
         .builtin = true},
        {.id = QStringLiteral("decade-artist-album"),
         .name = QStringLiteral("Decade › Artist › Album"),
         .levels = {level("$if(%date%,$left(%date%,3)0s,)", "", true), level("%albumartist%"),
                    level("%album%", "%date%")},
         .builtin = true},
        {.id = QStringLiteral("album"),
         .name = QStringLiteral("Album"),
         .levels = {level("%album%")},
         .builtin = true},
    };
}

core::Result<std::vector<LibraryViewDefinition>> customLibraryViews() {
    std::vector<LibraryViewDefinition> views;
    const auto bytes = QSettings{}.value(settings_key).toByteArray();
    if (bytes.isEmpty()) {
        return views;
    }
    const auto document = QJsonDocument::fromJson(bytes);
    if (!document.isObject() || document.object().value(QStringLiteral("version")).toInt() != 1 ||
        !document.object().value(QStringLiteral("views")).isArray()) {
        return std::unexpected(invalid(QStringLiteral("Cannot read this library views version")));
    }
    for (const auto& value : document.object().value(QStringLiteral("views")).toArray()) {
        const auto object = value.toObject();
        LibraryViewDefinition view{.id = object.value(QStringLiteral("id")).toString(),
                                   .name = object.value(QStringLiteral("name")).toString(),
                                   .levels = {},
                                   .builtin = false};
        for (const auto& stored : object.value(QStringLiteral("levels")).toArray()) {
            const auto entry = stored.toObject();
            // A level written in another dialect is not reinterpreted as
            // this one; the view is left out until it can be read.
            if (entry.value(QStringLiteral("dialect")).toString() != QStringLiteral("tkfmt") ||
                entry.value(QStringLiteral("dialect_version")).toInt() != 1) {
                view.levels.clear();
                break;
            }
            view.levels.push_back(
                {.format = entry.value(QStringLiteral("format")).toString().toStdString(),
                 .sort = entry.value(QStringLiteral("sort")).toString().toStdString(),
                 .descending = entry.value(QStringLiteral("descending")).toBool()});
        }
        if (!view.id.isEmpty() && !view.name.trimmed().isEmpty() && !view.levels.empty()) {
            views.push_back(std::move(view));
        }
    }
    return views;
}

std::vector<LibraryViewDefinition> libraryViews() {
    auto views = builtinLibraryViews();
    if (auto custom = customLibraryViews()) {
        std::ranges::move(*custom, std::back_inserter(views));
    }
    return views;
}

core::Result<void> saveCustomLibraryViews(const std::vector<LibraryViewDefinition>& views) {
    QJsonArray stored;
    for (const auto& view : views) {
        if (view.builtin) {
            continue;
        }
        if (view.id.isEmpty() || view.name.trimmed().isEmpty() || view.levels.empty()) {
            return std::unexpected(invalid(QStringLiteral("A view needs a name and a level")));
        }
        QJsonArray levels;
        for (const auto& item : view.levels) {
            auto error = libraryViewLevelError(QString::fromStdString(item.format));
            if (error.isEmpty() && !item.sort.empty()) {
                error = libraryViewLevelError(QString::fromStdString(item.sort));
            }
            if (!error.isEmpty()) {
                return std::unexpected(invalid(error));
            }
            levels.append(
                QJsonObject{{QStringLiteral("format"), QString::fromStdString(item.format)},
                            {QStringLiteral("sort"), QString::fromStdString(item.sort)},
                            {QStringLiteral("descending"), item.descending},
                            {QStringLiteral("dialect"), QStringLiteral("tkfmt")},
                            {QStringLiteral("dialect_version"), 1}});
        }
        stored.append(QJsonObject{{QStringLiteral("id"), view.id},
                                  {QStringLiteral("name"), view.name.trimmed()},
                                  {QStringLiteral("levels"), levels}});
    }
    QSettings{}.setValue(settings_key, QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                                                 {QStringLiteral("views"), stored}})
                                           .toJson(QJsonDocument::Compact));
    emit LibraryViewCatalog::instance().changed();
    return {};
}

LibraryViewCatalog& LibraryViewCatalog::instance() {
    static LibraryViewCatalog catalog;
    return catalog;
}

} // namespace trackknife::bench
