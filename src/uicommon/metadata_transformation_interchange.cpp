// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/metadata_transformation_interchange.hpp"

#include "trackknife/core/unicode.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSaveFile>
#include <QSet>
#include <QString>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace trackknife::ui {
namespace {

constexpr auto interchange_format = "trackknife-metadata-transformation-chain";
// Exports before the application reclaimed its original name used this id.
constexpr auto legacy_interchange_format = "trackbench-metadata-transformation-chain";
constexpr std::uint32_t interchange_version = 1U;

[[nodiscard]] core::Error interchangeError(const core::ErrorCode code, std::string message,
                                           const std::string_view location = {}) {
    core::Error error{.code = code, .message = std::move(message), .context = {}};
    if (!location.empty()) {
        error.context.push_back({"location", std::string{location}});
    }
    return error;
}

[[nodiscard]] QString jsonString(const std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

[[nodiscard]] std::string exactUtf8(const QString& value) {
    const auto bytes = value.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

[[nodiscard]] core::Result<void> addTextBytes(std::size_t& total, const std::string_view value) {
    const auto maximum =
        static_cast<std::size_t>(metadata_transformation_interchange_maximum_bytes);
    if (value.size() > maximum - total) {
        return std::unexpected(
            interchangeError(core::ErrorCode::limit_exceeded,
                             "Native tagging-script JSON exceeds the 8 MiB interchange limit"));
    }
    total += value.size();
    return {};
}

[[nodiscard]] core::Result<void>
validateSerializedTextBudget(const metadata::MetadataTransformationChain& chain) {
    std::size_t total = 0U;
    if (auto added = addTextBytes(total, chain.name); !added) {
        return added;
    }
    for (const auto& action : chain.actions) {
        const auto added = std::visit(
            [&total](const auto& typed) -> core::Result<void> {
                using Action = std::decay_t<decltype(typed)>;
                const auto add = [&total](const std::string_view value) {
                    return addTextBytes(total, value);
                };
                if constexpr (!std::is_same_v<Action, metadata::MetadataCaptureValuesAction> &&
                              !std::is_same_v<Action, metadata::MetadataBlocklistFieldsAction> &&
                              !std::is_same_v<Action, metadata::MetadataAllowlistFieldsAction>) {
                    if (auto target = add(typed.target_field); !target) {
                        return target;
                    }
                }
                if constexpr (std::is_same_v<Action, metadata::MetadataSetValuesAction> ||
                              std::is_same_v<Action, metadata::MetadataAddValuesAction>) {
                    for (const auto& value : typed.values) {
                        if (auto item = add(value); !item) {
                            return item;
                        }
                    }
                } else if constexpr (std::is_same_v<Action,
                                                    metadata::MetadataRemoveFieldIfAction>) {
                    if (auto dialect = add(typed.dialect.dialect); !dialect) {
                        return dialect;
                    }
                    return add(typed.condition);
                } else if constexpr (std::is_same_v<Action, metadata::MetadataFormatValueAction>) {
                    if (auto dialect = add(typed.dialect.dialect); !dialect) {
                        return dialect;
                    }
                    return add(typed.source);
                } else if constexpr (std::is_same_v<Action, metadata::MetadataCopyFieldAction>) {
                    return add(typed.source_field);
                } else if constexpr (std::is_same_v<Action, metadata::MetadataSplitValuesAction> ||
                                     std::is_same_v<Action, metadata::MetadataJoinValuesAction>) {
                    return add(typed.separator);
                } else if constexpr (std::is_same_v<Action,
                                                    metadata::MetadataRemoveMatchingValuesAction>) {
                    return add(typed.match);
                } else if constexpr (std::is_same_v<
                                         Action, metadata::MetadataReplaceMatchingValuesAction>) {
                    if (auto match = add(typed.match); !match) {
                        return match;
                    }
                    for (const auto& value : typed.replacement_values) {
                        if (auto item = add(value); !item) {
                            return item;
                        }
                    }
                } else if constexpr (std::is_same_v<Action,
                                                    metadata::MetadataCaptureValuesAction>) {
                    if (auto dialect = add(typed.dialect.dialect); !dialect) {
                        return dialect;
                    }
                    if (auto source = add(typed.source); !source) {
                        return source;
                    }
                    return add(typed.pattern);
                } else if constexpr (std::is_same_v<Action,
                                                    metadata::MetadataBlocklistFieldsAction> ||
                                     std::is_same_v<Action,
                                                    metadata::MetadataAllowlistFieldsAction>) {
                    for (const auto& field : typed.fields) {
                        if (auto item = add(field); !item) {
                            return item;
                        }
                    }
                }
                return {};
            },
            action);
        if (!added) {
            return added;
        }
    }
    return {};
}

[[nodiscard]] core::Result<void> requireExactKeys(const QJsonObject& object,
                                                  const std::initializer_list<const char*> keys,
                                                  const std::string_view location) {
    QSet<QString> expected;
    expected.reserve(static_cast<qsizetype>(keys.size()));
    for (const auto* key : keys) {
        expected.insert(QString::fromLatin1(key));
    }
    const auto actual_keys = object.keys();
    const QSet<QString> actual{actual_keys.begin(), actual_keys.end()};
    if (actual != expected) {
        return std::unexpected(interchangeError(
            core::ErrorCode::invalid_argument,
            "Native tagging-script JSON contains missing or unexpected fields", location));
    }
    return {};
}

[[nodiscard]] core::Result<std::string> readString(const QJsonObject& object, const char* key,
                                                   const std::string_view location) {
    const auto value = object.value(QString::fromLatin1(key));
    if (!value.isString()) {
        return std::unexpected(interchangeError(core::ErrorCode::invalid_argument,
                                                "Native tagging-script field must be a string",
                                                location));
    }
    return exactUtf8(value.toString());
}

[[nodiscard]] core::Result<std::uint32_t> readUnsigned(const QJsonObject& object, const char* key,
                                                       const std::string_view location) {
    const auto value = object.value(QString::fromLatin1(key));
    if (!value.isDouble()) {
        return std::unexpected(interchangeError(core::ErrorCode::invalid_argument,
                                                "Native tagging-script field must be an integer",
                                                location));
    }
    const auto number = value.toDouble();
    if (!std::isfinite(number) || number < 0.0 ||
        number > static_cast<double>(std::numeric_limits<std::uint32_t>::max()) ||
        std::trunc(number) != number) {
        return std::unexpected(interchangeError(
            core::ErrorCode::invalid_argument,
            "Native tagging-script integer is outside the supported range", location));
    }
    return static_cast<std::uint32_t>(number);
}

[[nodiscard]] QJsonArray valuesToJson(const std::vector<std::string>& values) {
    QJsonArray result;
    for (const auto& value : values) {
        result.push_back(jsonString(value));
    }
    return result;
}

[[nodiscard]] core::Result<std::vector<std::string>>
readValues(const QJsonObject& object, const char* key, const std::string_view location) {
    const auto value = object.value(QString::fromLatin1(key));
    if (!value.isArray()) {
        return std::unexpected(
            interchangeError(core::ErrorCode::invalid_argument,
                             "Native tagging-script exact values must be a JSON array", location));
    }
    const auto array = value.toArray();
    std::vector<std::string> result;
    result.reserve(static_cast<std::size_t>(array.size()));
    for (const auto& entry : array) {
        if (!entry.isString()) {
            return std::unexpected(interchangeError(
                core::ErrorCode::invalid_argument,
                "Native tagging-script exact values must all be strings", location));
        }
        result.push_back(exactUtf8(entry.toString()));
    }
    return result;
}

[[nodiscard]] QString matchModeName(const metadata::MetadataFieldMatchMode mode) {
    switch (mode) {
    case metadata::MetadataFieldMatchMode::logical:
        return QStringLiteral("logical");
    case metadata::MetadataFieldMatchMode::exact_native:
        return QStringLiteral("exact_native");
    }
    return {};
}

[[nodiscard]] core::Result<metadata::MetadataFieldMatchMode>
readMatchMode(const QJsonObject& object, const std::string_view location) {
    const auto name = readString(object, "match_mode", location);
    if (!name) {
        return std::unexpected(name.error());
    }
    if (*name == "logical") {
        return metadata::MetadataFieldMatchMode::logical;
    }
    if (*name == "exact_native") {
        return metadata::MetadataFieldMatchMode::exact_native;
    }
    return std::unexpected(interchangeError(
        core::ErrorCode::unsupported, "Native tagging-script match mode is unsupported", location));
}

template <typename Dialect> [[nodiscard]] QJsonObject dialectToJson(const Dialect& dialect) {
    return QJsonObject{
        {QStringLiteral("compiler_schema"), static_cast<qint64>(dialect.compiler_schema)},
        {QStringLiteral("dialect"), jsonString(dialect.dialect)},
        {QStringLiteral("dialect_version"), static_cast<qint64>(dialect.dialect_version)},
    };
}

template <typename Dialect>
[[nodiscard]] core::Result<Dialect> readDialect(const QJsonObject& object,
                                                const std::string_view location) {
    const auto value = object.value(QStringLiteral("dialect"));
    if (!value.isObject()) {
        return std::unexpected(
            interchangeError(core::ErrorCode::invalid_argument,
                             "Native tagging-script dialect identity must be an object", location));
    }
    const auto dialect_object = value.toObject();
    if (auto keys = requireExactKeys(dialect_object,
                                     {"compiler_schema", "dialect", "dialect_version"}, location);
        !keys) {
        return std::unexpected(keys.error());
    }
    auto name = readString(dialect_object, "dialect", location);
    auto version = readUnsigned(dialect_object, "dialect_version", location);
    auto compiler = readUnsigned(dialect_object, "compiler_schema", location);
    if (!name) {
        return std::unexpected(name.error());
    }
    if (!version) {
        return std::unexpected(version.error());
    }
    if (!compiler) {
        return std::unexpected(compiler.error());
    }
    return Dialect{
        .dialect = std::move(*name), .dialect_version = *version, .compiler_schema = *compiler};
}

[[nodiscard]] QString transformName(const metadata::MetadataValueTransformKind transform) {
    switch (transform) {
    case metadata::MetadataValueTransformKind::trim_ascii:
        return QStringLiteral("trim_ascii");
    case metadata::MetadataValueTransformKind::lowercase:
        return QStringLiteral("lowercase");
    case metadata::MetadataValueTransformKind::uppercase:
        return QStringLiteral("uppercase");
    case metadata::MetadataValueTransformKind::capitalize_first:
        return QStringLiteral("capitalize_first");
    }
    return {};
}

[[nodiscard]] core::Result<metadata::MetadataValueTransformKind>
readTransform(const QJsonObject& object, const std::string_view location) {
    auto name = readString(object, "transform", location);
    if (!name) {
        return std::unexpected(name.error());
    }
    if (*name == "trim_ascii") {
        return metadata::MetadataValueTransformKind::trim_ascii;
    }
    if (*name == "lowercase") {
        return metadata::MetadataValueTransformKind::lowercase;
    }
    if (*name == "uppercase") {
        return metadata::MetadataValueTransformKind::uppercase;
    }
    if (*name == "capitalize_first") {
        return metadata::MetadataValueTransformKind::capitalize_first;
    }
    return std::unexpected(interchangeError(core::ErrorCode::unsupported,
                                            "Native tagging-script value transform is unsupported",
                                            location));
}

[[nodiscard]] QString captureSourceName(const metadata::MetadataCaptureSourceKind kind) {
    switch (kind) {
    case metadata::MetadataCaptureSourceKind::filename:
        return QStringLiteral("filename");
    case metadata::MetadataCaptureSourceKind::full_path:
        return QStringLiteral("full_path");
    case metadata::MetadataCaptureSourceKind::formatted:
        return QStringLiteral("formatted");
    case metadata::MetadataCaptureSourceKind::field:
        return QStringLiteral("field");
    }
    return {};
}

[[nodiscard]] core::Result<metadata::MetadataCaptureSourceKind>
readCaptureSource(const QJsonObject& object, const std::string_view location) {
    auto name = readString(object, "source_kind", location);
    if (!name) {
        return std::unexpected(name.error());
    }
    if (*name == "filename") {
        return metadata::MetadataCaptureSourceKind::filename;
    }
    if (*name == "full_path") {
        return metadata::MetadataCaptureSourceKind::full_path;
    }
    if (*name == "formatted") {
        return metadata::MetadataCaptureSourceKind::formatted;
    }
    if (*name == "field") {
        return metadata::MetadataCaptureSourceKind::field;
    }
    return std::unexpected(interchangeError(core::ErrorCode::unsupported,
                                            "Native tagging-script capture source is unsupported",
                                            location));
}

[[nodiscard]] QJsonObject actionToJson(const metadata::MetadataTransformationAction& action) {
    return std::visit(
        [](const auto& typed) -> QJsonObject {
            using Action = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Action, metadata::MetadataSetValuesAction>) {
                return {{QStringLiteral("kind"), QStringLiteral("set_values")},
                        {QStringLiteral("target_field"), jsonString(typed.target_field)},
                        {QStringLiteral("values"), valuesToJson(typed.values)}};
            } else if constexpr (std::is_same_v<Action, metadata::MetadataAddValuesAction>) {
                return {{QStringLiteral("kind"), QStringLiteral("add_values")},
                        {QStringLiteral("target_field"), jsonString(typed.target_field)},
                        {QStringLiteral("values"), valuesToJson(typed.values)}};
            } else if constexpr (std::is_same_v<Action, metadata::MetadataRemoveFieldAction>) {
                return {{QStringLiteral("kind"), QStringLiteral("remove_field")},
                        {QStringLiteral("match_mode"), matchModeName(typed.match_mode)},
                        {QStringLiteral("target_field"), jsonString(typed.target_field)}};
            } else if constexpr (std::is_same_v<Action, metadata::MetadataRemoveFieldIfAction>) {
                return {{QStringLiteral("condition"), jsonString(typed.condition)},
                        {QStringLiteral("dialect"), dialectToJson(typed.dialect)},
                        {QStringLiteral("kind"), QStringLiteral("remove_field_if")},
                        {QStringLiteral("match_mode"), matchModeName(typed.match_mode)},
                        {QStringLiteral("target_field"), jsonString(typed.target_field)}};
            } else if constexpr (std::is_same_v<Action, metadata::MetadataTransformValuesAction>) {
                return {{QStringLiteral("kind"), QStringLiteral("transform_values")},
                        {QStringLiteral("target_field"), jsonString(typed.target_field)},
                        {QStringLiteral("transform"), transformName(typed.transform)}};
            } else if constexpr (std::is_same_v<Action, metadata::MetadataFormatValueAction>) {
                return {{QStringLiteral("dialect"), dialectToJson(typed.dialect)},
                        {QStringLiteral("kind"), QStringLiteral("format_value")},
                        {QStringLiteral("source"), jsonString(typed.source)},
                        {QStringLiteral("target_field"), jsonString(typed.target_field)}};
            } else if constexpr (std::is_same_v<Action, metadata::MetadataCopyFieldAction>) {
                return {{QStringLiteral("kind"), QStringLiteral("copy_field")},
                        {QStringLiteral("source_field"), jsonString(typed.source_field)},
                        {QStringLiteral("target_field"), jsonString(typed.target_field)}};
            } else if constexpr (std::is_same_v<Action, metadata::MetadataSplitValuesAction>) {
                return {{QStringLiteral("kind"), QStringLiteral("split_values")},
                        {QStringLiteral("separator"), jsonString(typed.separator)},
                        {QStringLiteral("target_field"), jsonString(typed.target_field)}};
            } else if constexpr (std::is_same_v<Action, metadata::MetadataJoinValuesAction>) {
                return {{QStringLiteral("kind"), QStringLiteral("join_values")},
                        {QStringLiteral("separator"), jsonString(typed.separator)},
                        {QStringLiteral("target_field"), jsonString(typed.target_field)}};
            } else if constexpr (std::is_same_v<Action,
                                                metadata::MetadataRemoveMatchingValuesAction>) {
                return {{QStringLiteral("kind"), QStringLiteral("remove_matching_values")},
                        {QStringLiteral("match"), jsonString(typed.match)},
                        {QStringLiteral("target_field"), jsonString(typed.target_field)}};
            } else if constexpr (std::is_same_v<Action,
                                                metadata::MetadataReplaceMatchingValuesAction>) {
                return {
                    {QStringLiteral("kind"), QStringLiteral("replace_matching_values")},
                    {QStringLiteral("match"), jsonString(typed.match)},
                    {QStringLiteral("replacement_values"), valuesToJson(typed.replacement_values)},
                    {QStringLiteral("target_field"), jsonString(typed.target_field)}};
            } else if constexpr (std::is_same_v<Action,
                                                metadata::MetadataNumberSelectedItemsAction>) {
                return {{QStringLiteral("kind"), QStringLiteral("number_selected_items")},
                        {QStringLiteral("padding"), static_cast<qint64>(typed.padding)},
                        {QStringLiteral("start"), static_cast<qint64>(typed.start)},
                        {QStringLiteral("target_field"), jsonString(typed.target_field)}};
            } else if constexpr (std::is_same_v<Action,
                                                metadata::MetadataNumberGroupedItemsAction>) {
                return {{QStringLiteral("dialect"), dialectToJson(typed.dialect)},
                        {QStringLiteral("group_expression"), jsonString(typed.group_expression)},
                        {QStringLiteral("kind"), QStringLiteral("number_grouped_items")},
                        {QStringLiteral("padding"), static_cast<qint64>(typed.padding)},
                        {QStringLiteral("start"), static_cast<qint64>(typed.start)},
                        {QStringLiteral("target_field"), jsonString(typed.target_field)}};
            } else if constexpr (std::is_same_v<Action,
                                                metadata::MetadataKeepFirstCharactersAction>) {
                return {
                    {QStringLiteral("character_count"), static_cast<qint64>(typed.character_count)},
                    {QStringLiteral("kind"), QStringLiteral("keep_first_characters")},
                    {QStringLiteral("target_field"), jsonString(typed.target_field)}};
            } else if constexpr (std::is_same_v<Action, metadata::MetadataCaptureValuesAction>) {
                return {{QStringLiteral("dialect"), dialectToJson(typed.dialect)},
                        {QStringLiteral("kind"), QStringLiteral("capture_values")},
                        {QStringLiteral("pattern"), jsonString(typed.pattern)},
                        {QStringLiteral("source"), jsonString(typed.source)},
                        {QStringLiteral("source_kind"), captureSourceName(typed.source_kind)}};
            } else if constexpr (std::is_same_v<Action, metadata::MetadataBlocklistFieldsAction>) {
                return {{QStringLiteral("fields"), valuesToJson(typed.fields)},
                        {QStringLiteral("kind"), QStringLiteral("blocklist_fields")}};
            } else if constexpr (std::is_same_v<Action, metadata::MetadataAllowlistFieldsAction>) {
                return {{QStringLiteral("fields"), valuesToJson(typed.fields)},
                        {QStringLiteral("kind"), QStringLiteral("allowlist_fields")}};
            }
            return {};
        },
        action);
}

[[nodiscard]] core::Result<metadata::MetadataTransformationAction>
readAction(const QJsonValue& value, const std::size_t index) {
    const auto location = "actions[" + std::to_string(index) + "]";
    if (!value.isObject()) {
        return std::unexpected(interchangeError(core::ErrorCode::invalid_argument,
                                                "Native tagging-script action must be an object",
                                                location));
    }
    const auto object = value.toObject();
    auto kind = readString(object, "kind", location);
    if (!kind) {
        return std::unexpected(kind.error());
    }
    if (*kind == "set_values" || *kind == "add_values") {
        if (auto keys = requireExactKeys(object, {"kind", "target_field", "values"}, location);
            !keys) {
            return std::unexpected(keys.error());
        }
        auto target = readString(object, "target_field", location);
        auto values = readValues(object, "values", location);
        if (!target) {
            return std::unexpected(target.error());
        }
        if (!values) {
            return std::unexpected(values.error());
        }
        if (*kind == "set_values") {
            return metadata::MetadataSetValuesAction{.target_field = std::move(*target),
                                                     .values = std::move(*values)};
        }
        return metadata::MetadataAddValuesAction{.target_field = std::move(*target),
                                                 .values = std::move(*values)};
    }
    if (*kind == "remove_field") {
        if (auto keys = requireExactKeys(object, {"kind", "match_mode", "target_field"}, location);
            !keys) {
            return std::unexpected(keys.error());
        }
        auto target = readString(object, "target_field", location);
        auto mode = readMatchMode(object, location);
        if (!target) {
            return std::unexpected(target.error());
        }
        if (!mode) {
            return std::unexpected(mode.error());
        }
        return metadata::MetadataRemoveFieldAction{.target_field = std::move(*target),
                                                   .match_mode = *mode};
    }
    if (*kind == "blocklist_fields" || *kind == "allowlist_fields") {
        if (auto keys = requireExactKeys(object, {"fields", "kind"}, location); !keys) {
            return std::unexpected(keys.error());
        }
        auto fields = readValues(object, "fields", location);
        if (!fields) {
            return std::unexpected(fields.error());
        }
        if (*kind == "blocklist_fields") {
            return metadata::MetadataBlocklistFieldsAction{.fields = std::move(*fields)};
        }
        return metadata::MetadataAllowlistFieldsAction{.fields = std::move(*fields)};
    }
    if (*kind == "remove_field_if") {
        if (auto keys = requireExactKeys(
                object, {"condition", "dialect", "kind", "match_mode", "target_field"}, location);
            !keys) {
            return std::unexpected(keys.error());
        }
        auto target = readString(object, "target_field", location);
        auto condition = readString(object, "condition", location);
        auto dialect = readDialect<titleformat::DialectVersion>(object, location);
        auto mode = readMatchMode(object, location);
        if (!target) {
            return std::unexpected(target.error());
        }
        if (!condition) {
            return std::unexpected(condition.error());
        }
        if (!dialect) {
            return std::unexpected(dialect.error());
        }
        if (!mode) {
            return std::unexpected(mode.error());
        }
        return metadata::MetadataRemoveFieldIfAction{
            .target_field = std::move(*target),
            .dialect = std::move(*dialect),
            .condition = std::move(*condition),
            .match_mode = *mode,
        };
    }
    if (*kind == "transform_values") {
        if (auto keys = requireExactKeys(object, {"kind", "target_field", "transform"}, location);
            !keys) {
            return std::unexpected(keys.error());
        }
        auto target = readString(object, "target_field", location);
        auto transform = readTransform(object, location);
        if (!target) {
            return std::unexpected(target.error());
        }
        if (!transform) {
            return std::unexpected(transform.error());
        }
        return metadata::MetadataTransformValuesAction{.target_field = std::move(*target),
                                                       .transform = *transform};
    }
    if (*kind == "format_value") {
        if (auto keys =
                requireExactKeys(object, {"dialect", "kind", "source", "target_field"}, location);
            !keys) {
            return std::unexpected(keys.error());
        }
        auto target = readString(object, "target_field", location);
        auto source = readString(object, "source", location);
        auto dialect = readDialect<titleformat::DialectVersion>(object, location);
        if (!target) {
            return std::unexpected(target.error());
        }
        if (!source) {
            return std::unexpected(source.error());
        }
        if (!dialect) {
            return std::unexpected(dialect.error());
        }
        return metadata::MetadataFormatValueAction{.target_field = std::move(*target),
                                                   .dialect = std::move(*dialect),
                                                   .source = std::move(*source)};
    }
    if (*kind == "copy_field") {
        if (auto keys =
                requireExactKeys(object, {"kind", "source_field", "target_field"}, location);
            !keys) {
            return std::unexpected(keys.error());
        }
        auto target = readString(object, "target_field", location);
        auto source = readString(object, "source_field", location);
        if (!target) {
            return std::unexpected(target.error());
        }
        if (!source) {
            return std::unexpected(source.error());
        }
        return metadata::MetadataCopyFieldAction{.target_field = std::move(*target),
                                                 .source_field = std::move(*source)};
    }
    if (*kind == "split_values" || *kind == "join_values") {
        if (auto keys = requireExactKeys(object, {"kind", "separator", "target_field"}, location);
            !keys) {
            return std::unexpected(keys.error());
        }
        auto target = readString(object, "target_field", location);
        auto separator = readString(object, "separator", location);
        if (!target) {
            return std::unexpected(target.error());
        }
        if (!separator) {
            return std::unexpected(separator.error());
        }
        if (*kind == "split_values") {
            return metadata::MetadataSplitValuesAction{.target_field = std::move(*target),
                                                       .separator = std::move(*separator)};
        }
        return metadata::MetadataJoinValuesAction{.target_field = std::move(*target),
                                                  .separator = std::move(*separator)};
    }
    if (*kind == "remove_matching_values") {
        if (auto keys = requireExactKeys(object, {"kind", "match", "target_field"}, location);
            !keys) {
            return std::unexpected(keys.error());
        }
        auto target = readString(object, "target_field", location);
        auto match = readString(object, "match", location);
        if (!target) {
            return std::unexpected(target.error());
        }
        if (!match) {
            return std::unexpected(match.error());
        }
        return metadata::MetadataRemoveMatchingValuesAction{.target_field = std::move(*target),
                                                            .match = std::move(*match)};
    }
    if (*kind == "replace_matching_values") {
        if (auto keys = requireExactKeys(
                object, {"kind", "match", "replacement_values", "target_field"}, location);
            !keys) {
            return std::unexpected(keys.error());
        }
        auto target = readString(object, "target_field", location);
        auto match = readString(object, "match", location);
        auto replacement = readValues(object, "replacement_values", location);
        if (!target) {
            return std::unexpected(target.error());
        }
        if (!match) {
            return std::unexpected(match.error());
        }
        if (!replacement) {
            return std::unexpected(replacement.error());
        }
        return metadata::MetadataReplaceMatchingValuesAction{
            .target_field = std::move(*target),
            .match = std::move(*match),
            .replacement_values = std::move(*replacement),
        };
    }
    if (*kind == "number_grouped_items") {
        if (auto keys = requireExactKeys(
                object, {"dialect", "group_expression", "kind", "padding", "start", "target_field"},
                location);
            !keys) {
            return std::unexpected(keys.error());
        }
        auto target = readString(object, "target_field", location);
        if (!target) {
            return std::unexpected(target.error());
        }
        auto dialect = readDialect<titleformat::DialectVersion>(object, location);
        if (!dialect) {
            return std::unexpected(dialect.error());
        }
        auto expression = readString(object, "group_expression", location);
        if (!expression) {
            return std::unexpected(expression.error());
        }
        auto start = readUnsigned(object, "start", location);
        if (!start) {
            return std::unexpected(start.error());
        }
        auto padding = readUnsigned(object, "padding", location);
        if (!padding) {
            return std::unexpected(padding.error());
        }
        return metadata::MetadataNumberGroupedItemsAction{
            .target_field = std::move(*target),
            .dialect = std::move(*dialect),
            .group_expression = std::move(*expression),
            .start = *start,
            .padding = *padding,
        };
    }
    if (*kind == "number_selected_items") {
        if (auto keys =
                requireExactKeys(object, {"kind", "padding", "start", "target_field"}, location);
            !keys) {
            return std::unexpected(keys.error());
        }
        auto target = readString(object, "target_field", location);
        auto start = readUnsigned(object, "start", location);
        auto padding = readUnsigned(object, "padding", location);
        if (!target) {
            return std::unexpected(target.error());
        }
        if (!start) {
            return std::unexpected(start.error());
        }
        if (!padding) {
            return std::unexpected(padding.error());
        }
        return metadata::MetadataNumberSelectedItemsAction{
            .target_field = std::move(*target), .start = *start, .padding = *padding};
    }
    if (*kind == "keep_first_characters") {
        if (auto keys =
                requireExactKeys(object, {"character_count", "kind", "target_field"}, location);
            !keys) {
            return std::unexpected(keys.error());
        }
        auto target = readString(object, "target_field", location);
        auto count = readUnsigned(object, "character_count", location);
        if (!target) {
            return std::unexpected(target.error());
        }
        if (!count) {
            return std::unexpected(count.error());
        }
        return metadata::MetadataKeepFirstCharactersAction{.target_field = std::move(*target),
                                                           .character_count = *count};
    }
    if (*kind == "capture_values") {
        if (auto keys = requireExactKeys(
                object, {"dialect", "kind", "pattern", "source", "source_kind"}, location);
            !keys) {
            return std::unexpected(keys.error());
        }
        auto dialect = readDialect<metadata::CapturePatternDialectVersion>(object, location);
        auto source_kind = readCaptureSource(object, location);
        auto source = readString(object, "source", location);
        auto pattern = readString(object, "pattern", location);
        if (!dialect) {
            return std::unexpected(dialect.error());
        }
        if (!source_kind) {
            return std::unexpected(source_kind.error());
        }
        if (!source) {
            return std::unexpected(source.error());
        }
        if (!pattern) {
            return std::unexpected(pattern.error());
        }
        return metadata::MetadataCaptureValuesAction{
            .dialect = std::move(*dialect),
            .source_kind = *source_kind,
            .source = std::move(*source),
            .pattern = std::move(*pattern),
        };
    }
    return std::unexpected(interchangeError(core::ErrorCode::unsupported,
                                            "Native tagging-script action kind is unsupported",
                                            location));
}

} // namespace

core::Result<QByteArray>
serializeMetadataTransformationChain(const metadata::MetadataTransformationChain& chain) {
    if (auto valid = metadata::validate_metadata_transformation_chain(chain); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    if (auto bounded = validateSerializedTextBudget(chain); !bounded) {
        return std::unexpected(std::move(bounded.error()));
    }
    QJsonArray actions;
    for (const auto& action : chain.actions) {
        actions.push_back(actionToJson(action));
    }
    auto bytes = QJsonDocument{
        QJsonObject{
            {QStringLiteral("chain"),
             QJsonObject{
                 {QStringLiteral("actions"), actions},
                 {QStringLiteral("name"), jsonString(chain.name)},
                 {QStringLiteral("schema_version"), static_cast<qint64>(chain.schema_version)},
             }},
            {QStringLiteral("format"), QString::fromLatin1(interchange_format)},
            {QStringLiteral("format_version"), static_cast<qint64>(interchange_version)},
        }}.toJson(QJsonDocument::Indented);
    if (bytes.size() > metadata_transformation_interchange_maximum_bytes) {
        return std::unexpected(
            interchangeError(core::ErrorCode::limit_exceeded,
                             "Native tagging-script JSON exceeds the 8 MiB interchange limit"));
    }
    return bytes;
}

core::Result<metadata::MetadataTransformationChain>
deserializeMetadataTransformationChain(const QByteArray& bytes) {
    if (bytes.isEmpty()) {
        return std::unexpected(interchangeError(core::ErrorCode::invalid_argument,
                                                "Native tagging-script file is empty"));
    }
    if (bytes.size() > metadata_transformation_interchange_maximum_bytes) {
        return std::unexpected(
            interchangeError(core::ErrorCode::limit_exceeded,
                             "Native tagging-script JSON exceeds the 8 MiB interchange limit"));
    }
    const auto utf8 = std::string_view{bytes.constData(), static_cast<std::size_t>(bytes.size())};
    if (auto valid_utf8 = core::unicodeCodePointCount(utf8); !valid_utf8) {
        return std::unexpected(interchangeError(core::ErrorCode::invalid_argument,
                                                "Native tagging-script file is not valid UTF-8"));
    }

    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(bytes, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::unexpected(
            interchangeError(core::ErrorCode::invalid_argument,
                             "Native tagging-script file is not a valid JSON object",
                             std::to_string(parse_error.offset)));
    }
    const auto envelope = document.object();
    if (auto keys = requireExactKeys(envelope, {"chain", "format", "format_version"}, "root");
        !keys) {
        return std::unexpected(keys.error());
    }
    auto format = readString(envelope, "format", "root.format");
    auto version = readUnsigned(envelope, "format_version", "root.format_version");
    if (!format) {
        return std::unexpected(format.error());
    }
    if (!version) {
        return std::unexpected(version.error());
    }
    if ((*format != interchange_format && *format != legacy_interchange_format) ||
        *version != interchange_version) {
        return std::unexpected(interchangeError(
            core::ErrorCode::unsupported,
            "Native tagging-script format or envelope version is unsupported", "root"));
    }
    const auto chain_value = envelope.value(QStringLiteral("chain"));
    if (!chain_value.isObject()) {
        return std::unexpected(interchangeError(core::ErrorCode::invalid_argument,
                                                "Native tagging-script chain must be an object",
                                                "root.chain"));
    }
    const auto chain_object = chain_value.toObject();
    if (auto keys =
            requireExactKeys(chain_object, {"actions", "name", "schema_version"}, "root.chain");
        !keys) {
        return std::unexpected(keys.error());
    }
    auto schema = readUnsigned(chain_object, "schema_version", "root.chain.schema_version");
    auto name = readString(chain_object, "name", "root.chain.name");
    if (!schema) {
        return std::unexpected(schema.error());
    }
    if (!name) {
        return std::unexpected(name.error());
    }
    const auto actions_value = chain_object.value(QStringLiteral("actions"));
    if (!actions_value.isArray()) {
        return std::unexpected(interchangeError(core::ErrorCode::invalid_argument,
                                                "Native tagging-script actions must be an array",
                                                "root.chain.actions"));
    }
    const auto action_values = actions_value.toArray();
    std::vector<metadata::MetadataTransformationAction> actions;
    actions.reserve(static_cast<std::size_t>(action_values.size()));
    for (qsizetype index = 0; index < action_values.size(); ++index) {
        auto action = readAction(action_values.at(index), static_cast<std::size_t>(index));
        if (!action) {
            return std::unexpected(action.error());
        }
        actions.push_back(std::move(*action));
    }
    metadata::MetadataTransformationChain chain{
        .schema_version = *schema, .name = std::move(*name), .actions = std::move(actions)};
    if (auto valid = metadata::validate_metadata_transformation_chain(chain); !valid) {
        auto error = std::move(valid.error());
        error.message = "Imported native tagging script is invalid: " + error.message;
        return std::unexpected(std::move(error));
    }
    return chain;
}

core::Result<metadata::MetadataTransformationChain>
loadMetadataTransformationChainFile(const QString& path) {
    QFile input{path};
    if (!input.open(QIODevice::ReadOnly)) {
        return std::unexpected(
            interchangeError(core::ErrorCode::io, "Could not open native tagging script: " +
                                                      exactUtf8(input.errorString())));
    }
    if (input.size() > metadata_transformation_interchange_maximum_bytes) {
        return std::unexpected(
            interchangeError(core::ErrorCode::limit_exceeded,
                             "Native tagging-script JSON exceeds the 8 MiB interchange limit"));
    }
    const auto bytes = input.read(metadata_transformation_interchange_maximum_bytes + 1);
    if (input.error() != QFileDevice::NoError) {
        return std::unexpected(
            interchangeError(core::ErrorCode::io, "Could not read native tagging script: " +
                                                      exactUtf8(input.errorString())));
    }
    return deserializeMetadataTransformationChain(bytes);
}

core::Result<void>
saveMetadataTransformationChainFile(const QString& path,
                                    const metadata::MetadataTransformationChain& chain) {
    auto bytes = serializeMetadataTransformationChain(chain);
    if (!bytes) {
        return std::unexpected(std::move(bytes.error()));
    }
    QSaveFile output{path};
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly) || output.write(*bytes) != bytes->size() ||
        !output.commit()) {
        return std::unexpected(interchangeError(
            core::ErrorCode::io, "Could not atomically write native tagging script: " +
                                     exactUtf8(output.errorString())));
    }
    return {};
}

} // namespace trackknife::ui
